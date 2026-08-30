#include "live_binaural_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Keep SAF's complex declaration outside the C linkage opened by some SAF headers.
#include <saf_utility_complex.h>
#include <saf_utility_fft.h>
#include <samplerate.h>

#include <fmt/format.h>

#include "binaural_internal.h"

namespace mradm::live_scene {

namespace {

using binaural_internal::BinauralState;
using binaural_internal::compute_hrtf_into;
using binaural_internal::HrtfDataset;
using binaural_internal::k_n_ears;

constexpr std::uint32_t k_convolution_block = 1024U;
constexpr std::size_t k_diffuse_delay_len = 32U;

// These private renderer-local aggregate workspaces intentionally expose their storage to the
// convolution helpers in this translation unit.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct OlaState {
    explicit OlaState(std::size_t overlap_len = 0U) : left(overlap_len, 0.0F), right(overlap_len, 0.0F) {}

    std::vector<float> left;
    std::vector<float> right;
};

struct DiffuseState {
    std::array<float, k_diffuse_delay_len> delay{};
    std::size_t write_pos{0U};
};

struct RuntimeElement {
    ElementDescriptor descriptor;
    ObjectState current;
    ObjectState target;
    std::uint32_t ramp_remaining{0U};
    bool initialized{false};
    OlaState ola;
    DiffuseState diffuse;
};

struct ConvolutionScratch {
    std::vector<float> fft_input;
    std::vector<float_complex> source_fd;
    std::vector<float_complex> output_fd;
    std::vector<float> fft_output;
    std::vector<float> source_start;
    std::vector<float> source_end;
    std::vector<float> diffuse;
    std::vector<float> left_start;
    std::vector<float> right_start;
    std::vector<float> left_end;
    std::vector<float> right_end;
    std::vector<float_complex> hrtf_start;
    std::vector<float_complex> hrtf_end;
    std::vector<float_complex> hrtf_temp;

    void resize(const BinauralState& state) {
        fft_input.resize(static_cast<std::size_t>(state.fft_size));
        source_fd.resize(static_cast<std::size_t>(state.n_bands));
        output_fd.resize(static_cast<std::size_t>(state.n_bands));
        fft_output.resize(static_cast<std::size_t>(state.fft_size));
        source_start.resize(k_convolution_block);
        source_end.resize(k_convolution_block);
        diffuse.resize(k_convolution_block);
        left_start.resize(k_convolution_block);
        right_start.resize(k_convolution_block);
        left_end.resize(k_convolution_block);
        right_end.resize(k_convolution_block);
    }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] ObjectState default_state(const ElementDescriptor& descriptor) {
    ObjectState state;
    state.valid_fields = k_known_state_fields & ~state_position;
    if (descriptor.role == ElementRole::object || descriptor.has_position) {
        state.valid_fields |= state_position;
    }
    if (descriptor.has_position) {
        state.x = descriptor.x;
        state.y = descriptor.y;
        state.z = descriptor.z;
    }
    return state;
}

void copy_state_fields(ObjectState& destination, const ObjectState& source, std::uint64_t requested_fields) {
    const auto fields = source.valid_fields & requested_fields;
    if ((fields & state_active) != 0U) {
        destination.active = source.active;
    }
    if ((fields & state_linear_gain) != 0U) {
        destination.linear_gain = source.linear_gain;
    }
    if ((fields & state_position) != 0U) {
        destination.x = source.x;
        destination.y = source.y;
        destination.z = source.z;
    }
    if ((fields & state_extent) != 0U) {
        destination.width = source.width;
        destination.height = source.height;
        destination.depth = source.depth;
    }
    if ((fields & state_diffuse) != 0U) {
        destination.diffuse = source.diffuse;
    }
    if ((fields & state_divergence) != 0U) {
        destination.divergence = source.divergence;
    }
    if ((fields & state_channel_lock) != 0U) {
        destination.channel_lock = source.channel_lock;
    }
    if ((fields & state_screen_reference) != 0U) {
        destination.screen_reference = source.screen_reference;
    }
    if ((fields & state_head_locked) != 0U) {
        destination.head_locked = source.head_locked;
    }
    destination.valid_fields |= fields;
}

[[nodiscard]] ObjectState
interpolated_state(const ObjectState& start, const ObjectState& target, float alpha) noexcept {
    const auto lerp = [alpha](float from, float to) { return from + ((to - from) * alpha); };
    ObjectState result = start;
    result.linear_gain = lerp(start.linear_gain, target.linear_gain);
    result.x = lerp(start.x, target.x);
    result.y = lerp(start.y, target.y);
    result.z = lerp(start.z, target.z);
    result.width = lerp(start.width, target.width);
    result.height = lerp(start.height, target.height);
    result.depth = lerp(start.depth, target.depth);
    result.diffuse = lerp(start.diffuse, target.diffuse);
    result.divergence = lerp(start.divergence, target.divergence);
    if (alpha >= 1.0F) {
        result.active = target.active;
        result.channel_lock = target.channel_lock;
        result.screen_reference = target.screen_reference;
        result.head_locked = target.head_locked;
        result.valid_fields = target.valid_fields;
    }
    return result;
}

[[nodiscard]] std::pair<float, float> cartesian_to_polar(float x, float y, float z) noexcept {
    constexpr double k_rad_to_deg = 180.0 / std::numbers::pi_v<double>;
    const double horizontal = std::hypot(static_cast<double>(x), static_cast<double>(y));
    const double azimuth = std::atan2(-static_cast<double>(x), static_cast<double>(y)) * k_rad_to_deg;
    const double elevation = std::atan2(static_cast<double>(z), horizontal) * k_rad_to_deg;
    return {static_cast<float>(azimuth), static_cast<float>(elevation)};
}

[[nodiscard]] std::optional<std::pair<float, float>> label_position(std::string_view label) {
    static const std::unordered_map<std::string_view, std::pair<float, float>> k_positions{
        {"M+000", {0.0F, 0.0F}},     {"M+030", {30.0F, 0.0F}},   {"M-030", {-30.0F, 0.0F}},
        {"M+060", {60.0F, 0.0F}},    {"M-060", {-60.0F, 0.0F}},  {"M+090", {90.0F, 0.0F}},
        {"M-090", {-90.0F, 0.0F}},   {"M+110", {110.0F, 0.0F}},  {"M-110", {-110.0F, 0.0F}},
        {"M+135", {135.0F, 0.0F}},   {"M-135", {-135.0F, 0.0F}}, {"M+180", {180.0F, 0.0F}},
        {"U+000", {0.0F, 30.0F}},    {"U+030", {30.0F, 30.0F}},  {"U-030", {-30.0F, 30.0F}},
        {"U+045", {45.0F, 30.0F}},   {"U-045", {-45.0F, 30.0F}}, {"U+110", {110.0F, 30.0F}},
        {"U-110", {-110.0F, 30.0F}}, {"U+135", {135.0F, 30.0F}}, {"U-135", {-135.0F, 30.0F}},
        {"U+180", {180.0F, 30.0F}},  {"T+000", {0.0F, 90.0F}},   {"B+045", {45.0F, -30.0F}},
        {"B-045", {-45.0F, -30.0F}},
    };
    const auto found = k_positions.find(label);
    if (found == k_positions.end()) {
        return std::nullopt;
    }
    return found->second;
}

[[nodiscard]] Result<HrtfDataset> resample_dataset(HrtfDataset dataset, std::uint32_t sample_rate) {
    if (std::cmp_equal(dataset.sample_rate, sample_rate)) {
        return dataset;
    }
    if (dataset.sample_rate <= 0 || dataset.hrir_len <= 0 || sample_rate == 0U) {
        return make_error(ErrorCode::render_failed, "live binaural HRTF dataset has an invalid sample rate");
    }

    const double ratio = static_cast<double>(sample_rate) / static_cast<double>(dataset.sample_rate);
    const auto output_length = static_cast<int>(std::ceil(static_cast<double>(dataset.hrir_len) * ratio));
    std::vector<float> output(static_cast<std::size_t>(dataset.num_dirs) * k_n_ears *
                              static_cast<std::size_t>(output_length));
    std::vector<float> scratch(static_cast<std::size_t>(output_length) + 64U, 0.0F);
    for (int direction = 0; direction < dataset.num_dirs; ++direction) {
        for (int ear = 0; ear < k_n_ears; ++ear) {
            const auto input_offset = (static_cast<std::size_t>(direction) * k_n_ears + static_cast<std::size_t>(ear)) *
                                      static_cast<std::size_t>(dataset.hrir_len);
            SRC_DATA request{};
            request.data_in = dataset.hrirs.data() + input_offset;
            request.data_out = scratch.data();
            request.input_frames = dataset.hrir_len;
            request.output_frames = static_cast<long>(scratch.size());
            request.src_ratio = ratio;
            request.end_of_input = 1;
            const int error = src_simple(&request, SRC_SINC_MEDIUM_QUALITY, 1);
            if (error != 0) {
                return make_error(ErrorCode::render_failed,
                                  fmt::format("failed to resample live binaural HRTF: {}", src_strerror(error)));
            }
            const auto copy_count = std::min<std::size_t>(static_cast<std::size_t>(output_length),
                                                          static_cast<std::size_t>(request.output_frames_gen));
            const auto output_offset =
                (static_cast<std::size_t>(direction) * k_n_ears + static_cast<std::size_t>(ear)) *
                static_cast<std::size_t>(output_length);
            std::copy_n(scratch.data(), copy_count, output.data() + output_offset);
        }
    }
    dataset.sample_rate = static_cast<int>(sample_rate);
    dataset.hrir_len = output_length;
    dataset.hrirs = std::move(output);
    return dataset;
}

void decorrelate(DiffuseState& state, const float* input, std::size_t frames, float* output) noexcept {
    constexpr std::array<std::size_t, 8U> k_offsets{3U, 7U, 11U, 17U, 19U, 23U, 29U, 31U};
    constexpr std::array<float, 8U> k_polarity{1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, -1.0F, -1.0F};
    constexpr float k_normalization = 0.35355339F;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        state.delay.at(state.write_pos) = input[frame];
        float sum = 0.0F;
        for (std::size_t tap = 0; tap < k_offsets.size(); ++tap) {
            const auto read = (state.write_pos + k_diffuse_delay_len - k_offsets.at(tap)) % k_diffuse_delay_len;
            sum += state.delay.at(read) * k_polarity.at(tap);
        }
        output[frame] = sum * k_normalization;
        state.write_pos = (state.write_pos + 1U) % k_diffuse_delay_len;
    }
}

void advance_silence(OlaState& state, std::size_t frames, float* left, float* right) noexcept {
    const auto emitted = std::min(frames, state.left.size());
    for (std::size_t frame = 0; frame < emitted; ++frame) {
        left[frame] += state.left[frame];
        right[frame] += state.right[frame];
    }
    if (frames >= state.left.size()) {
        std::ranges::fill(state.left, 0.0F);
        std::ranges::fill(state.right, 0.0F);
        return;
    }
    const auto remaining = state.left.size() - frames;
    std::move(state.left.begin() + static_cast<std::ptrdiff_t>(frames), state.left.end(), state.left.begin());
    std::move(state.right.begin() + static_cast<std::ptrdiff_t>(frames), state.right.end(), state.right.begin());
    std::fill(state.left.begin() + static_cast<std::ptrdiff_t>(remaining), state.left.end(), 0.0F);
    std::fill(state.right.begin() + static_cast<std::ptrdiff_t>(remaining), state.right.end(), 0.0F);
}

// The convolution kernel keeps its preallocated FFT/OLA workspaces explicit to make allocation-free reuse obvious.
// NOLINTNEXTLINE(readability-function-size)
void convolve(void* fft,
              const BinauralState& state,
              const float* source,
              std::size_t frames,
              float gain,
              const std::vector<float_complex>& hrtf,
              OlaState& ola,
              float* left,
              float* right,
              ConvolutionScratch& scratch) {
    std::ranges::fill(scratch.fft_input, 0.0F);
    std::copy_n(source, frames, scratch.fft_input.begin());
    saf_rfft_forward(fft, scratch.fft_input.data(), scratch.source_fd.data());
    for (int ear = 0; ear < k_n_ears; ++ear) {
        for (int band = 0; band < state.n_bands; ++band) {
            scratch.output_fd[static_cast<std::size_t>(band)] =
                gain * scratch.source_fd[static_cast<std::size_t>(band)] *
                hrtf[(static_cast<std::size_t>(band) * k_n_ears) + static_cast<std::size_t>(ear)];
        }
        saf_rfft_backward(fft, scratch.output_fd.data(), scratch.fft_output.data());
        auto& overlap = ear == 0 ? ola.left : ola.right;
        float* destination = ear == 0 ? left : right;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            destination[frame] += scratch.fft_output[frame] + (frame < overlap.size() ? overlap[frame] : 0.0F);
        }
        for (std::size_t tap = 0; tap < overlap.size(); ++tap) {
            const float residual = frames + tap < overlap.size() ? overlap[frames + tap] : 0.0F;
            overlap[tap] = scratch.fft_output[frames + tap] + residual;
        }
    }
}

class LiveBinauralRenderer final : public ILiveSceneRenderer {
  public:
    LiveBinauralRenderer(RendererConfig config, std::unique_ptr<BinauralState> state, DiagnosticSink diagnostics)
        : config_(std::move(config)), state_(std::move(state)), diagnostics_(std::move(diagnostics)) {
        saf_rfft_create(&fft_, state_->fft_size);
        scratch_.resize(*state_);
    }

    LiveBinauralRenderer(const LiveBinauralRenderer&) = delete;
    LiveBinauralRenderer& operator=(const LiveBinauralRenderer&) = delete;
    LiveBinauralRenderer(LiveBinauralRenderer&&) = delete;
    LiveBinauralRenderer& operator=(LiveBinauralRenderer&&) = delete;

    ~LiveBinauralRenderer() override {
        if (fft_ != nullptr) {
            saf_rfft_destroy(&fft_);
        }
    }

    [[nodiscard]] Result<void> configure_generation(std::uint64_t generation_id,
                                                    std::span<const ElementDescriptor> descriptors) override {
        generation_id_ = generation_id;
        elements_.clear();
        element_index_.clear();
        warned_.clear();
        elements_.reserve(descriptors.size());
        for (const auto& descriptor : descriptors) {
            RuntimeElement runtime;
            runtime.descriptor = descriptor;
            runtime.current = default_state(descriptor);
            runtime.target = runtime.current;
            runtime.ola = OlaState(static_cast<std::size_t>(state_->overlap_len));
            element_index_.emplace(descriptor.element_id, elements_.size());
            elements_.push_back(std::move(runtime));
        }
        return {};
    }

    void reset() override {
        generation_id_ = 0U;
        elements_.clear();
        element_index_.clear();
        warned_.clear();
    }

    [[nodiscard]] Result<void> render(const Frame& frame, std::span<float> output) override {
        const auto required = static_cast<std::size_t>(frame.duration_samples) * 2U;
        if (output.size() < required) {
            return make_error(ErrorCode::invalid_argument, "live binaural output buffer is too small");
        }
        if (frame.generation_id != generation_id_) {
            return make_error(ErrorCode::invalid_argument, "live binaural frame uses an unconfigured generation");
        }
        std::ranges::fill(output.first(required), 0.0F);

        for (const auto& initial : frame.initial_states) {
            const auto found = element_index_.find(initial.element_id);
            if (found == element_index_.end()) {
                return make_error(ErrorCode::invalid_argument, "initial state references an unknown element");
            }
            auto& element = elements_[found->second];
            ObjectState state = default_state(element.descriptor);
            copy_state_fields(state, initial.state, initial.state.valid_fields);
            element.current = state;
            element.target = state;
            element.ramp_remaining = 0U;
            element.initialized = true;
        }
        for (auto& element : elements_) {
            if (!element.initialized) {
                element.initialized = true;
            }
        }

        std::unordered_map<std::uint64_t, const PcmPlane*> planes;
        planes.reserve(frame.pcm.size());
        for (const auto& plane : frame.pcm) {
            planes.emplace(plane.element_id, &plane);
        }

        std::size_t update_index = 0U;
        std::uint32_t cursor = 0U;
        while (cursor < frame.duration_samples) {
            while (update_index < frame.updates.size() && frame.updates[update_index].offset_samples == cursor) {
                const auto applied = apply_update(frame.updates[update_index]);
                if (!applied) {
                    return tl::unexpected{applied.error()};
                }
                ++update_index;
            }
            std::uint32_t segment_end = frame.duration_samples;
            if (update_index < frame.updates.size()) {
                segment_end = std::min(segment_end, frame.updates[update_index].offset_samples);
            }
            std::uint32_t frames = std::min<std::uint32_t>(k_convolution_block, segment_end - cursor);
            for (const auto& element : elements_) {
                if (element.ramp_remaining > 0U) {
                    frames = std::min(frames, element.ramp_remaining);
                }
            }
            if (frames == 0U) {
                return make_error(ErrorCode::invalid_argument, "live binaural metadata updates are not ordered");
            }
            for (auto& element : elements_) {
                const auto plane = planes.find(element.descriptor.element_id);
                const PcmPlane* pcm = plane != planes.end() ? plane->second : nullptr;
                auto rendered = render_element(frame, element, pcm, cursor, frames, output);
                if (!rendered) {
                    return tl::unexpected{rendered.error()};
                }
            }
            advance_states(frames);
            cursor += frames;
        }
        return {};
    }

    [[nodiscard]] std::uint32_t output_channels() const noexcept override { return 2U; }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return config_.sample_rate; }
    [[nodiscard]] std::uint32_t tail_input_frames() const noexcept override {
        return static_cast<std::uint32_t>(state_->overlap_len) + static_cast<std::uint32_t>(k_diffuse_delay_len);
    }

  private:
    [[nodiscard]] Result<void> apply_update(const MetadataUpdate& update) {
        const auto found = element_index_.find(update.element_id);
        if (found == element_index_.end()) {
            return make_error(ErrorCode::invalid_argument, "metadata update references an unknown element");
        }
        auto& element = elements_[found->second];
        ObjectState target = element.target;
        copy_state_fields(target, update.state, update.changed_fields);
        element.target = target;
        element.ramp_remaining = update.ramp_duration_samples;
        if (element.ramp_remaining == 0U) {
            element.current = element.target;
        }
        return {};
    }

    void advance_states(std::uint32_t frames) noexcept {
        for (auto& element : elements_) {
            if (element.ramp_remaining == 0U) {
                continue;
            }
            const auto advanced = std::min(frames, element.ramp_remaining);
            const float alpha = static_cast<float>(advanced) / static_cast<float>(element.ramp_remaining);
            element.current = interpolated_state(element.current, element.target, alpha);
            element.ramp_remaining -= advanced;
            if (element.ramp_remaining == 0U) {
                element.current = element.target;
            }
        }
    }

    [[nodiscard]] ObjectState segment_end_state(const RuntimeElement& element, std::uint32_t frames) const noexcept {
        if (element.ramp_remaining == 0U) {
            return element.current;
        }
        const float alpha =
            static_cast<float>(std::min(frames, element.ramp_remaining)) / static_cast<float>(element.ramp_remaining);
        return interpolated_state(element.current, element.target, alpha);
    }

    [[nodiscard]] Result<std::pair<float, float>>
    direction_for(const RuntimeElement& element, const ObjectState& state, const Frame& frame) {
        if (element.descriptor.role == ElementRole::direct_speaker && !element.descriptor.speaker_label.empty()) {
            if (const auto direction = label_position(element.descriptor.speaker_label)) {
                return *direction;
            }
            warn_once(frame,
                      element.descriptor.element_id,
                      0U,
                      DiagnosticCode::direct_speaker_fallback,
                      "DirectSpeakers label is unknown; using its canonical fixed position");
        }
        if (element.descriptor.has_position) {
            return cartesian_to_polar(element.descriptor.x, element.descriptor.y, element.descriptor.z);
        }
        if ((state.valid_fields & state_position) != 0U) {
            return cartesian_to_polar(state.x, state.y, state.z);
        }
        if (element.descriptor.role == ElementRole::direct_speaker) {
            return make_error(ErrorCode::unsupported,
                              fmt::format("DirectSpeakers element {} has neither a known label nor a position",
                                          element.descriptor.element_id));
        }
        return std::pair<float, float>{0.0F, 0.0F};
    }

    [[nodiscard]] Result<void> hrtf_for(const RuntimeElement& element,
                                        const ObjectState& object_state,
                                        const Frame& frame,
                                        std::vector<float_complex>& output) {
        auto direction = direction_for(element, object_state, frame);
        if (!direction) {
            return tl::unexpected{direction.error()};
        }
        float azimuth = direction->first;
        const float elevation = direction->second;
        if (object_state.channel_lock) {
            azimuth = std::fabs(azimuth - 30.0F) < std::fabs(azimuth + 30.0F) ? 30.0F : -30.0F;
        }
        if (object_state.screen_reference) {
            warn_once(frame,
                      element.descriptor.element_id,
                      state_screen_reference,
                      DiagnosticCode::semantic_degraded,
                      "live binaural has no screen transform; canonical position is used");
        }

        struct DirectionWeight {
            float azimuth;
            float elevation;
            float weight;
        };
        std::vector<DirectionWeight> directions{{azimuth, elevation, 1.0F}};
        if (element.descriptor.role == ElementRole::object) {
            const float divergence = std::clamp(object_state.divergence, 0.0F, 1.0F);
            if (divergence > 0.0F) {
                directions.front().weight = 1.0F - divergence;
                directions.push_back({azimuth - (30.0F * divergence), elevation, divergence * 0.5F});
                directions.push_back({azimuth + (30.0F * divergence), elevation, divergence * 0.5F});
            }
            const float width = std::clamp(object_state.width, 0.0F, 1.0F) * 60.0F;
            const float height = std::clamp(object_state.height, 0.0F, 1.0F) * 45.0F;
            const float depth = std::clamp(object_state.depth, 0.0F, 1.0F) * 20.0F;
            const bool has_extent = width > 0.01F || height > 0.01F || depth > 0.01F;
            if (has_extent && config_.binaural_spread_mode == BinauralSpreadMode::none) {
                warn_once(frame,
                          element.descriptor.element_id,
                          state_extent,
                          DiagnosticCode::semantic_degraded,
                          "binaural spread mode is none; extent is ignored");
            } else if (has_extent) {
                if (config_.binaural_spread_mode == BinauralSpreadMode::saf_spreader) {
                    warn_once(frame,
                              element.descriptor.element_id,
                              state_extent,
                              DiagnosticCode::semantic_degraded,
                              "dynamic extent uses the live HRTF cloud path; SAF spreader topology is fixed");
                }
                directions.push_back({azimuth - width, elevation, 0.5F});
                directions.push_back({azimuth + width, elevation, 0.5F});
                directions.push_back({azimuth, std::clamp(elevation - height, -90.0F, 90.0F), 0.5F});
                directions.push_back({azimuth, std::clamp(elevation + height, -90.0F, 90.0F), 0.5F});
                if (depth > 0.01F) {
                    directions.push_back({azimuth + 180.0F, elevation, depth / 20.0F});
                }
            }
        }

        const float weight_sum =
            std::accumulate(directions.begin(), directions.end(), 0.0F, [](float sum, const DirectionWeight& item) {
                return sum + item.weight;
            });
        output.assign(static_cast<std::size_t>(state_->n_bands) * k_n_ears, float_complex{0.0F, 0.0F});
        for (const auto& item : directions) {
            compute_hrtf_into(*state_, item.azimuth, item.elevation, scratch_.hrtf_temp);
            const float normalized = item.weight / std::max(weight_sum, 1.0e-6F);
            for (std::size_t band = 0; band < output.size(); ++band) {
                output[band] += scratch_.hrtf_temp[band] * normalized;
            }
        }
        return {};
    }

    [[nodiscard]] Result<void> render_element(const Frame& frame,
                                              RuntimeElement& element,
                                              const PcmPlane* plane,
                                              std::uint32_t offset,
                                              std::uint32_t frames,
                                              std::span<float> output) {
        const ObjectState start = element.current;
        const ObjectState end = segment_end_state(element, frames);
        const auto frame_count = static_cast<std::size_t>(frames);
        const bool has_signal = plane != nullptr && plane->has_signal;
        for (std::size_t index = 0; index < frame_count; ++index) {
            scratch_.source_start[index] = has_signal ? plane->samples[static_cast<std::size_t>(offset) + index] : 0.0F;
        }

        const float start_gain = start.active ? start.linear_gain : 0.0F;
        const float end_gain = end.active ? end.linear_gain : 0.0F;
        if (element.descriptor.role == ElementRole::lfe) {
            for (std::size_t index = 0; index < frame_count; ++index) {
                const float alpha = static_cast<float>(index) / static_cast<float>(frame_count);
                const float gain = start_gain + ((end_gain - start_gain) * alpha);
                const float sample = scratch_.source_start[index] * gain;
                const auto output_index = (static_cast<std::size_t>(offset) + index) * 2U;
                output[output_index] += sample;
                output[output_index + 1U] += sample;
            }
            return {};
        }

        std::fill_n(scratch_.left_start.begin(), frame_count, 0.0F);
        std::fill_n(scratch_.right_start.begin(), frame_count, 0.0F);
        if (start_gain == 0.0F && end_gain == 0.0F) {
            std::fill_n(scratch_.source_start.begin(), frame_count, 0.0F);
        }
        decorrelate(element.diffuse, scratch_.source_start.data(), frame_count, scratch_.diffuse.data());
        const float start_diffuse = std::clamp(start.diffuse, 0.0F, 1.0F);
        const float end_diffuse = std::clamp(end.diffuse, 0.0F, 1.0F);
        const bool has_diffuse_tail = (start_diffuse > 0.0F || end_diffuse > 0.0F) &&
                                      std::any_of(scratch_.diffuse.begin(),
                                                  scratch_.diffuse.begin() + static_cast<std::ptrdiff_t>(frame_count),
                                                  [](float sample) { return sample != 0.0F; });
        if ((!has_signal && !has_diffuse_tail) || (start_gain == 0.0F && end_gain == 0.0F)) {
            advance_silence(element.ola, frame_count, scratch_.left_start.data(), scratch_.right_start.data());
        } else {
            for (std::size_t index = 0; index < frame_count; ++index) {
                scratch_.source_end[index] =
                    (scratch_.source_start[index] * (1.0F - end_diffuse)) + (scratch_.diffuse[index] * end_diffuse);
                scratch_.source_start[index] =
                    (scratch_.source_start[index] * (1.0F - start_diffuse)) + (scratch_.diffuse[index] * start_diffuse);
            }
            auto start_hrtf = hrtf_for(element, start, frame, scratch_.hrtf_start);
            if (!start_hrtf) {
                return tl::unexpected{start_hrtf.error()};
            }
            auto end_hrtf = hrtf_for(element, end, frame, scratch_.hrtf_end);
            if (!end_hrtf) {
                return tl::unexpected{end_hrtf.error()};
            }

            if (element.ramp_remaining == 0U && start_diffuse == end_diffuse) {
                convolve(fft_,
                         *state_,
                         scratch_.source_start.data(),
                         frame_count,
                         start_gain,
                         scratch_.hrtf_start,
                         element.ola,
                         scratch_.left_start.data(),
                         scratch_.right_start.data(),
                         scratch_);
            } else {
                OlaState start_ola = element.ola;
                OlaState end_ola = element.ola;
                std::fill_n(scratch_.left_end.begin(), frame_count, 0.0F);
                std::fill_n(scratch_.right_end.begin(), frame_count, 0.0F);
                convolve(fft_,
                         *state_,
                         scratch_.source_start.data(),
                         frame_count,
                         start_gain,
                         scratch_.hrtf_start,
                         start_ola,
                         scratch_.left_start.data(),
                         scratch_.right_start.data(),
                         scratch_);
                convolve(fft_,
                         *state_,
                         scratch_.source_end.data(),
                         frame_count,
                         end_gain,
                         scratch_.hrtf_end,
                         end_ola,
                         scratch_.left_end.data(),
                         scratch_.right_end.data(),
                         scratch_);
                for (std::size_t index = 0; index < frame_count; ++index) {
                    const float alpha = static_cast<float>(index) / static_cast<float>(frame_count);
                    scratch_.left_start[index] =
                        (scratch_.left_start[index] * (1.0F - alpha)) + (scratch_.left_end[index] * alpha);
                    scratch_.right_start[index] =
                        (scratch_.right_start[index] * (1.0F - alpha)) + (scratch_.right_end[index] * alpha);
                }
                element.ola = std::move(end_ola);
            }
        }

        for (std::size_t index = 0; index < frame_count; ++index) {
            const auto output_index = (static_cast<std::size_t>(offset) + index) * 2U;
            output[output_index] += scratch_.left_start[index];
            output[output_index + 1U] += scratch_.right_start[index];
        }
        return {};
    }

    void warn_once(
        const Frame& frame, std::uint64_t element_id, std::uint64_t field, DiagnosticCode code, std::string message) {
        const auto key = (field << 8U) ^ static_cast<std::uint64_t>(code);
        if (!warned_.insert(key).second || !diagnostics_) {
            return;
        }
        diagnostics_(
            {LogLevel::warning, code, frame.epoch_id, frame.generation_id, element_id, field, std::move(message)});
    }

    RendererConfig config_;
    std::unique_ptr<BinauralState> state_;
    DiagnosticSink diagnostics_;
    void* fft_{nullptr};
    ConvolutionScratch scratch_;
    std::uint64_t generation_id_{0U};
    std::vector<RuntimeElement> elements_;
    std::unordered_map<std::uint64_t, std::size_t> element_index_;
    std::unordered_set<std::uint64_t> warned_;
};

} // namespace

Result<std::unique_ptr<ILiveSceneRenderer>> create_live_binaural_renderer(const RendererConfig& config,
                                                                          DiagnosticSink diagnostics) {
    Result<HrtfDataset> dataset = config.sofa_path.empty()
                                      ? Result<HrtfDataset>{binaural_internal::built_in_kemar_dataset()}
                                      : binaural_internal::load_sofa_dataset(config.sofa_path, 0U);
    if (!dataset) {
        return tl::unexpected{dataset.error()};
    }
    auto resampled = resample_dataset(std::move(*dataset), config.sample_rate);
    if (!resampled) {
        return tl::unexpected{resampled.error()};
    }
    auto state = binaural_internal::build_binaural_state(std::move(*resampled), k_convolution_block);
    if (state == nullptr) {
        return make_error(ErrorCode::render_failed, "failed to build live binaural HRTF interpolation state");
    }
    auto renderer = std::make_unique<LiveBinauralRenderer>(config, std::move(state), std::move(diagnostics));
    if (renderer->sample_rate() == 0U) {
        return make_error(ErrorCode::render_failed, "live binaural renderer has an invalid sample rate");
    }
    std::unique_ptr<ILiveSceneRenderer> result = std::move(renderer);
    return result;
}

} // namespace mradm::live_scene
