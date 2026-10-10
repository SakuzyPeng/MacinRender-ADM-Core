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
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "live_binaural.h"
#include "live_scene_trace.h"
#include "scene_math.h"

// Private project DSP complex type.

#include <fmt/format.h>

#include "adm/scene.h"

#include "binaural_internal.h"
#include "dsp.h"
#include "head_rotation.h"
#include "live_binaural_convolver.h"
#include "resampler.h"

namespace mradm::live_scene {

namespace {

using binaural_internal::BinauralState;
using binaural_internal::HrtfDataset;
using binaural_internal::k_n_ears;
constexpr std::uint32_t k_convolution_block = 1024U;


// These private renderer-local aggregate workspaces intentionally expose their storage to the
// convolution helpers in this translation unit.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct RuntimeElement {
    ElementDescriptor descriptor;
    ObjectState target;
    bool initialized{false};
};
struct StagedControl {
    ObjectState target;
    bool initialized{false};
    bool seen_initial{false};
    bool seen_pcm{false};
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

[[nodiscard]] std::pair<float, float> cartesian_to_polar(float x, float y, float z) noexcept {
    const auto p = dsp::scene_math<3, 3>(0U, {x, y, z});
    return {p[0], p[1]};
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

    const auto input_length = static_cast<std::size_t>(dataset.hrir_len);
    const auto numerator = static_cast<std::uint64_t>(dataset.hrir_len) * sample_rate;
    const auto output_frames = (numerator + static_cast<std::uint64_t>(dataset.sample_rate) - 1U) /
                               static_cast<std::uint64_t>(dataset.sample_rate);
    const auto filters = static_cast<std::size_t>(dataset.num_dirs) * k_n_ears;
    if (output_frames > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) || filters == 0U ||
        output_frames > std::numeric_limits<std::size_t>::max() / filters) {
        return make_error(ErrorCode::unsupported, "resampled HRTF dataset is too large");
    }
    const auto output_length = static_cast<std::size_t>(output_frames);
    auto resampler = dsp::Resampler::create(1U, static_cast<std::uint32_t>(dataset.sample_rate), sample_rate);
    if (!resampler) {
        return tl::unexpected{resampler.error()};
    }
    std::vector<float> output(filters * output_length);
    for (std::size_t filter = 0U; filter < filters; ++filter) {
        auto reset = resampler->reset();
        if (!reset) {
            return tl::unexpected{reset.error()};
        }
        const auto source = std::span{dataset.hrirs}.subspan(filter * input_length, input_length);
        const auto destination = std::span{output}.subspan(filter * output_length, output_length);
        std::size_t consumed = 0U;
        std::size_t written = 0U;
        while (consumed < input_length) {
            auto progress = resampler->process(source.subspan(consumed), destination.subspan(written));
            if (!progress) {
                return tl::unexpected{progress.error()};
            }
            consumed += progress->input_frames;
            written += progress->output_frames;
            if (progress->input_frames == 0U && progress->output_frames == 0U) {
                return make_error(ErrorCode::render_failed, "HRTF resampler made no progress");
            }
        }
        while (written < output_length) {
            auto generated = resampler->finish(destination.subspan(written));
            if (!generated) {
                return tl::unexpected{generated.error()};
            }
            if (*generated == 0U) {
                return make_error(ErrorCode::render_failed, "HRTF resampler returned a short filter");
            }
            written += *generated;
        }
    }
    dataset.sample_rate = static_cast<int>(sample_rate);
    dataset.hrir_len = static_cast<int>(output_length);
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
        const auto bytes = sizeof(BinauralState) + state->dataset_name.capacity() + state->filters.bytes() +
                           ((state->hrtf_td.capacity() + state->grid_dirs_deg.capacity()) * sizeof(float)) +
                           sizeof(binaural_internal::HrtfGrid);
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
    auto prepared = binaural_internal::build_binaural_state(std::move(*resampled), k_convolution_block, true);
    if (!prepared) {
        return tl::unexpected{prepared.error()};
    }
    auto state_owner = std::move(*prepared);
    // Live convolution and extent rendering use only the frequency-domain HRTFs
    // and compressed interpolation grid. The original HRIRs and measurement
    // directions are needed during preparation (and by the offline SAF spreader),
    // but retaining them in this live-only cache wastes a full dataset per entry.
    std::vector<float>{}.swap(state_owner->hrtf_td);
    std::vector<float>{}.swap(state_owner->grid_dirs_deg);
    std::shared_ptr<const BinauralState> state = std::move(state_owner);
    if (key && key == HrtfStateCache::make_key(config)) {
        cache.insert(*key, state);
    }
    return state;
}

// Producer ranges are validated upstream. Policy-expanded coordinates/extents may exceed them;
// this numerical boundary checks finiteness without narrowing the policy domain.
[[nodiscard]] bool valid_state(const ObjectState& state) noexcept {
    const auto coordinate = [](float v) { return std::isfinite(v); };
    const auto finite = [](float v) { return std::isfinite(v); };
    if ((state.valid_fields & ~k_known_state_fields) != 0U) {
        return false;
    }
    if ((state.valid_fields & state_linear_gain) != 0U &&
        (!std::isfinite(state.linear_gain) || state.linear_gain < 0)) {
        return false;
    }
    if ((state.valid_fields & state_position) != 0U &&
        (!coordinate(state.x) || !coordinate(state.y) || !coordinate(state.z))) {
        return false;
    }
    if ((state.valid_fields & state_extent) != 0U &&
        (!finite(state.width) || !finite(state.height) || !finite(state.depth))) {
        return false;
    }
    if ((state.valid_fields & state_diffuse) != 0U && !finite(state.diffuse)) {
        return false;
    }
    if ((state.valid_fields & state_divergence) != 0U && !finite(state.divergence)) {
        return false;
    }
    if ((state.valid_fields & state_divergence_range) != 0U &&
        (!std::isfinite(state.divergence_azimuth_range) || state.divergence_azimuth_range < 0 ||
         !std::isfinite(state.divergence_position_range) || state.divergence_position_range < 0)) {
        return false;
    }
    return (state.valid_fields & state_channel_lock_max_distance) == 0U || !state.channel_lock_max_distance ||
           (std::isfinite(*state.channel_lock_max_distance) && *state.channel_lock_max_distance >= 0);
}


[[nodiscard]] MradmLiveBinauralState numerical_state(const ObjectState& s) {
    return {s.valid_fields,
            s.active ? 1U : 0U,
            s.channel_lock ? 1U : 0U,
            s.screen_reference ? 1U : 0U,
            s.head_locked ? 1U : 0U,
            s.linear_gain,
            {s.x, s.y, s.z},
            {s.width, s.height, s.depth},
            s.diffuse,
            s.divergence,
            {s.divergence_azimuth_range, s.divergence_position_range},
            s.channel_lock_max_distance ? 1U : 0U,
            s.channel_lock_max_distance.value_or(0.0F)};
}
class LiveBinauralRenderer final : public ILiveSceneRenderer {
  public:
    LiveBinauralRenderer(RendererConfig config, std::shared_ptr<const BinauralState> state, DiagnosticSink diagnostics)
        : config_(std::move(config)), state_(std::move(state)), diagnostics_(std::move(diagnostics)) {
        auto prepared =
            dsp::LiveBinauralSession::create(state_->filters.get(), {}, config_.sample_rate, numerical_spread());
        if (!prepared) {
            throw std::runtime_error(prepared.error().message);
        }
        session_ = std::move(*prepared);
    }
    [[nodiscard]] Result<void> configure_generation(uint64_t generation,
                                                    std::span<const ElementDescriptor> descriptors) override {
        if (descriptors.size() > std::numeric_limits<uint32_t>::max()) {
            return make_error(ErrorCode::invalid_argument, "Live binaural element count overflows numerical indices");
        }
        std::vector<RuntimeElement> next;
        std::unordered_map<uint64_t, size_t> indices;
        std::vector<MradmLiveBinauralDescription> numeric;
        next.reserve(descriptors.size());
        indices.reserve(descriptors.size());
        numeric.reserve(descriptors.size());
        for (const auto& descriptor : descriptors) {
            if ((descriptor.role != ElementRole::object && descriptor.role != ElementRole::direct_speaker &&
                 descriptor.role != ElementRole::lfe) ||
                !indices.emplace(descriptor.element_id, next.size()).second) {
                return make_error(ErrorCode::invalid_argument, "Live binaural has an invalid or duplicate element");
            }
            auto defaults = default_state(descriptor);
            if (descriptor.has_position) {
                auto position = defaults;
                position.valid_fields |= state_position;
                if (!valid_state(position)) {
                    return make_error(ErrorCode::invalid_argument, "Live binaural element position is invalid");
                }
            }
            const auto fallback = descriptor.has_position ? cartesian_to_polar(descriptor.x, descriptor.y, descriptor.z)
                                                          : std::pair{0.0F, 0.0F};
            numeric.push_back({static_cast<uint32_t>(descriptor.role), 0, {fallback.first, fallback.second}});
            next.push_back({descriptor, defaults, false});
        }
        auto prepared =
            dsp::LiveBinauralSession::create(state_->filters.get(), numeric, config_.sample_rate, numerical_spread());
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        session_ = std::move(*prepared);
        elements_.swap(next);
        element_index_.swap(indices);
        generation_id_ = generation;
        warned_ = 0;
        return {};
    }
    void reset() override {
        session_.reset();
        generation_id_ = 0;
        elements_.clear();
        element_index_.clear();
        staged_.clear();
        initial_.clear();
        events_.clear();
        planes_.clear();
        warned_ = 0;
    }
    [[nodiscard]] Result<void> render(const Frame& frame, std::span<float> output) override {
        if (static_cast<size_t>(frame.duration_samples) > std::numeric_limits<size_t>::max() / 2U) {
            return make_error(ErrorCode::invalid_argument, "Live binaural output size overflows");
        }
        const size_t required = static_cast<size_t>(frame.duration_samples) * 2U;
        if (output.size() < required || frame.generation_id != generation_id_) {
            return make_error(ErrorCode::invalid_argument, "Live binaural output or generation is invalid");
        }
        auto prepared = prepare_initials(frame);
        if (!prepared) {
            return prepared;
        }
        prepared = prepare_planes(frame);
        if (!prepared) {
            return prepared;
        }
        prepared = prepare_events(frame);
        if (!prepared) {
            return prepared;
        }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
        trace_commands(frame);
#endif
        auto report = session_.process(
            frame.duration_samples,
            planes_,
            initial_,
            events_,
            {listener_orientation_.yaw_deg, listener_orientation_.pitch_deg, listener_orientation_.roll_deg},
            warned_,
            output.first(required));
        if (!report) {
            return tl::unexpected{report.error()};
        }
        for (size_t i = 0; i < elements_.size(); ++i) {
            elements_[i].target = staged_[i].target;
            elements_[i].initialized = staged_[i].initialized;
        }
        warned_ = report->mask;
        if (diagnostics_) {
            for (const auto& record : std::span{report->records}.first(report->count)) {
                publish(frame, record);
            }
        }
        return {};
    }
    [[nodiscard]] uint32_t output_channels() const noexcept override { return 2; }
    [[nodiscard]] uint32_t sample_rate() const noexcept override { return config_.sample_rate; }
    [[nodiscard]] uint32_t tail_input_frames() const noexcept override {
        return static_cast<uint32_t>(state_->fft_size - 1) + 32U;
    }
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        listener_orientation_ = orientation;
    }

  private:
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    // Commands as handed to the Rust session; the state copies the effective rows, so only the
    // C++-derived DirectSpeakers direction and the timing words are recorded.
    void trace_commands(const Frame& frame) const {
        std::vector<int> commands;
        std::vector<float> directions;
        for (const auto* list : {&initial_, &events_}) {
            for (const auto& command : *list) {
                commands.insert(commands.end(),
                                {static_cast<int>(command.element),
                                 static_cast<int>(command.offset),
                                 static_cast<int>(command.duration),
                                 static_cast<int>(command.has_direction),
                                 static_cast<int>(command.diagnostic),
                                 static_cast<int>(static_cast<uint32_t>(command.changed)),
                                 static_cast<int>(static_cast<uint32_t>(command.cleared))});
                directions.insert(directions.end(), {command.direction[0], command.direction[1]});
            }
        }
        if (!commands.empty()) {
            const auto key = consistency::scene_renderer_key(frame) + ".30-binaural";
            consistency::dump(key + "-commands.i32", commands);
            consistency::dump(key + "-directions.f32", directions);
        }
    }
#endif
    Result<void> prepare_initials(const Frame& frame) {
        staged_.resize(elements_.size());
        planes_.assign(elements_.size(), {});
        initial_.clear();
        events_.clear();
        for (size_t i = 0; i < elements_.size(); ++i) {
            staged_[i] = {elements_[i].target, elements_[i].initialized, false, false};
        }
        for (const auto& initial : frame.initial_states) {
            const auto found = element_index_.find(initial.element_id);
            if (found == element_index_.end() || staged_[found->second].seen_initial || !valid_state(initial.state)) {
                return make_error(ErrorCode::invalid_argument, "Live binaural initial state is invalid or duplicated");
            }
            const auto index = found->second;
            auto& control = staged_[index];
            control.seen_initial = true;
            control.target = default_state(elements_[index].descriptor);
            copy_state_fields(control.target, initial.state, initial.state.valid_fields);
            auto command = make_command(index, control.target);
            if (!command) {
                return tl::unexpected{command.error()};
            }
            initial_.push_back(*command);
            control.initialized = true;
        }
        for (size_t i = 0; i < elements_.size(); ++i) {
            if (!staged_[i].initialized) {
                auto command = make_command(i, staged_[i].target);
                if (!command) {
                    return tl::unexpected{command.error()};
                }
                initial_.push_back(*command);
                staged_[i].initialized = true;
            }
        }
        return {};
    }
    Result<void> prepare_planes(const Frame& frame) {
        for (const auto& plane : frame.pcm) {
            const auto found = element_index_.find(plane.element_id);
            if (found == element_index_.end() || staged_[found->second].seen_pcm ||
                (plane.has_signal && plane.samples.size() < frame.duration_samples)) {
                return make_error(ErrorCode::invalid_argument, "Live binaural PCM view is invalid or duplicated");
            }
            staged_[found->second].seen_pcm = true;
            planes_[found->second] = {plane.samples.data(), plane.samples.size(), plane.has_signal ? 1U : 0U, 0};
        }
        return {};
    }
    Result<void> prepare_events(const Frame& frame) {
        uint32_t previous = 0;
        for (const auto& event : frame.updates) {
            const auto found = element_index_.find(event.element_id);
            if (found == element_index_.end() || event.offset_samples < previous ||
                event.offset_samples >= frame.duration_samples ||
                ((event.changed_fields | event.cleared_fields) & ~k_known_state_fields) != 0U ||
                (event.changed_fields & ~event.state.valid_fields) != 0U || !valid_state(event.state)) {
                return make_error(ErrorCode::invalid_argument, "Live binaural metadata event is invalid or unordered");
            }
            previous = event.offset_samples;
            auto& control = staged_[found->second];
            control.target.valid_fields &= ~event.cleared_fields;
            copy_state_fields(control.target, event.state, event.changed_fields);
            auto command = make_command(found->second, control.target);
            if (!command) {
                return tl::unexpected{command.error()};
            }
            command->offset = event.offset_samples;
            command->changed = event.changed_fields;
            command->cleared = event.cleared_fields;
            command->duration = config_.object_smoothing_frames;
            if (event.jump_position) {
                command->duration = 0;
            } else if (event.ramp_duration_samples != 0) {
                command->duration = event.ramp_duration_samples;
            }
            events_.push_back(*command);
        }
        return {};
    }
    [[nodiscard]] uint32_t numerical_spread() const noexcept {
        if (config_.binaural_spread_mode == BinauralSpreadMode::none) {
            return 1U;
        }
        if (config_.binaural_spread_mode == BinauralSpreadMode::saf_spreader) {
            return 2U;
        }
        return 0U;
    }
    Result<MradmLiveBinauralCommand> make_command(size_t index, const ObjectState& state) {
        MradmLiveBinauralCommand result{};
        result.element = static_cast<uint32_t>(index);
        result.state = numerical_state(state);
        const auto& descriptor = elements_[index].descriptor;
        if (descriptor.role != ElementRole::direct_speaker) {
            return result;
        }
        std::optional<std::pair<float, float>> direction;
        if ((state.valid_fields & state_position) != 0U) {
            direction = cartesian_to_polar(state.x, state.y, state.z);
        } else {
            if (!descriptor.speaker_label.empty()) {
                direction = label_position(descriptor.speaker_label);
                if (!direction) {
                    result.diagnostic = 1;
                }
            }
            if (!direction && descriptor.has_position) {
                direction = cartesian_to_polar(descriptor.x, descriptor.y, descriptor.z);
            }
        }
        if (!direction) {
            return make_error(ErrorCode::unsupported,
                              fmt::format("DirectSpeakers element {} has neither a known label nor a position",
                                          descriptor.element_id));
        }
        result.has_direction = 1;
        result.direction[0] = direction->first;
        result.direction[1] = direction->second;
        return result;
    }
    [[nodiscard]] static std::tuple<DiagnosticCode, uint64_t, const char*> diagnostic_fields(uint32_t kind) {
        switch (kind) {
        case 1:
            return {DiagnosticCode::direct_speaker_fallback,
                    0U,
                    "DirectSpeakers label is unknown; using its canonical fixed position"};
        case 2:
            return {DiagnosticCode::semantic_degraded,
                    state_screen_reference,
                    "live binaural has no screen transform; canonical position is used"};
        case 3:
            return {DiagnosticCode::semantic_degraded, state_extent, "binaural spread mode is none; extent is ignored"};
        case 4:
            return {DiagnosticCode::semantic_degraded,
                    state_extent,
                    "dynamic extent uses the live HRTF cloud path; SAF spreader topology is fixed"};
        default:
            std::terminate();
        }
    }
    void publish(const Frame& frame, const MradmLiveBinauralDiagnostic& value) {
        const auto [code, field, message] = diagnostic_fields(value.kind);
        diagnostics_({LogLevel::warning,
                      code,
                      frame.epoch_id,
                      frame.generation_id,
                      elements_[value.element].descriptor.element_id,
                      field,
                      message});
    }
    RendererConfig config_;
    std::shared_ptr<const BinauralState> state_;
    DiagnosticSink diagnostics_;
    dsp::LiveBinauralSession session_;
    uint64_t generation_id_{0};
    std::vector<RuntimeElement> elements_;
    std::unordered_map<uint64_t, size_t> element_index_;
    std::vector<StagedControl> staged_;
    std::vector<MradmLiveBinauralCommand> initial_, events_;
    std::vector<MradmLiveBinauralPlane> planes_;
    uint32_t warned_{0};
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
