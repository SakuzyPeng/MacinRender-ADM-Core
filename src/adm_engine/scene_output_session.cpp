#include "scene_output_session.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

#include "../adm_render_common/speaker_layouts.h"

namespace mradm::realtime {

SceneOutputSession::SceneOutputSession(std::shared_ptr<SceneStreamEngine> stream,
                                       std::unique_ptr<IAudioOutputDevice> device,
                                       bool protect_stereo)
    : stream_(std::move(stream)), device_(std::move(device)), channels_(stream_->output_format().channels) {
    if (protect_stereo && channels_ == 2U) {
        peak_guard_ = std::make_unique<StereoPeakGuard>(stream_->output_format().sample_rate);
        peak_input_.resize(StereoPeakGuard::k_pull_frames * 2U);
    }
}

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
    return create_with_device(stream, std::move(device), config.kind != SceneOutputKind::system_spatial);
}

Result<std::unique_ptr<SceneOutputSession>> SceneOutputSession::create_with_device(
    const std::shared_ptr<SceneStreamEngine>& stream, std::unique_ptr<IAudioOutputDevice> device, bool protect_stereo) {
    if (!stream || !device) {
        return make_error(ErrorCode::invalid_argument, "Scene output requires a stream and device");
    }
    const auto format = stream->output_format();
    if (!stream->attach_output()) {
        return make_error(ErrorCode::invalid_argument, "Scene stream already has an output consumer");
    }
    std::unique_ptr<SceneOutputSession> session;
    try {
        session.reset(new SceneOutputSession(stream, std::move(device), protect_stereo));
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
    if (peak_guard_) {
        (*peak_guard_).reset();
        peak_source_ended_ = false;
    }
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
    const auto result = peak_guard_ ? pull_stereo(output, count) : stream_->pull(output.data(), count);
    consumed_.store(result.first_media_frame + result.media_frames);
    buffering_.store((result.flags & (scene_pull_buffering | scene_pull_underrun)) != 0U);
    failed_.store((result.flags & scene_pull_failed) != 0U);
    if ((result.flags & scene_pull_eos) != 0U) {
        eos_.store(true);
        device_->mark_end();
    }
    if (!peak_guard_ && !device_->has_device_volume()) {
        const float gain = volume_.load();
        for (std::size_t i = 0; i < static_cast<std::size_t>(result.media_frames) * channels_; ++i) {
            output[i] *= gain;
        }
    }
    pulls_.fetch_sub(1U, std::memory_order_seq_cst);
    return result.media_frames;
}

ScenePullResult SceneOutputSession::pull_stereo(std::span<float> output, std::uint32_t frames) noexcept {
    ScenePullResult result;
    result.first_media_frame = consumed_.load();
    result.requested_frames = frames;
    const float volume = volume_.load();
    while (result.media_frames < frames) {
        const auto destination = output.subspan(static_cast<std::size_t>(result.media_frames) * 2U,
                                                static_cast<std::size_t>(frames - result.media_frames) * 2U);
        const auto produced = peak_guard_->pop(destination, volume, peak_source_ended_);
        result.media_frames += static_cast<std::uint32_t>(produced);
        if (result.media_frames == frames || peak_source_ended_) {
            break;
        }
        const auto needed = static_cast<std::size_t>(frames - result.media_frames) + peak_guard_->lookahead_frames() -
                            peak_guard_->buffered_frames();
        const auto stream_status = stream_->status();
        // Prefetch only available PCM. A short speculative read must not count
        // as an underrun; the one-frame probe also discovers EOS on an empty ring.
        const auto request =
            std::min({StereoPeakGuard::k_pull_frames,
                      peak_guard_->writable_frames(),
                      needed,
                      std::max(std::size_t{1U}, static_cast<std::size_t>(stream_status.buffered_output_frames))});
        const auto pulled = stream_->pull(peak_input_.data(), static_cast<std::uint32_t>(request));
        result.epoch_id = pulled.epoch_id;
        result.flags |= pulled.flags & (scene_pull_buffering | scene_pull_underrun | scene_pull_failed);
        peak_source_ended_ = (pulled.flags & scene_pull_eos) != 0U;
        peak_guard_->push(std::span{peak_input_}.first(static_cast<std::size_t>(pulled.media_frames) * 2U));
        if (pulled.media_frames == 0U && !peak_source_ended_) {
            break;
        }
    }
    if (peak_source_ended_ && peak_guard_->buffered_frames() == 0U) {
        result.flags |= scene_pull_eos;
    }
    return result;
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
