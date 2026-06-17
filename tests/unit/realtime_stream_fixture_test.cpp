#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adm/logging.h"
#include "adm/options.h"
#include "adm/render.h"
#include "adm/scene.h"

#include "audio_output_device.h"
#include "monitor_engine.h"
#include "render_stream_factory.h"
#include "ring_buffer.h"

namespace {

bool check(bool cond, std::string_view msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return cond;
}

// Deterministic test stream: sample(frame, ch) = frame * 0.001 + ch. A positional pattern
// (no internal DSP) so any requested frame count maps to an exact, checkable value — it
// catches off-by-one across mismatched process() sizes, seek-without-reset, ring wrap.
class PatternStream final : public mradm::IRenderStream {
  public:
    // total_frames == 0 means infinite; otherwise process() returns 0 (EOF) past it.
    explicit PatternStream(uint32_t channels, uint64_t total_frames = 0) : channels_(channels), total_(total_frames) {}

    [[nodiscard]] static float sample(uint64_t frame, uint32_t ch) {
        return (static_cast<float>(frame) * 0.001F) + static_cast<float>(ch);
    }

    mradm::Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        std::size_t n = frames;
        if (total_ > 0) {
            if (playhead_ >= total_) {
                return std::size_t{0}; // EOF
            }
            n = std::min<std::size_t>(frames, static_cast<std::size_t>(total_ - playhead_));
        }
        for (std::size_t f = 0; f < n; ++f) {
            for (uint32_t c = 0; c < channels_; ++c) {
                out[(f * channels_) + c] = sample(playhead_ + f, c);
            }
        }
        playhead_ += n;
        return n;
    }

    mradm::Result<void> seek(uint64_t frame) override {
        playhead_ = frame;
        return {};
    }

    [[nodiscard]] uint32_t out_channels() const override { return channels_; }
    [[nodiscard]] uint32_t sample_rate() const override { return 48000U; }
    [[nodiscard]] std::string_view output_layout() const override { return "binaural"; }

  private:
    uint32_t channels_;
    uint64_t total_{0};
    uint64_t playhead_{0};
};

class PatternStreamFactory final : public mradm::realtime::IRenderStreamFactory {
  public:
    explicit PatternStreamFactory(uint64_t total_frames = 0) : total_(total_frames) {}

    mradm::Result<std::unique_ptr<mradm::IRenderStream>>
    open(const mradm::AdmScene& /*scene*/, const mradm::RenderOptions& /*opts*/, mradm::LogSink& /*logs*/) override {
        return std::make_unique<PatternStream>(2U, total_);
    }

  private:
    uint64_t total_{0};
};

// Synchronous test device: pulls `total_frames` in fixed `device_block` chunks (a block
// size unrelated to any stream-internal sizing) and captures the interleaved output, so a
// test can assert the device→pull→stream path is sample-exact.
class CaptureSink final : public mradm::realtime::IAudioOutputDevice {
  public:
    CaptureSink(std::size_t total_frames, std::size_t device_block)
        : total_frames_(total_frames), device_block_(device_block) {}

    mradm::Result<void> start(uint32_t channels, uint32_t sample_rate, PullFn pull) override {
        channels_ = channels;
        rate_ = sample_rate;
        std::vector<float> block(device_block_ * channels, 0.0F);
        std::size_t done = 0;
        while (done < total_frames_) {
            const std::size_t want = std::min(device_block_, total_frames_ - done);
            std::ranges::fill(block, 0.0F); // device buffer starts as silence
            pull(std::span<float>(block.data(), want * channels), want);
            captured_.insert(
                captured_.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(want * channels));
            done += want;
        }
        return {};
    }

    void stop() override {}
    [[nodiscard]] uint32_t actual_sample_rate() const override { return rate_; }
    [[nodiscard]] const std::vector<float>& captured() const { return captured_; }

  private:
    std::size_t total_frames_;
    std::size_t device_block_;
    uint32_t channels_{0};
    uint32_t rate_{0};
    std::vector<float> captured_;
};

bool near(float a, float b) {
    return std::fabs(a - b) < 1.0e-4F;
}

// ── ring buffer ───────────────────────────────────────────────────────────────

bool test_ring_basic() {
    bool ok = true;
    mradm::realtime::FloatRingBuffer ring(8);
    ok &= check(ring.capacity() == 8U, "ring capacity");
    ok &= check(ring.available_read() == 0U && ring.available_write() == 8U, "ring starts empty");

    const std::array<float, 5> in{1, 2, 3, 4, 5};
    ok &= check(ring.push(in.data(), in.size()) == 5U, "push 5");
    ok &= check(ring.available_read() == 5U, "5 readable after push");

    std::array<float, 5> out{};
    ok &= check(ring.pop(out.data(), 3U) == 3U, "pop 3");
    ok &= check(near(out[0], 1) && near(out[1], 2) && near(out[2], 3), "popped values in order");
    ok &= check(ring.available_read() == 2U, "2 left after pop");

    // Overfill: capacity 8, 2 already held -> at most 6 more accepted.
    const std::vector<float> big(100, 9.0F);
    ok &= check(ring.push(big.data(), big.size()) == 6U, "push clamps to free space");
    ok &= check(ring.available_write() == 0U, "full after clamp");
    ok &= check(ring.push(in.data(), 1U) == 0U, "push on full returns 0");

    // Pop more than available -> clamps.
    std::array<float, 32> drain{};
    ok &= check(ring.pop(drain.data(), drain.size()) == 8U, "pop clamps to available");
    ok &= check(ring.available_read() == 0U, "empty after drain");
    return ok;
}

// SPSC integrity across a wrapping ring smaller than the stream: every produced value is
// consumed exactly once, in order (0,1,2,...,N-1), under concurrent producer/consumer.
bool test_ring_spsc_threaded() {
    constexpr std::size_t k_total = 200000;
    mradm::realtime::FloatRingBuffer ring(1024);

    // stop lets a failing consumer release the producer: without it a mismatch would
    // break the consumer loop, the producer would fill the ring and spin on push()==0,
    // and the test would hang instead of failing.
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        std::size_t produced = 0;
        while (produced < k_total && !stop.load(std::memory_order_relaxed)) {
            const auto v = static_cast<float>(produced);
            if (ring.push(&v, 1U) == 1U) {
                ++produced;
            }
        }
    });

    bool ordered = true;
    std::size_t consumed = 0;
    while (consumed < k_total) {
        float v = 0.0F;
        if (ring.pop(&v, 1U) == 1U) {
            if (!near(v, static_cast<float>(consumed))) {
                ordered = false;
                break;
            }
            ++consumed;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();

    bool ok = true;
    ok &= check(ordered, "SPSC values consumed in order, none lost/duplicated");
    ok &= check(consumed == k_total, "SPSC consumed all values");
    return ok;
}

// ── stream ────────────────────────────────────────────────────────────────────

// process() across mismatched request sizes must yield one contiguous pattern stream.
bool test_pattern_stream_contiguous() {
    bool ok = true;
    PatternStream stream(2U);
    const std::array<std::size_t, 4> sizes{100, 1000, 4096, 7};
    uint64_t frame = 0;
    bool exact = true;
    for (const std::size_t n : sizes) {
        std::vector<float> out(n * 2U, 0.0F);
        const auto produced = stream.process(std::span<float>(out), n);
        if (!produced || *produced != n) {
            exact = false;
            break;
        }
        for (std::size_t f = 0; f < n && exact; ++f) {
            if (!near(out[(f * 2U) + 0U], PatternStream::sample(frame + f, 0U)) ||
                !near(out[(f * 2U) + 1U], PatternStream::sample(frame + f, 1U))) {
                exact = false;
            }
        }
        frame += n;
    }
    ok &= check(exact, "process() is sample-exact across mismatched request sizes");
    return ok;
}

bool test_pattern_stream_seek() {
    bool ok = true;
    PatternStream stream(2U);
    std::vector<float> warm(std::size_t{100} * 2U, 0.0F);
    stream.process(std::span<float>(warm), 100);

    ok &= check(stream.seek(5000U).has_value(), "seek returns ok");
    std::array<float, std::size_t{10} * 2U> out{};
    stream.process(std::span<float>(out), 10);
    ok &= check(near(out[0], PatternStream::sample(5000U, 0U)) && near(out[1], PatternStream::sample(5000U, 1U)),
                "first frame after seek is at the sought position (state reset)");
    ok &= check(near(out[(std::size_t{9} * 2U) + 0U], PatternStream::sample(5009U, 0U)),
                "frames advance from sought position");
    return ok;
}

// device → pull → stream path is sample-exact, with device_block != request granularity.
bool test_capture_sink_path() {
    bool ok = true;
    constexpr std::size_t k_total = 10000;
    auto stream = std::make_unique<PatternStream>(2U);
    CaptureSink sink(k_total, /*device_block=*/333);

    auto pull = [&stream](std::span<float> out, std::size_t frames) -> std::size_t {
        return stream->process(out, frames).value_or(0U);
    };
    ok &= check(sink.start(2U, 48000U, pull).has_value(), "sink starts");
    ok &= check(sink.actual_sample_rate() == 48000U, "sink reports rate");

    const auto& cap = sink.captured();
    ok &= check(cap.size() == k_total * 2U, "captured frame count");
    bool exact = true;
    for (std::size_t f = 0; f < k_total && exact; ++f) {
        if (!near(cap[(f * 2U) + 0U], PatternStream::sample(f, 0U)) ||
            !near(cap[(f * 2U) + 1U], PatternStream::sample(f, 1U))) {
            exact = false;
        }
    }
    ok &= check(exact, "captured PCM matches pattern across device blocks");
    return ok;
}

bool test_stream_factory() {
    bool ok = true;
    PatternStreamFactory factory;
    mradm::AdmScene scene;
    mradm::RenderOptions opts;
    mradm::NullLogSink logs;
    auto stream = factory.open(scene, opts, logs);
    ok &= check(stream.has_value() && *stream != nullptr, "factory opens a stream");
    if (stream.has_value() && *stream != nullptr) {
        ok &= check((*stream)->out_channels() == 2U, "factory stream channels");
        ok &= check((*stream)->output_layout() == "binaural", "factory stream layout");
    }
    return ok;
}

// ── monitor engine ──────────────────────────────────────────────────────────────

// Test device: stores the engine's pull and lets the test pump it synchronously, so
// MonitorEngine's async worker → ring → pull path can be driven deterministically.
class ManualSink final : public mradm::realtime::IAudioOutputDevice {
  public:
    mradm::Result<void> start(uint32_t channels, uint32_t sample_rate, PullFn pull) override {
        channels_ = channels;
        rate_ = sample_rate;
        pull_ = std::move(pull);
        return {};
    }
    void stop() override { pull_ = nullptr; }
    [[nodiscard]] uint32_t actual_sample_rate() const override { return rate_; }

    void pump(std::size_t frames) {
        std::vector<float> buf(frames * channels_, 0.0F);
        pull_(std::span<float>(buf), frames);
        captured_.insert(captured_.end(), buf.begin(), buf.end());
    }

    [[nodiscard]] uint32_t channels() const { return channels_; }
    [[nodiscard]] const std::vector<float>& captured() const { return captured_; }

  private:
    PullFn pull_;
    uint32_t channels_{0};
    uint32_t rate_{0};
    std::vector<float> captured_;
};

// Pull exactly `total` frames without out-running the producer: wait until the worker has
// rendered some frames ahead, then pump only what is buffered. Guarantees no underrun so
// the capture is a gap-free, checkable stream. Returns false if the producer stalls.
bool drain_exact(mradm::realtime::MonitorEngine& engine, ManualSink& sink, std::size_t total) {
    std::size_t done = 0;
    while (done < total) {
        std::size_t buffered = 0;
        for (int spin = 0; spin < 200000; ++spin) {
            buffered = engine.status().buffered_frames;
            if (buffered > 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        if (buffered == 0) {
            return false;
        }
        const std::size_t want = std::min(total - done, buffered);
        sink.pump(want);
        done += want;
    }
    return true;
}

bool test_monitor_playback() {
    bool ok = true;
    PatternStreamFactory factory;
    ManualSink sink;
    mradm::NullLogSink logs;
    mradm::AdmScene scene;
    mradm::RenderOptions opts;

    auto engine = mradm::realtime::MonitorEngine::create(factory, sink, scene, opts, logs);
    ok &= check(engine.has_value(), "monitor engine creates");
    if (!engine.has_value()) {
        return ok;
    }
    (*engine)->play();

    constexpr std::size_t k_frames = 5000;
    ok &= check(drain_exact(**engine, sink, k_frames), "drained frames without producer stall");

    const auto& cap = sink.captured();
    ok &= check(cap.size() == k_frames * 2U, "captured frame count");
    bool exact = true;
    for (std::size_t f = 0; f < k_frames && exact; ++f) {
        if (!near(cap[(f * 2U) + 0U], PatternStream::sample(f, 0U)) ||
            !near(cap[(f * 2U) + 1U], PatternStream::sample(f, 1U))) {
            exact = false;
        }
    }
    ok &= check(exact, "worker→ring→pull is sample-exact and contiguous");
    return ok;
}

bool test_monitor_loop() {
    bool ok = true;
    PatternStreamFactory factory;
    ManualSink sink;
    mradm::NullLogSink logs;
    mradm::AdmScene scene;
    mradm::RenderOptions opts;

    auto engine = mradm::realtime::MonitorEngine::create(factory, sink, scene, opts, logs);
    if (!check(engine.has_value(), "loop: engine creates")) {
        return false;
    }
    constexpr std::size_t k_loop = 1000;
    (*engine)->set_loop(0, k_loop);
    (*engine)->play();

    constexpr std::size_t k_frames = 2500;
    ok &= check(drain_exact(**engine, sink, k_frames), "loop: drained frames");
    const auto& cap = sink.captured();
    bool wrapped = true;
    for (std::size_t f = 0; f < k_frames && wrapped; ++f) {
        const uint64_t src = f % k_loop; // output wraps every loop length
        if (!near(cap[(f * 2U) + 0U], PatternStream::sample(src, 0U))) {
            wrapped = false;
        }
    }
    ok &= check(wrapped, "loop region wraps output to loop start");
    return ok;
}

bool test_monitor_pause() {
    bool ok = true;
    PatternStreamFactory factory;
    ManualSink sink;
    mradm::NullLogSink logs;
    mradm::AdmScene scene;
    mradm::RenderOptions opts;

    auto engine = mradm::realtime::MonitorEngine::create(factory, sink, scene, opts, logs);
    if (!check(engine.has_value(), "pause: engine creates")) {
        return false;
    }
    (*engine)->play();
    ok &= check(drain_exact(**engine, sink, 500), "pause: drained pre-pause frames");
    (*engine)->pause();

    // Paused pulls return silence (and do not drain the ring).
    sink.pump(500);
    const auto& cap = sink.captured();
    ok &= check(cap.size() == std::size_t{1000} * 2U, "pause: captured count");
    bool silent = true;
    for (std::size_t f = 500; f < 1000 && silent; ++f) {
        if (!near(cap[(f * 2U) + 0U], 0.0F) || !near(cap[(f * 2U) + 1U], 0.0F)) {
            silent = false;
        }
    }
    ok &= check(silent, "paused output is silence");
    return ok;
}

bool test_monitor_seek() {
    bool ok = true;
    PatternStreamFactory factory;
    ManualSink sink;
    mradm::NullLogSink logs;
    mradm::AdmScene scene;
    mradm::RenderOptions opts;

    auto engine = mradm::realtime::MonitorEngine::create(factory, sink, scene, opts, logs);
    if (!check(engine.has_value(), "seek: engine creates")) {
        return false;
    }
    (*engine)->play();
    ok &= check(drain_exact(**engine, sink, 300), "seek: drained pre-seek frames");

    constexpr uint64_t k_target = 5000;
    (*engine)->seek(k_target);
    // Wait until the worker has applied the seek (playhead jumps to the target).
    bool applied = false;
    for (int spin = 0; spin < 200000; ++spin) {
        if ((*engine)->status().playhead_frames == k_target) {
            applied = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    ok &= check(applied, "seek applied (playhead reset to target)");

    const std::size_t before = sink.captured().size();
    ok &= check(drain_exact(**engine, sink, 10), "seek: drained post-seek frames");
    const auto& cap = sink.captured();
    bool repositioned = true;
    for (std::size_t f = 0; f < 10 && repositioned; ++f) {
        const std::size_t idx = before + (f * 2U);
        if (!near(cap[idx + 0U], PatternStream::sample(k_target + f, 0U)) ||
            !near(cap[idx + 1U], PatternStream::sample(k_target + f, 1U))) {
            repositioned = false;
        }
    }
    ok &= check(repositioned, "post-seek output starts at the sought position (ring flushed)");
    return ok;
}

bool test_monitor_eof() {
    bool ok = true;
    PatternStreamFactory factory(2000); // finite: 2000 frames then EOF
    ManualSink sink;
    mradm::NullLogSink logs;
    mradm::AdmScene scene;
    mradm::RenderOptions opts;

    auto engine = mradm::realtime::MonitorEngine::create(factory, sink, scene, opts, logs);
    if (!check(engine.has_value(), "eof: engine creates")) {
        return false;
    }
    (*engine)->play();
    ok &= check(drain_exact(**engine, sink, 2000), "eof: drained all material");

    // The worker must reach a terminal state and stop polling process() (no spin).
    bool ended = false;
    for (int spin = 0; spin < 200000; ++spin) {
        if ((*engine)->status().ended) {
            ended = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    ok &= check(ended, "eof: status reports ended");
    ok &= check((*engine)->status().underruns == 0, "eof: clean end is not counted as underrun");

    // Pulling past the end yields silence, still no underrun.
    sink.pump(100);
    ok &= check((*engine)->status().underruns == 0, "eof: post-end pulls are silence, not underruns");
    const auto& cap = sink.captured();
    bool tail_silent = true;
    for (std::size_t f = 2000; f < 2100 && tail_silent; ++f) {
        if (!near(cap[(f * 2U) + 0U], 0.0F) || !near(cap[(f * 2U) + 1U], 0.0F)) {
            tail_silent = false;
        }
    }
    ok &= check(tail_silent, "eof: output past end is silence");
    return ok;
}

// Malformed backend: reports 0 channels. create() must reject it (else producer would spin
// on a zero-size scratch and pull() would divide by zero).
class ZeroChannelStream final : public mradm::IRenderStream {
  public:
    mradm::Result<std::size_t> process(std::span<float> /*out*/, std::size_t frames) override { return frames; }
    mradm::Result<void> seek(uint64_t /*frame*/) override { return {}; }
    [[nodiscard]] uint32_t out_channels() const override { return 0U; }
    [[nodiscard]] uint32_t sample_rate() const override { return 48000U; }
    [[nodiscard]] std::string_view output_layout() const override { return "binaural"; }
};

class ZeroChannelStreamFactory final : public mradm::realtime::IRenderStreamFactory {
  public:
    mradm::Result<std::unique_ptr<mradm::IRenderStream>>
    open(const mradm::AdmScene& /*scene*/, const mradm::RenderOptions& /*opts*/, mradm::LogSink& /*logs*/) override {
        return std::make_unique<ZeroChannelStream>();
    }
};

bool test_monitor_malformed_stream() {
    bool ok = true;
    ZeroChannelStreamFactory factory;
    ManualSink sink;
    mradm::NullLogSink logs;
    mradm::AdmScene scene;
    mradm::RenderOptions opts;

    auto engine = mradm::realtime::MonitorEngine::create(factory, sink, scene, opts, logs);
    ok &= check(!engine.has_value(), "create rejects a 0-channel stream");
    if (!engine.has_value()) {
        ok &= check(engine.error().code == mradm::ErrorCode::internal_error, "malformed stream → internal_error");
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_ring_basic();
    ok &= test_ring_spsc_threaded();
    ok &= test_pattern_stream_contiguous();
    ok &= test_pattern_stream_seek();
    ok &= test_capture_sink_path();
    ok &= test_stream_factory();
    ok &= test_monitor_playback();
    ok &= test_monitor_loop();
    ok &= test_monitor_pause();
    ok &= test_monitor_seek();
    ok &= test_monitor_eof();
    ok &= test_monitor_malformed_stream();

    if (ok) {
        std::cout << "realtime stream/ring tests passed\n";
        return 0;
    }
    return 1;
}
