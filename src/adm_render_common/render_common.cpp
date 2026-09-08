#include "render_common.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace mradm::render_common {

namespace {

struct DsLabelAlias {
    std::string_view canonical;
    std::string_view bs2051;
};

// Non-standard RoomCentric and DAW shorthand labels found in ADM BWF exports.
// Keep this table shared so SAF and Apple make exactly the same routing decision.
// clang-format off
constexpr std::array<DsLabelAlias, 34> k_ds_aliases = {{
    {"RCL",   "M+030"}, {"RCR",   "M-030"}, {"RCC",   "M+000"},
    {"RCLFE", "LFE1"},  {"RCLSS", "M+090"}, {"RCRSS", "M-090"},
    {"RCLRS", "M+135"}, {"RCRRS", "M-135"},
    {"RCLTS", "U+090"}, {"RCRTS", "U-090"},
    {"L",     "M+030"}, {"R",     "M-030"}, {"C",     "M+000"},
    {"LFE",   "LFE1"},  {"LFEL",  "LFE1"},  {"LFER",  "LFE2"},
    {"LS",    "M+090"}, {"RS",    "M-090"},
    {"LSS",   "M+090"}, {"RSS",   "M-090"},
    {"LRS",   "M+135"}, {"RRS",   "M-135"},
    {"LB",    "M+135"}, {"RB",    "M-135"},
    {"LW",    "M+060"}, {"RW",    "M-060"},
    {"CS",    "M+180"},
    {"VHL",   "U+045"}, {"VHR",   "U-045"}, {"VHC",   "U+000"},
    {"TSL",   "U+090"}, {"TSR",   "U-090"},
    {"LTM",   "U+090"}, {"RTM",   "U-090"},
}};
// clang-format on

[[nodiscard]] SceneDirectionVector vec_cross(const SceneDirectionVector& a, const SceneDirectionVector& b) noexcept {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

[[nodiscard]] SceneDirectionVector vec_normalize(const SceneDirectionVector& v) noexcept {
    const float len = std::max(1.0e-6F, canonical_vector_length(v.x, v.y, v.z));
    return {v.x / len, v.y / len, v.z / len};
}

// Inverse of direction_vector_from_polar: recover (azimuth, elevation) in degrees,
// project convention (azimuth +ve = left).
[[nodiscard]] std::pair<float, float> polar_from_direction(const SceneDirectionVector& dir) noexcept {
    constexpr float k_rad2deg = 180.0F / std::numbers::pi_v<float>;
    const float azimuth = std::atan2(-dir.x, dir.y) * k_rad2deg;
    const float elevation = std::atan2(dir.z, std::hypot(dir.x, dir.y)) * k_rad2deg;
    return {azimuth, elevation};
}

[[nodiscard]] std::optional<DirectSpeakerPosition> bs2051_label_position(std::string_view label) noexcept {
    if (label.size() != 5U || (label[1] != '+' && label[1] != '-')) {
        return std::nullopt;
    }

    int magnitude = 0;
    const auto* const begin = label.data() + 2;
    const auto* const end = label.data() + label.size();
    const auto parsed = std::from_chars(begin, end, magnitude);
    if (parsed.ec != std::errc{} || parsed.ptr != end || magnitude > 180) {
        return std::nullopt;
    }
    const auto azimuth = static_cast<float>(label[1] == '+' ? magnitude : -magnitude);
    switch (label[0]) {
    case 'M':
        return DirectSpeakerPosition{azimuth, 0.0F};
    case 'U':
        return DirectSpeakerPosition{azimuth, 30.0F};
    case 'B':
        return DirectSpeakerPosition{azimuth, -30.0F};
    case 'T':
        return magnitude == 0 ? std::optional<DirectSpeakerPosition>{{0.0F, 90.0F}} : std::nullopt;
    default:
        return std::nullopt;
    }
}

} // namespace

std::string normalise_speaker_label_key(std::string_view raw) {
    std::string key;
    key.reserve(raw.size());
    for (const char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return key;
}

std::string canonicalise_speaker_label(std::string_view raw) {
    std::string key;
    key.reserve(raw.size());
    for (const char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-') {
            key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return key;
}

std::string canonical_direct_speaker_label(std::string_view raw) {
    const std::string key = canonicalise_speaker_label(raw);
    const auto alias = // NOLINT(readability-qualified-auto): pointer on libc++, wrapper iterator on MSVC.
        std::ranges::find_if(k_ds_aliases, [&](const DsLabelAlias& entry) { return key == entry.canonical; });
    return alias != k_ds_aliases.end() ? std::string{alias->bs2051} : key;
}

std::string_view direct_speakers_routing_mode_name(DirectSpeakersRoutingMode mode) noexcept {
    switch (mode) {
    case DirectSpeakersRoutingMode::automatic:
        return "auto";
    case DirectSpeakersRoutingMode::label:
        return "label";
    case DirectSpeakersRoutingMode::position:
        return "position";
    case DirectSpeakersRoutingMode::matrix:
        return "matrix";
    }
    return "unknown";
}

std::optional<std::size_t> direct_speaker_index_for_labels(std::span<const DirectSpeakerRoutingTarget> targets,
                                                           const std::vector<std::string>& labels) {
    for (const auto& label : labels) {
        const auto target = std::ranges::find_if(
            targets, [&](const auto& speaker) { return !speaker.label.empty() && speaker.label == label; });
        if (target != targets.end()) {
            return static_cast<std::size_t>(std::distance(targets.begin(), target));
        }
    }

    for (const auto& label : labels) {
        const std::string key = canonicalise_speaker_label(label);
        // libc++ exposes this iterator as a pointer, while MSVC uses a wrapper type.
        const auto alias = // NOLINT(readability-qualified-auto)
            std::ranges::find_if(k_ds_aliases, [&](const DsLabelAlias& entry) { return key == entry.canonical; });
        if (alias == k_ds_aliases.end()) {
            continue;
        }
        const auto target = std::ranges::find_if(
            targets, [&](const auto& speaker) { return !speaker.label.empty() && speaker.label == alias->bs2051; });
        if (target != targets.end()) {
            return static_cast<std::size_t>(std::distance(targets.begin(), target));
        }
    }
    return std::nullopt;
}

Result<ResolvedDirectSpeakersMatrix>
resolve_direct_speakers_matrix_targets(const DirectSpeakersMatrix& matrix,
                                       std::span<const DirectSpeakerRoutingTarget> targets,
                                       std::string_view layout_id) {
    ResolvedDirectSpeakersMatrix resolved;
    resolved.routes.reserve(matrix.routes.size());

    for (const auto& route : matrix.routes) {
        ResolvedDirectSpeakersMatrixRoute output_route;
        output_route.source_label = route.source_label;
        output_route.source_key = canonical_direct_speaker_label(route.source_label);
        output_route.mute = route.mute;
        if (output_route.source_key.empty()) {
            return make_error(ErrorCode::invalid_argument,
                              "DirectSpeakers matrix source_label resolves to an empty label",
                              "source_label=" + route.source_label);
        }
        if (is_lfe_label(output_route.source_key)) {
            return make_error(
                ErrorCode::invalid_argument,
                fmt::format("DirectSpeakers matrix source '{}' is LFE; LFE uses dedicated routing", route.source_label),
                "layout=" + std::string{layout_id});
        }
        if (std::ranges::any_of(resolved.routes,
                                [&](const auto& existing) { return existing.source_key == output_route.source_key; })) {
            return make_error(
                ErrorCode::invalid_argument,
                fmt::format("duplicate DirectSpeakers matrix source '{}' after alias resolution", route.source_label),
                "layout=" + std::string{layout_id});
        }

        output_route.targets.reserve(route.targets.size());
        for (const auto& target : route.targets) {
            const std::string target_key = canonical_direct_speaker_label(target.label);
            if (target_key.empty()) {
                return make_error(ErrorCode::invalid_argument,
                                  "DirectSpeakers matrix target label resolves to an empty label",
                                  "source_label=" + route.source_label);
            }
            if (is_lfe_label(target_key)) {
                return make_error(
                    ErrorCode::invalid_argument,
                    fmt::format("DirectSpeakers matrix target '{}' is LFE; LFE uses dedicated routing", target.label),
                    "source_label=" + route.source_label);
            }

            std::optional<std::size_t> output_index;
            for (std::size_t channel = 0; channel < targets.size(); ++channel) {
                if (targets[channel].label.empty() ||
                    canonical_direct_speaker_label(targets[channel].label) != target_key) {
                    continue;
                }
                if (output_index.has_value()) {
                    return make_error(
                        ErrorCode::invalid_argument,
                        fmt::format("DirectSpeakers matrix target '{}' is ambiguous in output layout '{}'",
                                    target.label,
                                    layout_id),
                        "source_label=" + route.source_label);
                }
                output_index = channel;
            }
            if (!output_index.has_value()) {
                return make_error(ErrorCode::invalid_argument,
                                  fmt::format("DirectSpeakers matrix target '{}' is not present in output layout '{}'",
                                              target.label,
                                              layout_id),
                                  "source_label=" + route.source_label);
            }
            if (targets[*output_index].is_lfe) {
                return make_error(
                    ErrorCode::invalid_argument,
                    fmt::format("DirectSpeakers matrix target '{}' is LFE; LFE uses dedicated routing", target.label),
                    "source_label=" + route.source_label);
            }
            if (std::ranges::any_of(output_route.targets,
                                    [&](const auto& existing) { return existing.output_channel == *output_index; })) {
                return make_error(
                    ErrorCode::invalid_argument,
                    fmt::format("duplicate DirectSpeakers matrix target '{}' after alias resolution", target.label),
                    "source_label=" + route.source_label);
            }
            output_route.targets.push_back({*output_index, target.gain});
        }
        resolved.routes.push_back(std::move(output_route));
    }
    return resolved;
}

Result<const ResolvedDirectSpeakersMatrixRoute*>
direct_speakers_matrix_route_for_block(const ResolvedDirectSpeakersMatrix& matrix,
                                       const SceneDirectSpeakersBlock& block) {
    const ResolvedDirectSpeakersMatrixRoute* match = nullptr;
    for (const auto& label : block.speaker_labels) {
        const std::string key = canonical_direct_speaker_label(label);
        const auto route = std::ranges::find(matrix.routes, key, &ResolvedDirectSpeakersMatrixRoute::source_key);
        if (route == matrix.routes.end()) {
            continue;
        }
        if (match != nullptr && match != &*route) {
            return make_error(ErrorCode::invalid_argument,
                              "DirectSpeakers block matches multiple matrix source rows",
                              fmt::format("labels={}", fmt::join(block.speaker_labels, ",")));
        }
        match = &*route;
    }
    if (match == nullptr) {
        const std::string labels = block.speaker_labels.empty()
                                       ? std::string{"<missing>"}
                                       : fmt::format("{}", fmt::join(block.speaker_labels, ","));
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("DirectSpeakers block label '{}' is not covered by the routing matrix", labels));
    }
    return match;
}

std::optional<DirectSpeakerPosition> direct_speaker_position_for_labels(const std::vector<std::string>& labels) {
    for (const auto& label : labels) {
        const std::string key = canonicalise_speaker_label(label);
        // libc++ exposes this iterator as a pointer, while MSVC uses a wrapper type.
        const auto alias = // NOLINT(readability-qualified-auto)
            std::ranges::find_if(k_ds_aliases, [&](const DsLabelAlias& entry) { return key == entry.canonical; });
        const std::string_view bs2051 = alias != k_ds_aliases.end() ? alias->bs2051 : std::string_view{key};
        if (const auto position = bs2051_label_position(bs2051)) {
            return position;
        }
    }
    return std::nullopt;
}

DirectSpeakerPosition
direct_speaker_position_or_front(const SceneDirectSpeakersBlock& block, LogSink& logs, std::string_view log_module) {
    if (block.has_position) {
        return {block.azimuth, block.elevation};
    }
    logs.log(LogLevel::warning,
             log_module,
             "DirectSpeakers block has no nominal position; using front-centre (azimuth 0, elevation 0)");
    return {};
}

std::optional<float> resolve_live_channel_gain(const LiveOverrides& overrides,
                                               std::string_view object_id,
                                               std::string_view channel_label_key) {
    const LiveObjectOverride* whole = nullptr;
    const LiveObjectOverride* specific = nullptr;
    for (const auto& ov : overrides.objects) {
        if (ov.object_id != object_id) {
            continue;
        }
        if (ov.speaker_label.empty()) {
            whole = &ov; // whole-object override (legacy / Objects / bed-wide)
        } else if (!channel_label_key.empty() && canonicalise_speaker_label(ov.speaker_label) == channel_label_key) {
            specific = &ov; // per-channel override wins over the whole-object one
        }
    }
    const LiveObjectOverride* pick = (specific != nullptr) ? specific : whole;
    if (pick == nullptr) {
        return std::nullopt;
    }
    if (pick->mute) {
        return 0.0F;
    }
    return std::pow(10.0F, pick->gain_db / 20.0F);
}

std::optional<bool> resolve_live_head_locked(const LiveOverrides& overrides,
                                             std::string_view object_id,
                                             std::string_view channel_label_key) {
    const LiveObjectOverride* whole = nullptr;
    const LiveObjectOverride* specific = nullptr;
    for (const auto& ov : overrides.objects) {
        if (ov.object_id != object_id) {
            continue;
        }
        if (!ov.head_locked.has_value()) {
            continue;
        }
        if (ov.speaker_label.empty()) {
            whole = &ov;
        } else if (!channel_label_key.empty() && canonicalise_speaker_label(ov.speaker_label) == channel_label_key) {
            specific = &ov;
        }
    }
    const LiveObjectOverride* pick = (specific != nullptr) ? specific : whole;
    return pick != nullptr ? pick->head_locked : std::nullopt;
}

LiveGainRamp::LiveGainRamp(uint32_t sample_rate, uint32_t ramp_ms) noexcept
    : ramp_frames_(std::max<std::size_t>(
          1U, (static_cast<std::size_t>(sample_rate) * static_cast<std::size_t>(ramp_ms)) / 1000U)) {}

void LiveGainRamp::set_target(float target) noexcept {
    if (target == target_) {
        return;
    }
    target_ = target;
    if (!started_) {
        return;
    }
    if (ramp_frames_ <= 1U || current_ == target_) {
        current_ = target_;
        remaining_frames_ = 0U;
        step_ = 0.0F;
        return;
    }
    remaining_frames_ = ramp_frames_;
    step_ = (target_ - current_) / static_cast<float>(ramp_frames_ - 1U);
}

float LiveGainRamp::next() noexcept {
    if (!started_) {
        started_ = true;
        current_ = target_;
        remaining_frames_ = 0U;
        return current_;
    }

    const float value = current_;
    if (remaining_frames_ > 1U) {
        current_ += step_;
        --remaining_frames_;
    } else if (remaining_frames_ == 1U) {
        current_ = target_;
        remaining_frames_ = 0U;
        step_ = 0.0F;
    }
    return value;
}

InterleavedLiveGainSmoother::InterleavedLiveGainSmoother(std::size_t channels, uint32_t sample_rate) {
    ramps_.reserve(channels);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        ramps_.emplace_back(sample_rate);
    }
}

void InterleavedLiveGainSmoother::set_targets(std::span<const float> targets) noexcept {
    const std::size_t count = std::min(ramps_.size(), targets.size());
    for (std::size_t channel = 0; channel < count; ++channel) {
        ramps_[channel].set_target(targets[channel]);
    }
    for (std::size_t channel = count; channel < ramps_.size(); ++channel) {
        ramps_[channel].set_target(1.0F);
    }
}

void InterleavedLiveGainSmoother::apply(float* interleaved, std::size_t frames) noexcept {
    const std::size_t channels = ramps_.size();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            interleaved[(frame * channels) + channel] *= ramps_[channel].next();
        }
    }
}

bool is_lfe_label(std::string_view raw) noexcept {
    const std::string key = normalise_speaker_label_key(raw);
    return key == "LF" || key.find("LFE") != std::string::npos || key.find("SUB") != std::string::npos ||
           key.find("LOWFREQUENCY") != std::string::npos;
}

bool any_label_is_lfe(const std::vector<std::string>& labels) noexcept {
    return std::ranges::any_of(labels, [](const auto& label) { return is_lfe_label(label); });
}

bool direct_speakers_block_is_lfe(const SceneDirectSpeakersBlock& block) noexcept {
    return direct_speakers_lfe_target(block) != LfeTarget::none;
}

LfeTarget direct_speakers_lfe_target(const SceneDirectSpeakersBlock& block) noexcept {
    // Prefer an explicit second-LFE alias even if the metadata also carries a
    // generic LFE label from another naming layer.
    for (const auto& label : block.speaker_labels) {
        const std::string key = normalise_speaker_label_key(label);
        if (key == "LFE2" || key == "LFER") {
            return LfeTarget::lfe2;
        }
    }
    if (any_label_is_lfe(block.speaker_labels)) {
        return LfeTarget::lfe1;
    }
    return block.low_pass_hz.has_value() ? LfeTarget::lfe1 : LfeTarget::none;
}

float LfeRoutingPlan::gain(LfeTarget input, LfeTarget output) const noexcept {
    if (!applies_to_22_2 || input == LfeTarget::none || output == LfeTarget::none) {
        return 0.0F;
    }
    if (mode == LfeRoutingMode::split_power) {
        return k_lfe_split_power_gain;
    }
    return input == output ? 1.0F : 0.0F;
}

Result<LfeRoutingPlan> resolve_lfe_routing(const RenderPlan& plan, LogSink& logs, std::string_view log_module) {
    LfeRoutingPlan routing;
    routing.mode = plan.lfe_routing_mode;
    routing.applies_to_22_2 = plan.output_layout == "9+10+3";

    if (!routing.applies_to_22_2) {
        if (routing.mode == LfeRoutingMode::split_power) {
            logs.log(LogLevel::warning,
                     log_module,
                     "LFE routing mode 'split-power' only applies to 22.2 ('9+10+3'); using existing LFE routing");
        }
        return routing;
    }

    for (const auto& object : plan.scene.objects) {
        for (const auto& track : object.tracks) {
            for (const auto& block : track.ds_blocks) {
                switch (direct_speakers_lfe_target(block)) {
                case LfeTarget::lfe1:
                    routing.has_lfe1 = true;
                    break;
                case LfeTarget::lfe2:
                    routing.has_lfe2 = true;
                    break;
                case LfeTarget::none:
                    break;
                }
            }
        }
    }

    const std::string_view mode_name =
        routing.mode == LfeRoutingMode::direct ? std::string_view{"direct"} : std::string_view{"split-power"};
    logs.log(LogLevel::info, log_module, std::string{"22.2 LFE routing mode: "} + std::string{mode_name});

    if (routing.mode == LfeRoutingMode::split_power) {
        if (routing.has_lfe1 && routing.has_lfe2) {
            return make_error(ErrorCode::invalid_argument,
                              "22.2 split-power requires a single semantic LFE channel; scene contains both LFE1 "
                              "and LFE2");
        }
        if (!routing.has_lfe1 && !routing.has_lfe2) {
            logs.log(LogLevel::warning,
                     log_module,
                     "22.2 split-power requested but no semantic LFE channel was found; LFE outputs remain silent");
        }
    }
    return routing;
}

SerialWorker::SerialWorker() {
    thread_ = std::thread([this] { run(); });
}

SerialWorker::~SerialWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::future<void> SerialWorker::post(std::function<void()> task) {
    std::packaged_task<void()> packaged(std::move(task));
    std::future<void> future = packaged.get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(packaged));
    }
    cv_.notify_one();
    return future;
}

void SerialWorker::run() {
    for (;;) {
        std::packaged_task<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                return; // stop_ is set and nothing left to drain
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

PreparedObjectBlock prepare_object_block(const SceneObjectBlock& raw_block,
                                         const SceneObject& object,
                                         const std::vector<SceneOutputSpeaker>& speakers,
                                         LogSink& logs,
                                         std::string_view log_module,
                                         bool& screen_ref_warned) {
    SceneObjectBlock block = raw_block;
    if (object.position_offset) {
        block.position = apply_position_offset(block.position, *object.position_offset);
    }
    if (block.screen_ref && !screen_ref_warned) {
        logs.log(LogLevel::warning,
                 log_module,
                 "screenRef requires referenceScreen geometry; rendering block as screenRef=false");
        screen_ref_warned = true;
    }
    block = apply_channel_lock(block, speakers);

    return {
        expand_object_divergence(block),
        raw_block.start_sample,
        std::min(raw_block.end_sample, object.end_sample),
        raw_block.jump_position,
        raw_block.interp_length_samples,
    };
}

ExtentRadii extent_disk_radii(float width, float height, float depth, float distance) {
    // Distance-dependent spread scaling: nearer objects subtend a wider angle.
    const float spread_scale = std::clamp(1.0F / std::max(0.4F, distance), 0.5F, 2.5F);
    const float depth_radius = std::max(0.0F, depth) * 20.0F * spread_scale;
    const float width_radius = (std::max(0.0F, width) * 60.0F * spread_scale) + depth_radius;
    const float height_radius = (std::max(0.0F, height) * 45.0F * spread_scale) + depth_radius;
    return {width_radius, height_radius};
}

std::vector<ExtentDirection>
extent_disk_cloud(const SceneBlockPosition& position, float width, float height, float depth) {
    const auto polar = scene_position_to_polar(position);
    const auto [width_radius, height_radius] = extent_disk_radii(width, height, depth, polar.distance);

    if (width_radius <= 1.0e-4F && height_radius <= 1.0e-4F) {
        return {{polar.azimuth, polar.elevation, 1.0F}};
    }

    constexpr float k_deg2rad = std::numbers::pi_v<float> / 180.0F;

    const SceneDirectionVector center = direction_vector_from_position(position);
    SceneDirectionVector horizontal = vec_cross({0.0F, 0.0F, 1.0F}, center);
    if (std::hypot(horizontal.x, horizontal.y, horizontal.z) < 1.0e-4F) {
        horizontal = {1.0F, 0.0F, 0.0F};
    } else {
        horizontal = vec_normalize(horizontal);
    }
    const SceneDirectionVector vertical = vec_normalize(vec_cross(center, horizontal));

    std::vector<ExtentDirection> cloud;
    cloud.reserve(k_extent_disk_samples.size() - 1U);
    for (const auto& sample : k_extent_disk_samples) {
        if (sample.weight <= 0.0F) {
            continue;
        }
        const float h = std::tan(sample.x * width_radius * k_deg2rad);
        const float v = std::tan(sample.y * height_radius * k_deg2rad);
        const SceneDirectionVector dir = vec_normalize({(center.x + (horizontal.x * h)) + (vertical.x * v),
                                                        (center.y + (horizontal.y * h)) + (vertical.y * v),
                                                        (center.z + (horizontal.z * h)) + (vertical.z * v)});
        const auto [azimuth, elevation] = polar_from_direction(dir);
        cloud.push_back({azimuth, elevation, sample.weight});
    }
    return cloud;
}

} // namespace mradm::render_common
