#include "render_common.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include "scene_math.h"
#include "speaker_pcm.h"

namespace mradm::render_common {

namespace {

// The routing rules live in Rust mradm-render `routing` (PCM consistency 4.5); these wrappers
// keep the C++ interface for the backends that have not moved yet and for the Apple island.
[[nodiscard]] std::string string_of(MradmAdmText value) {
    return {reinterpret_cast<const char*>(value.data), value.len};
}

[[nodiscard]] std::vector<MradmAdmText> texts(const std::vector<std::string>& labels) {
    std::vector<MradmAdmText> result;
    result.reserve(labels.size());
    std::ranges::transform(labels, std::back_inserter(result), [](const std::string& l) { return text(l); });
    return result;
}

[[nodiscard]] std::vector<MradmRenderTarget> routing_targets(std::span<const DirectSpeakerRoutingTarget> targets) {
    std::vector<MradmRenderTarget> result;
    result.reserve(targets.size());
    std::ranges::transform(targets, std::back_inserter(result), [](const DirectSpeakerRoutingTarget& t) {
        return MradmRenderTarget{text(t.label), t.is_lfe ? 1U : 0U};
    });
    return result;
}

[[nodiscard]] std::string label_rule(uint32_t kind, std::string_view raw) {
    std::string out(raw.size() + 8, '\0');
    std::size_t len = out.size();
    if (mradm_render_label(kind,
                           reinterpret_cast<const uint8_t*>(raw.data()),
                           raw.size(),
                           reinterpret_cast<uint8_t*>(out.data()),
                           &len) != 0) {
        throw std::logic_error("speaker label rule failed");
    }
    out.resize(len);
    return out;
}

// Rust error text: fixed wording plus input bytes, so `extra` (the inputs' size) always fits.
class ErrorText {
  public:
    explicit ErrorText(std::size_t extra) : message_(extra + 512), context_(extra + 512) {}
    uint8_t* message() noexcept { return message_.data(); }
    uint8_t* context() noexcept { return context_.data(); }
    // The capacity going in, the length written coming out.
    std::size_t* message_len() noexcept { return &(message_len_ = message_.size()); }
    std::size_t* context_len() noexcept { return &(context_len_ = context_.size()); }
    [[nodiscard]] tl::unexpected<Error> error(int32_t code) const {
        return make_error(static_cast<ErrorCode>(code),
                          std::string{reinterpret_cast<const char*>(message_.data()), message_len_},
                          std::string{reinterpret_cast<const char*>(context_.data()), context_len_});
    }

  private:
    std::vector<uint8_t> message_;
    std::vector<uint8_t> context_;
    std::size_t message_len_{0};
    std::size_t context_len_{0};
};

[[nodiscard]] std::size_t total_size(const std::vector<std::string>& labels) {
    std::size_t size = 0;
    for (const auto& label : labels) {
        size += label.size() + 1;
    }
    return size;
}

} // namespace

std::string normalise_speaker_label_key(std::string_view raw) {
    return label_rule(0U, raw);
}

std::string canonicalise_speaker_label(std::string_view raw) {
    return label_rule(1U, raw);
}

std::string canonical_direct_speaker_label(std::string_view raw) {
    return label_rule(2U, raw);
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
    const auto records = routing_targets(targets);
    const auto label_texts = texts(labels);
    std::size_t index = 0;
    if (mradm_render_target_index(records.data(), records.size(), label_texts.data(), label_texts.size(), &index) ==
        0U) {
        return std::nullopt;
    }
    return index;
}

Result<ResolvedDirectSpeakersMatrix>
resolve_direct_speakers_matrix_targets(const DirectSpeakersMatrix& matrix,
                                       std::span<const DirectSpeakerRoutingTarget> targets,
                                       std::string_view layout_id) {
    const MatrixRecords routes(matrix);
    const auto records = routing_targets(targets);
    MradmRenderMatrix* raw = nullptr;
    MradmRenderMatrixView view{};
    ErrorText error(layout_id.size() + routes.label_bytes);
    const auto code = mradm_render_matrix_resolve(routes.routes.data(),
                                                  routes.routes.size(),
                                                  records.data(),
                                                  records.size(),
                                                  text(layout_id),
                                                  &raw,
                                                  &view,
                                                  error.message(),
                                                  error.message_len(),
                                                  error.context(),
                                                  error.context_len());
    if (code != 0) {
        return error.error(code);
    }
    ResolvedDirectSpeakersMatrix resolved;
    resolved.handle.reset(raw, &mradm_render_matrix_destroy);
    for (const auto& route : std::span{view.routes, view.routes_len}) {
        auto& output = resolved.routes.emplace_back();
        output.source_label = string_of(route.source_label);
        output.source_key = string_of(route.source_key);
        output.mute = route.mute != 0U;
        for (const auto& target : std::span{route.targets, route.targets_len}) {
            output.targets.push_back({target.output, target.gain});
        }
    }
    return resolved;
}

Result<const ResolvedDirectSpeakersMatrixRoute*>
direct_speakers_matrix_route_for_block(const ResolvedDirectSpeakersMatrix& matrix,
                                       const SceneDirectSpeakersBlock& block) {
    const auto label_texts = texts(block.speaker_labels);
    std::size_t index = 0;
    ErrorText error(total_size(block.speaker_labels));
    const auto code = mradm_render_matrix_route(matrix.handle.get(),
                                                label_texts.data(),
                                                label_texts.size(),
                                                &index,
                                                error.message(),
                                                error.message_len(),
                                                error.context(),
                                                error.context_len());
    if (code != 0) {
        return error.error(code);
    }
    return &matrix.routes.at(index);
}

std::optional<DirectSpeakerPosition> direct_speaker_position_for_labels(const std::vector<std::string>& labels) {
    const auto label_texts = texts(labels);
    DirectSpeakerPosition position;
    if (mradm_render_label_position(label_texts.data(), label_texts.size(), &position.azimuth, &position.elevation) ==
        0U) {
        return std::nullopt;
    }
    return position;
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

namespace {

// Bit 1: gain written; bit 2: head_locked written.
uint32_t live_override(const LiveOverrides& overrides,
                       std::string_view object_id,
                       std::string_view key,
                       float& gain,
                       uint32_t& head_locked) {
    const auto records = override_records(overrides);
    return mradm_render_live_override(records.data(), records.size(), text(object_id), text(key), &gain, &head_locked);
}

} // namespace

std::optional<float> resolve_live_channel_gain(const LiveOverrides& overrides,
                                               std::string_view object_id,
                                               std::string_view channel_label_key) {
    float gain = 0.0F;
    uint32_t head_locked = 0U;
    return (live_override(overrides, object_id, channel_label_key, gain, head_locked) & 1U) != 0U ? std::optional{gain}
                                                                                                  : std::nullopt;
}

std::optional<bool> resolve_live_head_locked(const LiveOverrides& overrides,
                                             std::string_view object_id,
                                             std::string_view channel_label_key) {
    float gain = 0.0F;
    uint32_t head_locked = 0U;
    return (live_override(overrides, object_id, channel_label_key, gain, head_locked) & 2U) != 0U
               ? std::optional{head_locked != 0U}
               : std::nullopt;
}

LiveGainRamp::LiveGainRamp(uint32_t sample_rate, uint32_t ramp_ms) : bank_(1U, sample_rate, ramp_ms) {}

void LiveGainRamp::reset() noexcept {
    bank_.reset();
}

void LiveGainRamp::set_target(float target) noexcept {
    bank_.set_targets(std::span{&target, 1U});
}

float LiveGainRamp::next() noexcept {
    float value = 0.0F;
    fill(std::span{&value, 1U});
    return value;
}

void LiveGainRamp::fill(std::span<float> output) noexcept {
    bank_.fill(output);
}

InterleavedLiveGainSmoother::InterleavedLiveGainSmoother(std::size_t channels, uint32_t sample_rate)
    : channels_(channels), bank_(channels, sample_rate, LiveGainRamp::k_default_ramp_ms) {}

void InterleavedLiveGainSmoother::set_targets(std::span<const float> targets) noexcept {
    bank_.set_targets(targets);
}

void InterleavedLiveGainSmoother::apply(float* interleaved, std::size_t frames) noexcept {
    if (frames > std::numeric_limits<std::size_t>::max() / channels_) {
        std::terminate();
    }
    bank_.apply(std::span{interleaved, frames * channels_});
}

void InterleavedLiveGainSmoother::fill(std::span<float> output) noexcept {
    bank_.fill(output);
}

void InterleavedLiveGainSmoother::reset() noexcept {
    bank_.reset();
}

bool is_lfe_label(std::string_view raw) noexcept {
    return mradm_render_is_lfe(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) != 0U;
}

bool direct_speakers_block_is_lfe(const SceneDirectSpeakersBlock& block) noexcept {
    return direct_speakers_lfe_target(block) != LfeTarget::none;
}

LfeTarget direct_speakers_lfe_target(const SceneDirectSpeakersBlock& block) noexcept {
    std::vector<MradmAdmText> labels;
    try {
        labels = texts(block.speaker_labels);
    } catch (...) {
        std::terminate();
    }
    return static_cast<LfeTarget>(
        mradm_render_lfe_target(labels.data(), labels.size(), block.low_pass_hz.has_value() ? 1U : 0U));
}

namespace {

[[nodiscard]] MradmRenderLfeRouting lfe_record(const LfeRoutingPlan& plan) noexcept {
    MradmRenderLfeRouting record{};
    record.applies_to_22_2 = plan.applies_to_22_2 ? 1U : 0U;
    record.split_power = plan.mode == LfeRoutingMode::split_power ? 1U : 0U;
    record.has_lfe1 = plan.has_lfe1 ? 1U : 0U;
    record.has_lfe2 = plan.has_lfe2 ? 1U : 0U;
    return record;
}

} // namespace

float LfeRoutingPlan::gain(LfeTarget input, LfeTarget output) const noexcept {
    const auto record = lfe_record(*this);
    return mradm_render_lfe_gain(&record, static_cast<uint32_t>(input), static_cast<uint32_t>(output));
}

Result<LfeRoutingPlan> resolve_lfe_routing(const RenderPlan& plan, LogSink& logs, std::string_view log_module) {
    std::vector<uint32_t> targets;
    for (const auto& object : plan.scene.objects) {
        for (const auto& track : object.tracks) {
            for (const auto* block : last_blocks_at_each_start(track.ds_blocks)) {
                targets.push_back(static_cast<uint32_t>(direct_speakers_lfe_target(*block)));
            }
        }
    }
    MradmRenderLfeRouting record{};
    ErrorText error(0);
    const auto code = mradm_render_lfe_routing(plan.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U,
                                               text(plan.output_layout),
                                               targets.data(),
                                               targets.size(),
                                               &record,
                                               error.message(),
                                               error.message_len());
    for (const auto& line : std::span{record.logs}.first(record.logs_len)) {
        logs.log(static_cast<LogLevel>(line.level), log_module, string_of(line.message));
    }
    if (code != 0) {
        return error.error(code);
    }
    LfeRoutingPlan routing;
    routing.mode = plan.lfe_routing_mode;
    routing.applies_to_22_2 = record.applies_to_22_2 != 0U;
    routing.has_lfe1 = record.has_lfe1 != 0U;
    routing.has_lfe2 = record.has_lfe2 != 0U;
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

std::vector<ExtentDirection>
extent_disk_cloud(const SceneBlockPosition& position, float width, float height, float depth) {
    const std::array input{position.cartesian ? position.x : position.azimuth,
                           position.cartesian ? position.y : position.elevation,
                           position.cartesian ? position.z : position.distance,
                           width,
                           height,
                           depth,
                           1.0F};
    std::array<MradmSceneCloudPoint, 17> points{};
    size_t count = 0;
    dsp::scene_check(mradm_dsp_scene_cloud(input.data(),
                                           input.size(),
                                           position.cartesian ? 1U : 0U,
                                           dsp::scene_arithmetic_flags,
                                           points.data(),
                                           points.size(),
                                           &count,
                                           nullptr,
                                           0));
    std::vector<ExtentDirection> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.push_back({points.at(i).azimuth, points.at(i).elevation, points.at(i).weight});
    }
    return result;
}

} // namespace mradm::render_common
