#include "monitor_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace mradm::realtime {

namespace {
// Worker production granularity (frames per stream.process() chunk) and ring depth. The
// ring holds ~k_ring_frames of audio ahead of the playhead so the worker's batch DSP +
// disk reads stay off the audio callback's critical path. Both are frame counts; the ring
// stores channels × frames floats.
constexpr uint64_t k_chunk_frames = 1024U;
constexpr std::size_t k_ring_frames = 8192U;
constexpr auto k_idle_nap = std::chrono::milliseconds(1);
} // namespace

MonitorEngine::MonitorEngine(std::unique_ptr<IRenderStream> stream, IAudioOutputDevice& device)
    : stream_(std::move(stream)), device_(device), channels_(stream_->out_channels()),
      sample_rate_(stream_->sample_rate()), ring_(k_ring_frames * channels_),
      scratch_(static_cast<std::size_t>(k_chunk_frames) * channels_, 0.0F) {}

Result<std::unique_ptr<MonitorEngine>> MonitorEngine::create(IRenderStreamFactory& factory,
                                                             IAudioOutputDevice& device,
                                                             const AdmScene& scene,
                                                             const RenderOptions& opts,
                                                             LogSink& logs) {
    auto stream = factory.open(scene, opts, logs);
    if (!stream) {
        return tl::unexpected{stream.error()};
    }
    if (*stream == nullptr) {
        return make_error(ErrorCode::internal_error, "render stream factory returned a null stream");
    }
    if ((*stream)->out_channels() == 0 || (*stream)->sample_rate() == 0) {
        return make_error(ErrorCode::internal_error,
                          "render stream reports an invalid format (0 channels or 0 sample rate)");
    }

    std::unique_ptr<MonitorEngine> engine{new MonitorEngine(std::move(*stream), device)};

    engine->worker_ = std::thread([engine = engine.get()] { engine->worker_loop(); });

    auto started = device.start(
        engine->channels_, engine->sample_rate_, [engine = engine.get()](std::span<float> out, std::size_t frames) {
            return engine->pull(out, frames);
        });
    if (!started) {
        engine->quit_.store(true, std::memory_order_release);
        engine->wake_.notify_all();
        if (engine->worker_.joinable()) {
            engine->worker_.join();
        }
        return tl::unexpected{started.error()};
    }
    return engine;
}

MonitorEngine::~MonitorEngine() {
    device_.stop(); // stop pulls first so no pull() runs after teardown begins
    quit_.store(true, std::memory_order_release);
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void MonitorEngine::play() {
    {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        state_.store(MonitorState::playing, std::memory_order_seq_cst);
    }
    wake_.notify_all();
}

void MonitorEngine::pause() {
    {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        state_.store(MonitorState::paused, std::memory_order_seq_cst);
    }
    wake_.notify_all();
}

void MonitorEngine::seek(uint64_t frame) {
    {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        seek_pending_ = true;
        seek_target_ = frame;
    }
    wake_.notify_all();
}

void MonitorEngine::set_loop(uint64_t start_frame, uint64_t end_frame) {
    // Disable when the range is empty/inverted.
    if (end_frame <= start_frame) {
        loop_end_.store(0, std::memory_order_relaxed);
        loop_start_.store(0, std::memory_order_relaxed);
        return;
    }
    loop_start_.store(start_frame, std::memory_order_relaxed);
    loop_end_.store(end_frame, std::memory_order_relaxed);
}

void MonitorEngine::worker_loop() {
    while (!quit_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(control_mutex_);
            if (seek_pending_) {
                apply_seek_locked(seek_target_);
                seek_pending_ = false;
            }
            const bool producer_done =
                ended_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed);
            // Idle (don't spin on process()) when not playing or the stream is exhausted /
            // failed. A seek re-arms production (clears ended_/failed_) and wakes us.
            if (state_.load(std::memory_order_seq_cst) != MonitorState::playing || producer_done) {
                wake_.wait(lock, [this] {
                    const bool done = ended_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed);
                    return quit_.load(std::memory_order_acquire) || seek_pending_ ||
                           (state_.load(std::memory_order_seq_cst) == MonitorState::playing && !done);
                });
                continue;
            }
        }

        // Produce ahead of the playhead without holding the lock.
        if (!top_up_ring()) {
            std::this_thread::sleep_for(k_idle_nap); // ring full (or nothing to add) — back off
        }
    }
}

bool MonitorEngine::top_up_ring() {
    bool produced = false;
    while (ring_.available_write() >= scratch_.size()) {
        uint64_t frames = k_chunk_frames;
        const uint64_t loop_end = loop_end_.load(std::memory_order_relaxed);
        const uint64_t loop_start = loop_start_.load(std::memory_order_relaxed);
        const bool looping = loop_end > loop_start;
        if (looping && producer_pos_ < loop_end) {
            frames = std::min<uint64_t>(frames, loop_end - producer_pos_);
        }

        auto result = stream_->process(std::span<float>(scratch_.data(), frames * channels_), frames);
        if (!result) {
            failed_.store(true, std::memory_order_relaxed); // render error: stop producing
            return produced;
        }
        const std::size_t got = *result;
        if (got == 0) {
            ended_.store(true, std::memory_order_relaxed); // end of material (non-looping)
            return produced;
        }
        ring_.push(scratch_.data(), got * channels_); // available_write checked: full push
        producer_pos_ += got;
        produced = true;

        if (looping && producer_pos_ >= loop_end) {
            // Producer-side wrap: ring stays valid, no flush needed. seek on a prepared
            // stream is not expected to fail; a worker has no log channel yet (see C ABI
            // step), so the result is intentionally discarded.
            (void) stream_->seek(loop_start);
            producer_pos_ = loop_start;
        }
        if (got < frames) {
            return produced; // stream produced a short block (end)
        }
    }
    return produced;
}

std::size_t MonitorEngine::pull(std::span<float> out, std::size_t frames) {
    const std::size_t floats = frames * channels_;
    in_pop_.store(true, std::memory_order_seq_cst);

    const bool active =
        state_.load(std::memory_order_seq_cst) == MonitorState::playing && !flushing_.load(std::memory_order_seq_cst);
    if (!active) {
        std::fill_n(out.data(), floats, 0.0F);
    } else {
        const std::size_t got = ring_.pop(out.data(), floats);
        if (got < floats) {
            std::fill_n(out.data() + got, floats - got, 0.0F);
            // A short read is an underrun only while the producer should still be feeding
            // us; once the stream has ended/failed, an empty ring is the expected silence.
            const bool producer_done =
                ended_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed);
            if (!producer_done) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        frames_played_.fetch_add(got / channels_, std::memory_order_relaxed);
    }

    // Per-channel peak / RMS over the block (silence included), for the UI meters.
    const std::size_t meter_ch = std::min<std::size_t>(channels_, k_max_level_channels);
    for (std::size_t c = 0; c < meter_ch; ++c) {
        float peak = 0.0F;
        double sumsq = 0.0;
        for (std::size_t f = 0; f < frames; ++f) {
            const float v = out[(f * channels_) + c];
            peak = std::max(peak, std::fabs(v));
            sumsq += static_cast<double>(v) * static_cast<double>(v);
        }
        peak_.at(c).store(peak, std::memory_order_relaxed);
        rms_.at(c).store(frames > 0 ? static_cast<float>(std::sqrt(sumsq / static_cast<double>(frames))) : 0.0F,
                         std::memory_order_relaxed);
    }

    in_pop_.store(false, std::memory_order_seq_cst);
    return frames;
}

void MonitorEngine::apply_seek_locked(uint64_t frame) {
    // Block the audio thread from touching the ring, then wait for any in-flight pop to
    // finish (bounded by one callback). seq_cst on flushing_/in_pop_ gives a total order:
    // a pull either sees flushing_ and skips the ring, or it set in_pop_ first and we wait
    // for it. The worker is the sole producer and is here (not in top_up_ring), so once the
    // consumer is idle the ring has no concurrent access and clear() is safe.
    flushing_.store(true, std::memory_order_seq_cst);
    while (in_pop_.load(std::memory_order_seq_cst)) {
        std::this_thread::yield();
    }
    ring_.clear();
    (void) stream_->seek(frame); // discarded: no worker log channel yet (see C ABI step)
    producer_pos_ = frame;
    frames_played_.store(frame, std::memory_order_relaxed);
    // Seeking re-arms production: a position before EOF has more to render.
    ended_.store(false, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    flushing_.store(false, std::memory_order_seq_cst);
}

MonitorStatus MonitorEngine::status() const {
    MonitorStatus s;
    s.state = state_.load(std::memory_order_seq_cst);
    s.playhead_frames = frames_played_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    const std::size_t cap = ring_.capacity();
    const std::size_t buffered_floats = ring_.available_read();
    s.buffered_frames = channels_ > 0 ? buffered_floats / channels_ : 0;
    s.ring_fill = cap > 0 ? static_cast<float>(buffered_floats) / static_cast<float>(cap) : 0.0F;
    s.ended = ended_.load(std::memory_order_relaxed);
    s.failed = failed_.load(std::memory_order_relaxed);
    return s;
}

MonitorLevels MonitorEngine::levels() const {
    MonitorLevels l;
    l.channels = std::min<uint32_t>(channels_, static_cast<uint32_t>(k_max_level_channels));
    for (std::size_t c = 0; c < l.channels; ++c) {
        l.peak.at(c) = peak_.at(c).load(std::memory_order_relaxed);
        l.rms.at(c) = rms_.at(c).load(std::memory_order_relaxed);
    }
    return l;
}

} // namespace mradm::realtime
