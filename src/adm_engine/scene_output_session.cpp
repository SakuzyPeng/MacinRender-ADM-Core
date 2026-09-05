#include "scene_output_session.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

#include "../adm_render_common/speaker_layouts.h"

namespace mradm::realtime {

SceneOutputSession::SceneOutputSession(std::shared_ptr<SceneStreamEngine> stream,
                                       std::unique_ptr<IAudioOutputDevice> device)
    : stream_(std::move(stream)), device_(std::move(device)), channels_(stream_->output_format().channels) {}

Result<std::unique_ptr<SceneOutputSession>> SceneOutputSession::create(const std::shared_ptr<SceneStreamEngine>& stream,
                                                                       const SceneDeviceConfig& config) {
    if (!stream) {
        return make_error(ErrorCode::invalid_argument, "Scene output requires a stream");
    }
    const auto format = stream->output_format();
    std::unique_ptr<IAudioOutputDevice> device;
    if (config.kind == SceneOutputKind::system_spatial) {
        const auto* layout = render_layouts::find_speaker_layout(config.layout, config.geometry);
        if (layout == nullptr || layout->speakers.size() != format.channels) {
            return make_error(ErrorCode::invalid_argument, "Scene output layout does not match the rendered channels");
        }
#ifdef __APPLE__
        device = make_avsamplebuffer_device(config.layout);
#elif defined(_WIN32)
        device = make_spatialaudioclient_device(config.layout, config.geometry);
#else
        return make_error(ErrorCode::unsupported, "System spatial output is unavailable on this platform");
#endif
    } else {
        if (config.kind == SceneOutputKind::stereo && format.channels != 2U) {
            return make_error(ErrorCode::invalid_argument, "Stereo output requires two rendered channels");
        }
        device = make_miniaudio_device(config.kind == SceneOutputKind::null_device, config.device_id);
    }
    if (!stream->attach_output()) {
        return make_error(ErrorCode::invalid_argument, "Scene stream already has an output consumer");
    }
    std::unique_ptr<SceneOutputSession> session;
    try {
        session.reset(new SceneOutputSession(stream, std::move(device)));
    } catch (...) {
        stream->detach_output();
        throw;
    }
    auto* ptr = session.get();
    auto started = session->device_->start(
        format.channels, format.sample_rate, [ptr](std::span<float> output, std::size_t frames) {
            return ptr->pull(output, frames);
        });
    if (!started) {
        return tl::unexpected(started.error());
    }
    session->device_->pause();
    return session;
}

SceneOutputSession::~SceneOutputSession() {
    playing_.store(false);
    device_->stop();
    stream_->detach_output();
}

void SceneOutputSession::park() {
    playing_.store(false, std::memory_order_seq_cst);
    while (pulls_.load(std::memory_order_seq_cst) != 0U) {
        std::this_thread::yield();
    }
    device_->pause();
}

void SceneOutputSession::play() {
    const std::lock_guard lock(control_);
    playing_.store(true, std::memory_order_seq_cst);
    device_->resume();
}

void SceneOutputSession::pause() {
    const std::lock_guard lock(control_);
    park();
}

Result<void> SceneOutputSession::begin_epoch(std::uint64_t epoch, std::int64_t target) {
    const std::lock_guard lock(control_);
    // Reject a stale epoch before disturbing the current device queue.
    if (epoch <= stream_->status().epoch_id) {
        return make_error(ErrorCode::invalid_argument, "Scene output epoch must increase");
    }
    park();
    device_->flush();
    auto reset = stream_->begin_epoch(epoch, target);
    if (!reset) {
        return reset;
    }
    consumed_.store(0);
    presented_.store(0);
    eos_.store(false);
    failed_.store(false);
    buffering_.store(true);
    return {};
}

Result<void> SceneOutputSession::set_volume(float gain) {
    if (!std::isfinite(gain) || gain < 0.0F || gain > 1.0F) {
        return make_error(ErrorCode::invalid_argument, "Scene output volume must be finite and in [0, 1]");
    }
    const std::lock_guard lock(control_);
    volume_.store(gain);
    device_->set_volume(gain);
    return {};
}

std::size_t SceneOutputSession::pull(std::span<float> output, std::size_t frames) noexcept {
    std::ranges::fill(output, 0.0F);
    pulls_.fetch_add(1U, std::memory_order_seq_cst);
    if (!playing_.load(std::memory_order_seq_cst)) {
        pulls_.fetch_sub(1U, std::memory_order_seq_cst);
        return 0;
    }
    // The previous callback has been submitted. This is a callback-resolution
    // estimate, deliberately distinguished from ASBR's media presentation clock.
    presented_.store(consumed_.load());
    const auto count =
        static_cast<std::uint32_t>(std::min<std::size_t>(frames, std::numeric_limits<std::uint32_t>::max()));
    const auto result = stream_->pull(output.data(), count);
    consumed_.store(result.first_media_frame + result.media_frames);
    buffering_.store((result.flags & (scene_pull_buffering | scene_pull_underrun)) != 0U);
    failed_.store((result.flags & scene_pull_failed) != 0U);
    if ((result.flags & scene_pull_eos) != 0U) {
        eos_.store(true);
        device_->mark_end();
    }
    if (!device_->has_device_volume()) {
        const float gain = volume_.load();
        for (std::size_t i = 0; i < static_cast<std::size_t>(result.media_frames) * channels_; ++i) {
            output[i] *= gain;
        }
    }
    pulls_.fetch_sub(1U, std::memory_order_seq_cst);
    return result.media_frames;
}

SceneDeviceStatus SceneOutputSession::status() const {
    const std::lock_guard lock(control_);
    const auto device = device_->progress();
    const auto stream = stream_->status();
    SceneDeviceStatus result;
    result.epoch_id = stream.epoch_id;
    result.consumed_frames = consumed_.load();
    result.presented_frames =
        std::min(result.consumed_frames, device.has_media_clock ? device.presented_frames : presented_.load());
    result.queued_frames = result.consumed_frames - result.presented_frames;
    result.underruns = device.has_media_clock ? device.underruns : stream.underruns;
    result.media_clock = device.has_media_clock;
    result.recovering = device.recovering;
    result.failed = device.failed || failed_.load() || stream.failed;
    result.ended = eos_.load() && result.queued_frames == 0U && !result.failed;
    if (result.failed) {
        result.state = SceneOutputState::failed;
    } else if (result.ended) {
        result.state = SceneOutputState::ended;
    } else if (!playing_.load()) {
        result.state = SceneOutputState::paused;
    } else if (eos_.load()) {
        result.state = SceneOutputState::draining;
    } else if (buffering_.load()) {
        result.state = SceneOutputState::buffering;
    } else {
        result.state = SceneOutputState::playing;
    }
    return result;
}

} // namespace mradm::realtime
