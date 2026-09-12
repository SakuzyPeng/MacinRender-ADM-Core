#include "live_binaural_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Keep SAF's complex declaration outside the C linkage opened by some SAF headers.
#include <saf_utility_complex.h>
#include <samplerate.h>

#include <fmt/format.h>

#include "adm/scene.h"

#include "binaural_internal.h"
#include "head_rotation.h"
#include "live_binaural_convolver.h"

namespace mradm::live_scene {

namespace {

using binaural_internal::BinauralState;
using binaural_internal::compute_continuous_hrtf_into;
using binaural_internal::HrtfDataset;
using binaural_internal::k_n_ears;

constexpr std::uint32_t k_convolution_block = 1024U;
constexpr std::size_t k_diffuse_delay_len = 32U;

// These private renderer-local aggregate workspaces intentionally expose their storage to the
// convolution helpers in this translation unit.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct DiffuseState {
    std::array<float, k_diffuse_delay_len> delay{};
    std::size_t write_pos{0U};
};

struct RuntimeElement {
    ElementDescriptor descriptor;
    ObjectState current;
    ObjectState target;
    std::optional<std::pair<float, float>> current_direction;
    std::optional<std::pair<float, float>> target_direction;
    std::array<std::uint32_t, 11U> ramp_remaining{};
    bool initialized{false};
    BinauralConvolutionState convolution;
    DiffuseState diffuse;
};

struct ConvolutionScratch {
    std::vector<float> source;
    std::vector<float> diffuse;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float_complex> hrtf_target;
    std::vector<float_complex> hrtf_temp;

    void resize() {
        source.resize(k_convolution_block);
        diffuse.resize(k_convolution_block);
        left.resize(k_convolution_block);
        right.resize(k_convolution_block);
    }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] ObjectState default_state(const ElementDescriptor& descriptor) {
    ObjectState state;
    state.valid_fields = k_known_state_fields & ~state_position;
    if (descriptor.role == ElementRole::object) {
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
    if ((fields & state_divergence_range) != 0U) {
        destination.divergence_azimuth_range = source.divergence_azimuth_range;
        destination.divergence_position_range = source.divergence_position_range;
    }
    if ((fields & state_channel_lock_max_distance) != 0U) {
        destination.channel_lock_max_distance = source.channel_lock_max_distance;
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
    result.divergence_azimuth_range = lerp(start.divergence_azimuth_range, target.divergence_azimuth_range);
    result.divergence_position_range = lerp(start.divergence_position_range, target.divergence_position_range);
    if (start.channel_lock_max_distance && target.channel_lock_max_distance) {
        result.channel_lock_max_distance = lerp(*start.channel_lock_max_distance, *target.channel_lock_max_distance);
    }
    if (alpha >= 1.0F) {
        result.active = target.active;
        result.channel_lock = target.channel_lock;
        result.screen_reference = target.screen_reference;
        result.head_locked = target.head_locked;
        result.channel_lock_max_distance = target.channel_lock_max_distance;
        result.valid_fields = target.valid_fields;
    }
    return result;
}

[[nodiscard]] std::pair<float, float> interpolated_direction(const std::pair<float, float>& start,
                                                             const std::pair<float, float>& target,
                                                             float alpha) noexcept {
    if (alpha >= 1.0F) {
        return target;
    }
    const float azimuth_delta = std::remainder(target.first - start.first, 360.0F);
    return {start.first + (azimuth_delta * alpha), start.second + ((target.second - start.second) * alpha)};
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

[[nodiscard]] std::pair<float, float>
channel_locked_direction(float azimuth, float elevation, const ObjectState& object_state) {
    if (!object_state.channel_lock) {
        return {azimuth, elevation};
    }
    SceneObjectBlock block;
    block.position.cartesian = false;
    block.position.azimuth = azimuth;
    block.position.elevation = elevation;
    block.position.distance = 1.0F;
    block.channel_lock = true;
    block.channel_lock_max_distance = object_state.channel_lock_max_distance;
    static const std::vector<SceneOutputSpeaker> k_lock_speakers{
        {30.0F, 0.0F, false},
        {-30.0F, 0.0F, false},
    };
    const auto locked = apply_channel_lock(block, k_lock_speakers);
    const auto locked_position = scene_position_to_polar(locked.position);
    return {locked_position.azimuth, locked_position.elevation};
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

// Cache only immutable interpolation data. FFT workspaces and convolution tails
// remain private to each renderer, so preparing another output cannot alter it.
class HrtfStateCache {
  public:
    class Key {
      public:
        bool operator==(const Key&) const = default;

      private:
        friend class HrtfStateCache;
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
        std::uintmax_t size{0U};
        std::uint32_t sample_rate{0U};
    };

    [[nodiscard]] static std::optional<Key> make_key(const RendererConfig& config) {
        Key result;
        result.sample_rate = config.sample_rate;
        if (!config.sofa_path.empty()) {
            std::error_code error;
            result.path = std::filesystem::canonical(config.sofa_path, error);
            if (error) {
                return std::nullopt;
            }
            result.modified = std::filesystem::last_write_time(result.path, error);
            if (error) {
                return std::nullopt;
            }
            result.size = std::filesystem::file_size(result.path, error);
            if (error) {
                return std::nullopt;
            }
        }
        return result;
    }

    [[nodiscard]] std::shared_ptr<const BinauralState> find(const Key& key) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = std::ranges::find(entries_, key, &Entry::key);
        if (found == entries_.end()) {
            return {};
        }
        auto entry = std::move(*found);
        entries_.erase(found);
        auto state = entry.state;
        entries_.push_back(std::move(entry));
        return state;
    }

    void insert(Key key, std::shared_ptr<const BinauralState> state) {
        const auto bytes = sizeof(BinauralState) + state->dataset_name.capacity() +
                           (state->hrtf_fd.capacity() * sizeof(float_complex)) +
                           ((state->vbap_gains.capacity() + state->hrtf_td.capacity() +
                             state->grid_dirs_deg.capacity() + state->hrtf_magnitudes.capacity()) *
                            sizeof(float)) +
                           (state->vbap_dirs.capacity() * sizeof(int));
        constexpr std::size_t k_byte_budget = std::size_t{64U} * 1024U * 1024U;
        if (bytes > k_byte_budget) {
            return;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        if (std::ranges::find(entries_, key, &Entry::key) != entries_.end()) {
            return;
        }
        while (!entries_.empty() && (entries_.size() >= 4U || bytes_ + bytes > k_byte_budget)) {
            bytes_ -= entries_.front().bytes;
            entries_.erase(entries_.begin());
        }
        entries_.push_back({std::move(key), std::move(state), bytes});
        bytes_ += bytes;
    }

  private:
    struct Entry {
        Key key;
        std::shared_ptr<const BinauralState> state;
        std::size_t bytes{0U};
    };
    std::mutex mutex_;
    std::vector<Entry> entries_;
    std::size_t bytes_{0U};
};

[[nodiscard]] Result<std::shared_ptr<const BinauralState>> prepare_hrtf_state(const RendererConfig& config) {
    static HrtfStateCache cache;
    const auto key = HrtfStateCache::make_key(config);
    if (key) {
        if (auto cached = cache.find(*key)) {
            return cached;
        }
    }
    // File IO, resampling and table construction never hold the cache mutex.
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
    auto prepared = binaural_internal::build_binaural_state(std::move(*resampled), k_convolution_block);
    if (!prepared) {
        return make_error(ErrorCode::render_failed, "failed to build live binaural HRTF interpolation state");
    }
    prepared->hrtf_magnitudes.resize(prepared->hrtf_fd.size());
    std::ranges::transform(
        prepared->hrtf_fd, prepared->hrtf_magnitudes.begin(), [](float_complex value) { return std::abs(value); });
    // Live convolution and extent rendering use only the frequency-domain HRTFs
    // and compressed interpolation grid. The original HRIRs and measurement
    // directions are needed during preparation (and by the offline SAF spreader),
    // but retaining them in this live-only cache wastes a full dataset per entry.
    std::vector<float>{}.swap(prepared->hrtf_td);
    std::vector<float>{}.swap(prepared->grid_dirs_deg);
    std::shared_ptr<const BinauralState> state = std::move(prepared);
    if (key && key == HrtfStateCache::make_key(config)) {
        cache.insert(*key, state);
    }
    return state;
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

class LiveBinauralRenderer final : public ILiveSceneRenderer {
  public:
    LiveBinauralRenderer(RendererConfig config, std::shared_ptr<const BinauralState> state, DiagnosticSink diagnostics)
        : config_(std::move(config)), state_(std::move(state)), diagnostics_(std::move(diagnostics)),
          convolver_(state_->fft_size, k_convolution_block, config_.sample_rate) {
        scratch_.resize();
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
            if (descriptor.role != ElementRole::lfe) {
                runtime.convolution = convolver_.make_state();
            }
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

        auto initialized = initialize_frame(frame);
        if (!initialized) {
            return tl::unexpected{initialized.error()};
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
                const auto applied = apply_update(frame.updates[update_index], frame);
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
                for (const auto remaining : element.ramp_remaining) {
                    if (remaining > 0U) {
                        frames = std::min(frames, remaining);
                    }
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
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        listener_orientation_ = orientation;
    }
    [[nodiscard]] std::uint32_t tail_input_frames() const noexcept override {
        return convolver_.tail_frames() + static_cast<std::uint32_t>(k_diffuse_delay_len);
    }

  private:
    [[nodiscard]] Result<void> initialize_frame(const Frame& frame) {
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
            auto direction_initialized = initialize_direction(element, state, frame);
            if (!direction_initialized) {
                return tl::unexpected{direction_initialized.error()};
            }
            element.ramp_remaining.fill(0U);
            element.initialized = true;
        }
        for (auto& element : elements_) {
            if (!element.initialized) {
                auto direction_initialized = initialize_direction(element, element.current, frame);
                if (!direction_initialized) {
                    return tl::unexpected{direction_initialized.error()};
                }
                element.initialized = true;
            }
        }

        return {};
    }

    [[nodiscard]] Result<void>
    initialize_direction(RuntimeElement& element, const ObjectState& state, const Frame& frame) {
        if (element.descriptor.role != ElementRole::direct_speaker) {
            return {};
        }
        auto direction = direction_for(element, state, frame);
        if (!direction) {
            return tl::unexpected{direction.error()};
        }
        element.current_direction = *direction;
        element.target_direction = *direction;
        return {};
    }

    [[nodiscard]] Result<void> apply_update(const MetadataUpdate& update, const Frame& frame) {
        const auto found = element_index_.find(update.element_id);
        if (found == element_index_.end()) {
            return make_error(ErrorCode::invalid_argument, "metadata update references an unknown element");
        }
        auto& element = elements_[found->second];
        ObjectState target = element.target;
        target.valid_fields &= ~update.cleared_fields;
        copy_state_fields(target, update.state, update.changed_fields);
        std::optional<std::pair<float, float>> target_direction;
        if (element.descriptor.role == ElementRole::direct_speaker) {
            auto direction = direction_for(element, target, frame);
            if (!direction) {
                return tl::unexpected{direction.error()};
            }
            target_direction = *direction;
        }
        element.target = target;
        if ((update.changed_fields & state_head_locked) != 0U) {
            // Content reference frames switch at the event sample, independently of
            // any continuous ramp carried by the same event or an earlier event.
            element.current.head_locked = target.head_locked;
            element.current.valid_fields |= state_head_locked;
        }
        if (update.changed_fields == state_head_locked && update.cleared_fields == 0U) {
            return {};
        }
        if (target_direction) {
            element.target_direction = *target_direction;
        }
        std::uint32_t duration = config_.object_smoothing_frames;
        if (update.jump_position) {
            duration = 0U;
        } else if (update.ramp_duration_samples != 0U) {
            duration = update.ramp_duration_samples;
        }
        const auto changed = update.changed_fields | update.cleared_fields;
        for (std::size_t field = 0U; field < element.ramp_remaining.size(); ++field) {
            const std::uint64_t mask = 1ULL << field;
            if ((changed & mask) == 0U || mask == state_head_locked) {
                continue;
            }
            element.ramp_remaining.at(field) = duration;
            if (duration == 0U) {
                copy_state_fields(element.current, target, mask);
                element.current.valid_fields &= ~(update.cleared_fields & mask);
                if (mask == state_position) {
                    element.current_direction = element.target_direction;
                }
            }
        }
        return {};
    }

    void advance_states(std::uint32_t frames) noexcept {
        for (auto& element : elements_) {
            element.current_direction = segment_end_direction(element, frames);
            element.current = segment_end_state(element, frames);
            std::ranges::transform(element.ramp_remaining, element.ramp_remaining.begin(), [frames](auto remaining) {
                return remaining - std::min(frames, remaining);
            });
        }
    }

    [[nodiscard]] ObjectState segment_end_state(const RuntimeElement& element, std::uint32_t frames) const noexcept {
        ObjectState result = element.current;
        for (std::size_t field = 0U; field < element.ramp_remaining.size(); ++field) {
            const auto remaining = element.ramp_remaining.at(field);
            if (remaining == 0U) {
                continue;
            }
            const auto advanced = std::min(frames, remaining);
            const auto sample = interpolated_state(
                element.current, element.target, static_cast<float>(advanced) / static_cast<float>(remaining));
            const std::uint64_t mask = 1ULL << field;
            copy_state_fields(result, sample, mask);
            if (advanced == remaining && (element.target.valid_fields & mask) == 0U) {
                result.valid_fields &= ~mask;
            }
        }
        return result;
    }

    [[nodiscard]] std::optional<std::pair<float, float>> segment_end_direction(const RuntimeElement& element,
                                                                               std::uint32_t frames) const noexcept {
        const auto remaining = element.ramp_remaining[2U]; // Cartesian position / label fallback.
        if (!element.current_direction || !element.target_direction || remaining == 0U) {
            return element.current_direction;
        }
        const float alpha = static_cast<float>(std::min(frames, remaining)) / static_cast<float>(remaining);
        return interpolated_direction(*element.current_direction, *element.target_direction, alpha);
    }

    [[nodiscard]] Result<std::pair<float, float>>
    direction_for(const RuntimeElement& element, const ObjectState& state, const Frame& frame) {
        // A current canonical position (including a semantic-policy re-aim) wins
        // over the topology's fixed label. Without one, preserve label routing and
        // use the descriptor position only as its fallback.
        if ((state.valid_fields & state_position) != 0U) {
            return cartesian_to_polar(state.x, state.y, state.z);
        }
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
        if (element.descriptor.role == ElementRole::direct_speaker) {
            return make_error(ErrorCode::unsupported,
                              fmt::format("DirectSpeakers element {} has neither a known label nor a position",
                                          element.descriptor.element_id));
        }
        return std::pair<float, float>{0.0F, 0.0F};
    }

    [[nodiscard]] Result<void>
    hrtf_for(const RuntimeElement& element,
             const ObjectState& object_state,
             const Frame& frame,
             std::vector<float_complex>& output,
             const std::optional<std::pair<float, float>>& direction_override = std::nullopt) {
        std::pair<float, float> direction;
        if (direction_override) {
            direction = *direction_override;
        } else {
            auto resolved = direction_for(element, object_state, frame);
            if (!resolved) {
                return tl::unexpected{resolved.error()};
            }
            direction = *resolved;
        }
        float azimuth = direction.first;
        float elevation = direction.second;
        std::tie(azimuth, elevation) = channel_locked_direction(azimuth, elevation, object_state);
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
                float divergence_angle = object_state.divergence_azimuth_range;
                if (object_state.divergence_position_range > 0.0F &&
                    (object_state.valid_fields & state_position) != 0U) {
                    constexpr double k_radians_to_degrees = 180.0 / std::numbers::pi_v<double>;
                    const double distance = std::max(1.0e-6,
                                                     std::sqrt((static_cast<double>(object_state.x) * object_state.x) +
                                                               (static_cast<double>(object_state.y) * object_state.y) +
                                                               (static_cast<double>(object_state.z) * object_state.z)));
                    divergence_angle = static_cast<float>(
                        std::atan2(static_cast<double>(object_state.divergence_position_range), distance) *
                        k_radians_to_degrees);
                }
                divergence_angle = std::clamp(divergence_angle, 0.0F, 120.0F);
                const float side_weight = divergence / (divergence + 1.0F);
                directions.front().weight = (1.0F - divergence) / (divergence + 1.0F);
                directions.push_back({azimuth - divergence_angle, elevation, side_weight});
                directions.push_back({azimuth + divergence_angle, elevation, side_weight});
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
        const bool rotate_to_head_space = !listener_orientation_.is_identity() && !object_state.head_locked;
        const render_common::HeadRotation rotation{listener_orientation_};
        for (const auto& item : directions) {
            float rendered_azimuth = item.azimuth;
            float rendered_elevation = item.elevation;
            if (rotate_to_head_space) {
                std::tie(rendered_azimuth, rendered_elevation) =
                    rotation.rotate_az_el(rendered_azimuth, rendered_elevation);
            }
            compute_continuous_hrtf_into(*state_, rendered_azimuth, rendered_elevation, scratch_.hrtf_temp);
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
            scratch_.source[index] = has_signal ? plane->samples[static_cast<std::size_t>(offset) + index] : 0.0F;
        }
        const float start_gain = start.active ? start.linear_gain : 0.0F;
        const float end_gain = end.active ? end.linear_gain : 0.0F;
        if (element.descriptor.role == ElementRole::lfe) {
            for (std::size_t index = 0; index < frame_count; ++index) {
                const float alpha = static_cast<float>(index) / static_cast<float>(frame_count);
                const float gain = start_gain + ((end_gain - start_gain) * alpha);
                const float sample = scratch_.source[index] * gain;
                const auto output_index = (static_cast<std::size_t>(offset) + index) * 2U;
                output[output_index] += sample;
                output[output_index + 1U] += sample;
            }
            return {};
        }

        // Gain and diffuse belong to the input timeline. Preserve their already
        // mixed samples in history instead of reapplying today's controls to an
        // old, filtered output tail.
        if (start_gain == 0.0F && end_gain == 0.0F) {
            std::fill_n(scratch_.source.begin(), frame_count, 0.0F);
        }
        decorrelate(element.diffuse, scratch_.source.data(), frame_count, scratch_.diffuse.data());
        const float start_diffuse = std::clamp(start.diffuse, 0.0F, 1.0F);
        const float end_diffuse = std::clamp(end.diffuse, 0.0F, 1.0F);
        for (std::size_t index = 0U; index < frame_count; ++index) {
            const float alpha = static_cast<float>(index) / static_cast<float>(frame_count);
            const float gain = start_gain + ((end_gain - start_gain) * alpha);
            const float diffuse = start_diffuse + ((end_diffuse - start_diffuse) * alpha);
            scratch_.source[index] =
                gain * ((scratch_.source[index] * (1.0F - diffuse)) + (scratch_.diffuse[index] * diffuse));
        }
        if (element.convolution.tail_remaining == 0U &&
            std::ranges::none_of(std::span{scratch_.source}.first(frame_count),
                                 [](float sample) { return sample != 0.0F; })) {
            // Inactive sources need no FFT or HRTF lookup after their history
            // drains. Re-entry initializes at the then-current direction.
            element.convolution.initialized = false;
            return {};
        }
        if (!element.convolution.initialized) {
            auto initial = hrtf_for(element, start, frame, scratch_.hrtf_target, element.current_direction);
            if (!initial) {
                return tl::unexpected{initial.error()};
            }
            convolver_.initialize(element.convolution, scratch_.hrtf_target);
        }
        auto target = hrtf_for(element, end, frame, scratch_.hrtf_target, segment_end_direction(element, frames));
        if (!target) {
            return tl::unexpected{target.error()};
        }
        bool spatial_ramp = false;
        for (std::size_t field = 0U; field < element.ramp_remaining.size(); ++field) {
            const auto mask = 1ULL << field;
            if ((mask & (state_active | state_linear_gain | state_diffuse | state_head_locked)) == 0U) {
                spatial_ramp = spatial_ramp || element.ramp_remaining.at(field) != 0U;
            }
        }
        convolver_.process(element.convolution,
                           scratch_.hrtf_target,
                           std::span{scratch_.source}.first(frame_count),
                           std::span{scratch_.left}.first(frame_count),
                           std::span{scratch_.right}.first(frame_count),
                           spatial_ramp);
        for (std::size_t index = 0; index < frame_count; ++index) {
            const auto output_index = (static_cast<std::size_t>(offset) + index) * 2U;
            output[output_index] += scratch_.left[index];
            output[output_index + 1U] += scratch_.right[index];
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
    std::shared_ptr<const BinauralState> state_;
    DiagnosticSink diagnostics_;
    LiveBinauralConvolver convolver_;
    ConvolutionScratch scratch_;
    std::uint64_t generation_id_{0U};
    std::vector<RuntimeElement> elements_;
    std::unordered_map<std::uint64_t, std::size_t> element_index_;
    std::unordered_set<std::uint64_t> warned_;
    ListenerOrientation listener_orientation_{};
};

} // namespace

Result<std::unique_ptr<ILiveSceneRenderer>> create_live_binaural_renderer(const RendererConfig& config,
                                                                          DiagnosticSink diagnostics) {
    auto state = prepare_hrtf_state(config);
    if (!state) {
        return tl::unexpected{state.error()};
    }
    auto renderer = std::make_unique<LiveBinauralRenderer>(config, std::move(*state), std::move(diagnostics));
    if (renderer->sample_rate() == 0U) {
        return make_error(ErrorCode::render_failed, "live binaural renderer has an invalid sample rate");
    }
    std::unique_ptr<ILiveSceneRenderer> result = std::move(renderer);
    return result;
}

} // namespace mradm::live_scene
