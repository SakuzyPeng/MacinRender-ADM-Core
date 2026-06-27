#include "monitor_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ebur128.h>
#include <limits>

namespace mradm::realtime {

namespace {
// Worker production granularity (frames per stream.process() chunk) and ring depth. The
// ring holds ~k_ring_frames of audio ahead of the playhead so the worker's batch DSP +
// disk reads stay off the audio callback's critical path. Both are frame counts; the ring
// stores channels × frames floats.
constexpr uint64_t k_chunk_frames = 1024U;
constexpr std::size_t k_ring_frames = 8192U;
// Push devices (e.g. AVSampleBufferAudioRenderer for system spatial) don't pull on a realtime
// audio callback — they drain into the system media queue and tolerate latency (the system
// spatializer already adds >150 ms). Give the worker a much deeper lead there so an occasional
// DSP/scheduling spike can't drain the ring before the worker recovers. ~512 ms at 48 kHz.
// The low-latency realtime devices keep the shallow ring above.
constexpr std::size_t k_ring_frames_push = 24576U;
// While head tracking is actively driving the listener orientation, cap the realtime worker's lead
// to this shallow depth (~43 ms) instead of filling the whole ring. The head pose only affects
// newly-rendered slices, so the lead time IS the head-tracking latency: a deep lead means already-
// rendered (stale-orientation) audio has to drain before a turn is heard. Shrinking it while tracking
// trades a little underrun cushion for a ~4× more responsive turn; non-tracked monitoring keeps the
// deep lead. Only the low-latency realtime (callback) path honours this — the push / system-spatial
// path tracks at the output stage and is unaffected.
constexpr std::size_t k_tracking_lookahead_frames = 2048U;
// While head tracking, render in smaller chunks so the worker re-reads the pending orientation (and
// the stream re-applies it) at a finer cadence — the listener pose only updates between process()
// calls, so the chunk size is the orientation's output granularity. 512 frames ≈ 10.7 ms (vs the
// 21 ms default). Cheap for the binaural AU; non-tracked monitoring keeps the larger chunk.
constexpr uint64_t k_tracking_chunk_frames = 512U;
// Treat head tracking as active for this long after the last orientation update, so the shallow lead
// persists across the gaps between updates (~30 Hz) yet reverts to the deep lead shortly after
// tracking stops.
constexpr auto k_tracking_active_window = std::chrono::milliseconds(750);
constexpr auto k_idle_nap = std::chrono::milliseconds(1);
// Linear crossfade length for a backend hot-switch (~43 ms at 48 kHz): long enough to mask
// the discontinuity between two renderers, short enough to feel immediate.
constexpr uint64_t k_crossfade_frames = 2048U;
} // namespace

MonitorEngine::MonitorEngine(std::unique_ptr<IRenderStream> stream, IAudioOutputDevice& device, LogSink& logs)
    : stream_(std::move(stream)), device_(device), logs_(logs),
      pull_is_realtime_playback_(device.pull_is_realtime_playback()), channels_(stream_->out_channels()),
      sample_rate_(stream_->sample_rate()),
      output_stage_(stream_->renders_orientation_at_output() && stream_->intermediate_channels() > 0U),
      ring_channels_(output_stage_ ? stream_->intermediate_channels() : channels_),
      ring_((device.pull_is_realtime_playback() ? k_ring_frames : k_ring_frames_push) * ring_channels_),
      scratch_(static_cast<std::size_t>(k_chunk_frames) * ring_channels_, 0.0F),
      scratch_b_(static_cast<std::size_t>(k_chunk_frames) * channels_, 0.0F),
      meter_ring_(output_stage_ ? static_cast<std::size_t>(k_ring_frames) * channels_ : 0U) {
    if (output_stage_) {
        // pull_scratch_ holds one callback's worth of intermediate before render_output; sized for a
        // generous max block (the callback loops if it ever asks for more). meter_scratch_ is the
        // worker's drain buffer for the output tap.
        pull_scratch_.assign(static_cast<std::size_t>(k_ring_frames) * ring_channels_, 0.0F);
        meter_scratch_.assign(static_cast<std::size_t>(k_chunk_frames) * channels_, 0.0F);
    }
    rebuild_meter();
}

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

    std::unique_ptr<MonitorEngine> engine{new MonitorEngine(std::move(*stream), device, logs)};

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
    // Worker is joined and the device stopped, so no thread touches the meter anymore.
    if (meter_ != nullptr) {
        auto* st = static_cast<ebur128_state*>(meter_);
        ebur128_destroy(&st);
        meter_ = nullptr;
    }
}

void MonitorEngine::rebuild_meter() {
    // (Re)create the libebur128 state for the current monitor format. Called at construction
    // and on every seek (so integrated loudness restarts from the new position). MODE_S
    // implies MODE_M, MODE_I implies MODE_M — so momentary / short-term / integrated are all
    // available. Worker-only (construction runs before the worker starts) — no mutex.
    if (meter_ != nullptr) {
        auto* old = static_cast<ebur128_state*>(meter_);
        ebur128_destroy(&old);
        meter_ = nullptr;
    }
    // Reset the loudness snapshots: a new integration window starts here.
    constexpr float k_silence = -std::numeric_limits<float>::infinity();
    momentary_lufs_.store(k_silence, std::memory_order_relaxed);
    shortterm_lufs_.store(k_silence, std::memory_order_relaxed);
    integrated_lufs_.store(k_silence, std::memory_order_relaxed);
    lufs_meter_counter_ = 0;
    integrated_throttle_ = 0;
    if (channels_ == 0 || sample_rate_ == 0) {
        return;
    }
    ebur128_state* st = ebur128_init(channels_, sample_rate_, EBUR128_MODE_S | EBUR128_MODE_I);
    meter_ = st;
    if (st != nullptr && channels_ == 2) {
        ebur128_set_channel(st, 0, EBUR128_LEFT);
        ebur128_set_channel(st, 1, EBUR128_RIGHT);
    }
}

void MonitorEngine::feed_meter(const float* data, std::size_t frames) {
    // Worker-only: add `frames` of the monitored output signal (`data`, channels_ interleaved) to the
    // meter, and at ~10 Hz refresh the lock-free LUFS snapshots that levels() reads — integrated
    // loudness walks history, so don't recompute it every chunk. In single-stage mode `data` is the
    // worker's scratch_; in output-stage it is the callback's rendered-output tap (meter_scratch_).
    if (meter_ == nullptr) {
        return;
    }
    auto* st = static_cast<ebur128_state*>(meter_);
    ebur128_add_frames_float(st, data, frames);
    lufs_meter_counter_ += frames;
    if (lufs_meter_counter_ < sample_rate_ / 10) {
        return;
    }
    lufs_meter_counter_ = 0;
    const auto to_lufs = [](double v) {
        return std::isfinite(v) ? static_cast<float>(v) : -std::numeric_limits<float>::infinity();
    };
    double v = 0.0;
    // momentary (400 ms) / short-term (3 s) are cheap — refresh at ~10 Hz.
    if (ebur128_loudness_momentary(st, &v) == EBUR128_SUCCESS) {
        momentary_lufs_.store(to_lufs(v), std::memory_order_relaxed);
    }
    if (ebur128_loudness_shortterm(st, &v) == EBUR128_SUCCESS) {
        shortterm_lufs_.store(to_lufs(v), std::memory_order_relaxed);
    }
    // integrated (gated, walks the whole history) is the heavy one — refresh at ~1 Hz to keep the
    // worker light during long playback (1 Hz is plenty for the UI integrated-loudness readout).
    if (++integrated_throttle_ >= 10) {
        integrated_throttle_ = 0;
        if (ebur128_loudness_global(st, &v) == EBUR128_SUCCESS) {
            integrated_lufs_.store(to_lufs(v), std::memory_order_relaxed);
        }
    }
}

void MonitorEngine::play() {
    {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        state_.store(MonitorState::playing, std::memory_order_seq_cst);
    }
    wake_.notify_all();
    device_.resume(); // resume a buffered sink's clock (no-op before prefill / for miniaudio)
}

void MonitorEngine::pause() {
    {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        state_.store(MonitorState::paused, std::memory_order_seq_cst);
    }
    wake_.notify_all();
    device_.pause(); // halt a buffered sink's clock so it can't run away during a long pause
}

void MonitorEngine::seek(uint64_t frame) {
    {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        seek_pending_ = true;
        seek_target_ = frame;
    }
    wake_.notify_all();
}

void MonitorEngine::set_overrides(const LiveOverrides& overrides) {
    {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        pending_overrides_ = overrides;
        overrides_pending_ = true;
    }
    wake_.notify_all();
}

void MonitorEngine::set_listener_orientation(const ListenerOrientation& orientation) {
    // Mark head tracking active so the worker switches to the shallow lead (low head-tracking
    // latency). Reverts to the deep lead k_tracking_active_window after updates stop.
    last_orientation_update_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                      std::memory_order_relaxed);
    // Output-stage mode reads this lock-free snapshot on the audio callback (render_output), so the
    // pose enters at playout with no ring-drain delay. The worker-pending path below still runs but
    // is a no-op there (render_output uses the snapshot, not the stream's stored orientation).
    snap_yaw_.store(orientation.yaw_deg, std::memory_order_relaxed);
    snap_pitch_.store(orientation.pitch_deg, std::memory_order_relaxed);
    snap_roll_.store(orientation.roll_deg, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        pending_orientation_ = orientation;
        orientation_pending_ = true;
    }
    wake_.notify_all();
}

ListenerOrientation MonitorEngine::orientation_snapshot() const {
    ListenerOrientation o;
    o.yaw_deg = snap_yaw_.load(std::memory_order_relaxed);
    o.pitch_deg = snap_pitch_.load(std::memory_order_relaxed);
    o.roll_deg = snap_roll_.load(std::memory_order_relaxed);
    return o;
}

bool MonitorEngine::head_tracking_recent() const {
    const std::chrono::steady_clock::time_point last{
        std::chrono::steady_clock::duration{last_orientation_update_ns_.load(std::memory_order_relaxed)}};
    return (std::chrono::steady_clock::now() - last) < k_tracking_active_window;
}

void MonitorEngine::switch_stream(std::unique_ptr<IRenderStream> next) {
    {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        pending_stream_ = std::move(next);
        switch_pending_ = true;
    }
    wake_.notify_all();
}

void MonitorEngine::finalize_crossfade() {
    // Snap to the incoming stream (drop the old one). Worker-side; called when a fade
    // completes, or to settle a mid-flight fade before a seek / a new switch.
    if (xfade_active_) {
        stream_ = std::move(xfade_stream_);
        xfade_active_ = false;
        xfade_pos_ = 0;
    }
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
            if (overrides_pending_) {
                // Hand the snapshot to the stream (gain takes effect on the next block).
                // Cheap; safe to do under the lock since it only updates the stream's
                // override table, not its DSP. Apply even while paused so the revision
                // reflects reality and a subsequent resume already has the right gains.
                // During a crossfade both streams are audible, so apply to both. Remember
                // the snapshot so a later hot-switch can re-apply it to the incoming stream.
                stream_->set_overrides(pending_overrides_);
                if (xfade_active_) {
                    xfade_stream_->set_overrides(pending_overrides_);
                }
                current_overrides_ = pending_overrides_;
                applied_override_revision_.store(pending_overrides_.revision, std::memory_order_relaxed);
                overrides_pending_ = false;
            }
            if (orientation_pending_) {
                // Hand the latest head orientation to the stream(s). Cheap (a global AU param on
                // the Apple binaural backend; ignored elsewhere). Apply even while paused so the
                // pose is right on resume; during a crossfade both streams are audible. Remember
                // it so a later hot-switch re-applies it to the incoming stream.
                stream_->set_listener_orientation(pending_orientation_);
                if (xfade_active_) {
                    xfade_stream_->set_listener_orientation(pending_orientation_);
                }
                current_orientation_ = pending_orientation_;
                orientation_pending_ = false;
            }
            if (switch_pending_) {
                // Begin a crossfade to the incoming backend. Settle any in-flight fade
                // first, then seek the incoming stream to the current playhead so it renders
                // the same frames as the outgoing one, and re-apply the current overrides so
                // the switched backend keeps the user's edits. On seek failure keep the old.
                finalize_crossfade();
                // Output-stage holds intermediate in the ring (not blendable as output), so a switch
                // there starts the incoming stream at the playhead (not the read-ahead position).
                const uint64_t switch_to =
                    output_stage_ ? frames_played_.load(std::memory_order_relaxed) : producer_pos_;
                if (auto r = pending_stream_->seek(switch_to); r) {
                    pending_stream_->set_overrides(current_overrides_);
                    pending_stream_->set_listener_orientation(current_orientation_);
                    if (output_stage_) {
                        // Hard cut: park the callback, swap the stream, drop the ring so it refills
                        // from the new stream at the playhead. (Crossfade is unsupported here.)
                        flushing_.store(true, std::memory_order_seq_cst);
                        while (in_pop_.load(std::memory_order_seq_cst)) {
                            std::this_thread::yield();
                        }
                        stream_ = std::move(pending_stream_);
                        producer_pos_ = switch_to;
                        consumer_src_pos_ = switch_to;
                        ring_.clear();
                        meter_ring_.clear();
                        ended_.store(false, std::memory_order_relaxed);
                        failed_.store(false, std::memory_order_relaxed);
                        flushing_.store(false, std::memory_order_seq_cst);
                    } else {
                        xfade_stream_ = std::move(pending_stream_);
                        xfade_active_ = true;
                        xfade_pos_ = 0;
                        ended_.store(false, std::memory_order_relaxed); // incoming re-arms production
                        failed_.store(false, std::memory_order_relaxed);
                    }
                } else {
                    logs_.log(LogLevel::error, "monitor", r.error().message);
                    pending_stream_.reset();
                }
                switch_pending_ = false;
            }
            const bool producer_done =
                ended_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed);
            // Idle (don't spin on process()) when not playing or the stream is exhausted /
            // failed. A seek re-arms production (clears ended_/failed_) and wakes us; a
            // pending override / switch wakes us too so it applies promptly even while paused.
            if (state_.load(std::memory_order_seq_cst) != MonitorState::playing || producer_done) {
                wake_.wait(lock, [this] {
                    const bool done = ended_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed);
                    return quit_.load(std::memory_order_acquire) || seek_pending_ || overrides_pending_ ||
                           orientation_pending_ || switch_pending_ ||
                           (state_.load(std::memory_order_seq_cst) == MonitorState::playing && !done);
                });
                continue;
            }
        }

        // Output-stage: drain the callback's rendered-output tap to keep the LUFS meter fed (no-op
        // otherwise), and surface a callback render error (the affected block was already silenced).
        if (output_stage_) {
            drain_meter_ring();
            if (stream_->render_failed() && !failed_.load(std::memory_order_relaxed)) {
                failed_.store(true, std::memory_order_relaxed);
                logs_.log(LogLevel::error, "monitor", "apple 输出级渲染失败(AudioUnit 错误),已静音并停止");
            }
        }
        // Produce ahead of the playhead without holding the lock.
        if (!top_up_ring()) {
            std::this_thread::sleep_for(k_idle_nap); // ring full (or nothing to add) — back off
        }
    }
}

bool MonitorEngine::top_up_ring_output_stage() {
    // Output-stage producer: fill the ring with orientation-INDEPENDENT intermediate (source PCM)
    // via produce_intermediate. No crossfade and no meter here (the callback renders the output and
    // taps it for the meter); a deep lead is fine because the head pose is applied at the callback,
    // so the lead time no longer gates head-tracking latency.
    bool produced = false;
    std::size_t budget = (ring_.capacity() / scratch_.size()) + 1;
    while (ring_.available_write() >= scratch_.size() && budget-- > 0) {
        uint64_t frames = k_chunk_frames;
        const uint64_t loop_end = loop_end_.load(std::memory_order_relaxed);
        const uint64_t loop_start = loop_start_.load(std::memory_order_relaxed);
        const bool looping = loop_end > loop_start;
        if (looping && producer_pos_ < loop_end) {
            frames = std::min<uint64_t>(frames, loop_end - producer_pos_);
        }
        auto result = stream_->produce_intermediate(std::span<float>(scratch_.data(), frames * ring_channels_), frames);
        if (!result) {
            failed_.store(true, std::memory_order_relaxed);
            logs_.log(LogLevel::error, "monitor", result.error().message);
            return produced;
        }
        const std::size_t got = *result;
        if (got == 0) {
            ended_.store(true, std::memory_order_relaxed);
            return produced;
        }
        ring_.push(scratch_.data(), got * ring_channels_);
        producer_pos_ += got;
        produced = true;
        if (looping && producer_pos_ >= loop_end) {
            // Stage A loop wrap: reposition ONLY the source read. The render cursors / AU state stay
            // put — the consumer is still playing earlier frames and wraps its own state later, when
            // playout reaches the boundary (pull_output_stage → loop_render_reset).
            if (auto rep = stream_->reposition_source(loop_start); !rep) {
                logs_.log(LogLevel::error, "monitor", rep.error().message);
                failed_.store(true, std::memory_order_relaxed);
                return produced;
            }
            producer_pos_ = loop_start;
        }
        if (got < frames) {
            return produced; // short block (end)
        }
    }
    return produced;
}

bool MonitorEngine::top_up_ring() {
    if (output_stage_) {
        return top_up_ring_output_stage();
    }
    bool produced = false;
    // Produce at most one ring's worth per call, then return so the worker loop re-checks for a
    // pending command (seek / override / switch). Without this bound, a consumer that drains as
    // fast as we produce keeps available_write above a chunk indefinitely, so this loop never
    // exits and the worker never returns to apply queued commands — starving mid-play edits
    // exactly when production is near the consume rate.
    // Lead target: while head tracking, only render a shallow amount ahead so a head turn is heard
    // promptly (the lead is the head-tracking latency). Otherwise fill the whole ring for maximum
    // underrun cushion — there `available_read < capacity` never gates before `available_write`, so
    // this is behaviourally identical to filling unconditionally.
    const bool tracking = pull_is_realtime_playback_ && head_tracking_recent();
    const std::size_t lead_target_floats =
        tracking ? static_cast<std::size_t>(k_tracking_lookahead_frames) * channels_ : ring_.capacity();
    std::size_t budget = (ring_.capacity() / scratch_.size()) + 1;
    while (ring_.available_write() >= scratch_.size() && ring_.available_read() < lead_target_floats && budget-- > 0) {
        // Finer render granularity while head tracking so the orientation applies sooner (see
        // k_tracking_chunk_frames); the larger chunk otherwise keeps per-chunk overhead low.
        uint64_t frames = tracking ? k_tracking_chunk_frames : k_chunk_frames;
        const uint64_t loop_end = loop_end_.load(std::memory_order_relaxed);
        const uint64_t loop_start = loop_start_.load(std::memory_order_relaxed);
        // Loop-region clamping is suspended during a crossfade (both streams must stay in
        // lockstep over the short fade window; a loop wrap mid-fade is not worth the desync).
        const bool looping = !xfade_active_ && loop_end > loop_start;
        if (looping && producer_pos_ < loop_end) {
            frames = std::min<uint64_t>(frames, loop_end - producer_pos_);
        }

        auto result = stream_->process(std::span<float>(scratch_.data(), frames * channels_), frames);
        if (!result) {
            failed_.store(true, std::memory_order_relaxed); // render error: stop producing
            logs_.log(LogLevel::error, "monitor", result.error().message);
            return produced;
        }
        std::size_t got = *result;

        if (xfade_active_) {
            // Render the incoming stream over the same frames and linearly blend old→new.
            auto in_res = xfade_stream_->process(std::span<float>(scratch_b_.data(), frames * channels_), frames);
            if (!in_res) {
                failed_.store(true, std::memory_order_relaxed);
                logs_.log(LogLevel::error, "monitor", in_res.error().message);
                return produced;
            }
            got = std::min(got, *in_res);
            for (std::size_t f = 0; f < got; ++f) {
                const auto p = static_cast<double>(xfade_pos_ + f);
                const float t = static_cast<float>(std::min(1.0, p / static_cast<double>(k_crossfade_frames)));
                for (uint32_t c = 0; c < channels_; ++c) {
                    const std::size_t i = (f * channels_) + c;
                    scratch_[i] = (scratch_[i] * (1.0F - t)) + (scratch_b_[i] * t);
                }
            }
            xfade_pos_ += got;
            // Finalize when the ramp completes or either stream ran short (end of material).
            if (xfade_pos_ >= k_crossfade_frames || got < frames) {
                finalize_crossfade();
            }
        }

        if (got == 0) {
            ended_.store(true, std::memory_order_relaxed); // end of material (non-looping)
            return produced;
        }
        feed_meter(scratch_.data(), got); // add the produced chunk to the LUFS meter + refresh snapshots (worker-only)
        ring_.push(scratch_.data(), got * channels_); // available_write checked: full push
        producer_pos_ += got;
        produced = true;

        if (looping && producer_pos_ >= loop_end) {
            // Producer-side wrap: ring stays valid, no flush needed. On failure, stop
            // producing rather than pretend the wrap happened.
            if (auto seek_res = stream_->seek(loop_start); !seek_res) {
                logs_.log(LogLevel::error, "monitor", seek_res.error().message);
                failed_.store(true, std::memory_order_relaxed);
                return produced;
            }
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
    // PullFn contract: return the number of frames actually produced from the ring (not the
    // requested count). A short read lets a non-realtime "feed" sink (the AVSampleBufferAudio-
    // Renderer monitor) avoid enqueuing the silence-padded tail and stalling the system buffer;
    // a realtime callback sink (miniaudio) ignores it since pull already padded. paused /
    // flushing produces nothing, so the feed sink lets its buffer drain until playback resumes.
    std::size_t produced_frames = 0;
    if (!active) {
        std::fill_n(out.data(), floats, 0.0F);
    } else if (output_stage_) {
        produced_frames = pull_output_stage(out, frames);
    } else {
        const std::size_t got = ring_.pop(out.data(), floats);
        if (got < floats) {
            std::fill_n(out.data() + got, floats - got, 0.0F);
            // A short read is an underrun only while the producer should still be feeding
            // us; once the stream has ended/failed, an empty ring is the expected silence.
            const bool producer_done =
                ended_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed);
            if (!producer_done && pull_is_realtime_playback_) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        frames_played_.fetch_add(got / channels_, std::memory_order_relaxed);
        produced_frames = got / channels_;
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
    return produced_frames;
}

std::size_t MonitorEngine::pull_output_stage(std::span<float> out, std::size_t frames) {
    // Audio callback: pop the buffered intermediate and render it to the output with the CURRENT
    // head pose (orientation_snapshot, lock-free) — so a head turn is heard at playout. Tap the
    // rendered output into meter_ring_ for the worker's LUFS meter. No alloc/lock (pull_scratch_ is
    // preallocated; render_output is realtime-safe). Loops only if a callback ever asks for more
    // than pull_scratch_ holds.
    const ListenerOrientation o = orientation_snapshot();
    const std::size_t max_chunk = pull_scratch_.size() / ring_channels_;
    const uint64_t loop_end = loop_end_.load(std::memory_order_relaxed);
    const uint64_t loop_start = loop_start_.load(std::memory_order_relaxed);
    const bool looping = loop_end > loop_start;
    std::size_t done = 0;
    while (done < frames) {
        std::size_t want = std::min(frames - done, max_chunk);
        // Clamp to the loop end so the render cursors wrap exactly at the boundary (Stage B loop).
        if (looping && consumer_src_pos_ < loop_end) {
            want = std::min<std::size_t>(want, static_cast<std::size_t>(loop_end - consumer_src_pos_));
        }
        const std::size_t got_floats = ring_.pop(pull_scratch_.data(), want * ring_channels_);
        const std::size_t got = got_floats / ring_channels_;
        if (got == 0) {
            break;
        }
        const std::size_t rendered =
            stream_->render_output(std::span<const float>(pull_scratch_.data(), got * ring_channels_),
                                   std::span<float>(out.data() + (done * channels_), got * channels_),
                                   got,
                                   o);
        meter_ring_.push(out.data() + (done * channels_), rendered * channels_); // best-effort tap
        done += got;
        consumer_src_pos_ += got;
        if (looping && consumer_src_pos_ >= loop_end) {
            // Playout reached the loop boundary: wrap the render cursors (realtime-safe; no AU reset).
            stream_->loop_render_reset(loop_start);
            consumer_src_pos_ = loop_start;
        }
        if (got < want) {
            break; // ring drained mid-block
        }
    }
    if (done < frames) {
        std::fill_n(out.data() + (done * channels_), (frames - done) * channels_, 0.0F);
        const bool producer_done = ended_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed);
        if (!producer_done && pull_is_realtime_playback_) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    frames_played_.fetch_add(done, std::memory_order_relaxed);
    return done;
}

void MonitorEngine::drain_meter_ring() {
    // Worker: drain the callback's rendered-output tap and feed the LUFS meter (which can't run on
    // the audio callback). Bounded per call. peak/RMS are computed inline on the callback already.
    if (meter_scratch_.empty()) {
        return;
    }
    std::size_t budget = (meter_ring_.capacity() / meter_scratch_.size()) + 1;
    while (meter_ring_.available_read() >= channels_ && budget-- > 0) {
        const std::size_t got = meter_ring_.pop(meter_scratch_.data(), meter_scratch_.size());
        if (got < channels_) {
            break;
        }
        feed_meter(meter_scratch_.data(), got / channels_);
    }
}

void MonitorEngine::apply_seek_locked(uint64_t frame) {
    // Settle any in-flight crossfade to the incoming stream, so the seek targets the stream
    // that will actually be playing afterwards.
    finalize_crossfade();
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
    meter_ring_.clear(); // output-stage tap (no-op buffer otherwise); consumer is parked, safe to reset
    if (auto seek_res = stream_->seek(frame); seek_res) {
        producer_pos_ = frame;
        consumer_src_pos_ = frame; // output-stage consumer position (callback parked during seek)
        frames_played_.store(frame, std::memory_order_relaxed);
        // Seeking re-arms production: a position before EOF has more to render.
        ended_.store(false, std::memory_order_relaxed);
        failed_.store(false, std::memory_order_relaxed);
    } else {
        // Don't fake a repositioned playhead: the stream is still wherever it was. Surface
        // the failure instead of silently claiming the seek landed.
        logs_.log(LogLevel::error, "monitor", seek_res.error().message);
        failed_.store(true, std::memory_order_relaxed);
    }
    // Drop the output device's stale pre-seek buffer (the buffered sink's staging + system queue)
    // so the new position doesn't splice onto old-position audio. No-op for the miniaudio sink.
    device_.flush();
    // The playhead jumped: restart loudness integration from the new position (momentary /
    // short-term windows would otherwise span the discontinuity).
    rebuild_meter();
    flushing_.store(false, std::memory_order_seq_cst);
}

MonitorStatus MonitorEngine::status() const {
    MonitorStatus s;
    s.state = state_.load(std::memory_order_seq_cst);
    s.playhead_frames = frames_played_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    const std::size_t cap = ring_.capacity();
    const std::size_t buffered_floats = ring_.available_read();
    s.buffered_frames = ring_channels_ > 0 ? buffered_floats / ring_channels_ : 0;
    s.ring_fill = cap > 0 ? static_cast<float>(buffered_floats) / static_cast<float>(cap) : 0.0F;
    s.ended = ended_.load(std::memory_order_relaxed);
    s.failed = failed_.load(std::memory_order_relaxed);
    s.override_revision = applied_override_revision_.load(std::memory_order_relaxed);
    return s;
}

MonitorLevels MonitorEngine::levels() const {
    MonitorLevels l;
    l.channels = std::min<uint32_t>(channels_, static_cast<uint32_t>(k_max_level_channels));
    for (std::size_t c = 0; c < l.channels; ++c) {
        l.peak.at(c) = peak_.at(c).load(std::memory_order_relaxed);
        l.rms.at(c) = rms_.at(c).load(std::memory_order_relaxed);
    }
    // Program loudness: read the worker's lock-free snapshots (the meter itself is worker-only),
    // so this 30 Hz UI poll never blocks the realtime worker on a shared meter lock.
    l.momentary_lufs = momentary_lufs_.load(std::memory_order_relaxed);
    l.shortterm_lufs = shortterm_lufs_.load(std::memory_order_relaxed);
    l.integrated_lufs = integrated_lufs_.load(std::memory_order_relaxed);
    return l;
}

} // namespace mradm::realtime
