#include <algorithm>
#include <array>
#include <atomic>
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
    explicit PatternStream(uint32_t channels) : channels_(channels) {}

    [[nodiscard]] static float sample(uint64_t frame, uint32_t ch) {
        return (static_cast<float>(frame) * 0.001F) + static_cast<float>(ch);
    }

    mradm::Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        for (std::size_t f = 0; f < frames; ++f) {
            for (uint32_t c = 0; c < channels_; ++c) {
                out[(f * channels_) + c] = sample(playhead_ + f, c);
            }
        }
        playhead_ += frames;
        return frames;
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
    uint64_t playhead_{0};
};

class PatternStreamFactory final : public mradm::realtime::IRenderStreamFactory {
  public:
    mradm::Result<std::unique_ptr<mradm::IRenderStream>>
    open(const mradm::AdmScene& /*scene*/, const mradm::RenderOptions& /*opts*/, mradm::LogSink& /*logs*/) override {
        return std::make_unique<PatternStream>(2U);
    }
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

} // namespace

int main() {
    bool ok = true;
    ok &= test_ring_basic();
    ok &= test_ring_spsc_threaded();
    ok &= test_pattern_stream_contiguous();
    ok &= test_pattern_stream_seek();
    ok &= test_capture_sink_path();
    ok &= test_stream_factory();

    if (ok) {
        std::cout << "realtime stream/ring tests passed\n";
        return 0;
    }
    return 1;
}
