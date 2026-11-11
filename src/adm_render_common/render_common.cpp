#include "render_common.h"

#include <span>
#include <string>
#include <utility>

#include "engine_records.h"

namespace mradm::render_common {

std::optional<float> resolve_live_channel_gain(const LiveOverrideView& overrides,
                                               std::string_view object_id,
                                               std::string_view channel_label_key) {
    float gain = 0.0F;
    const auto& records = overrides.records;
    return (mradm_render_live_override(
                records.data(), records.size(), text(object_id), text(channel_label_key), &gain, nullptr) &
            1U) != 0U
               ? std::optional{gain}
               : std::nullopt;
}

std::optional<bool> resolve_live_head_locked(const LiveOverrideView& overrides,
                                             std::string_view object_id,
                                             std::string_view channel_label_key) {
    uint32_t head_locked = 0U;
    const auto& records = overrides.records;
    return (mradm_render_live_override(
                records.data(), records.size(), text(object_id), text(channel_label_key), nullptr, &head_locked) &
            2U) != 0U
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

} // namespace mradm::render_common
