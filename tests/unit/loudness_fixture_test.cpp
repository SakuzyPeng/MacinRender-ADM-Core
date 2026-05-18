#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <ebur128.h>

#include "adm/audio_io.h"
#include "adm/logging.h"
#include "adm/loudness.h"

namespace {

class NullLog final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel, std::string_view, std::string_view) override {}
};

bool fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return false;
}

// Writes a mono 440 Hz sine WAV (float32).
bool write_sine_wav(const std::string& path, float amplitude, double duration_s,
                    uint32_t sample_rate = 48000) {
    const auto num_frames = static_cast<uint64_t>(duration_s * sample_rate);
    auto writer_res = mradm::audio::FloatWavWriter::open(path, 1, sample_rate);
    if (!writer_res) {
        return fail("FloatWavWriter::open failed: " + writer_res.error().message);
    }
    auto& writer = *writer_res;

    constexpr std::size_t k_block = 4096;
    std::vector<float> buf(k_block);
    uint64_t written = 0;
    while (written < num_frames) {
        const uint64_t n = std::min(static_cast<uint64_t>(k_block), num_frames - written);
        for (uint64_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(written + i) / sample_rate;
            buf[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * t));
        }
        writer.write(buf.data(), n);
        written += n;
    }
    return true;
}

// Returns LUFS, or NaN on failure.
double measure_lufs(const std::string& path) {
    auto reader_res = mradm::audio::FloatWavReader::open(path);
    if (!reader_res) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    auto& reader = *reader_res;

    struct Free {
        void operator()(ebur128_state* s) const noexcept { ebur128_destroy(&s); }
    };
    std::unique_ptr<ebur128_state, Free> st{
        ebur128_init(reader.channels(), reader.sample_rate(), EBUR128_MODE_I)};
    if (!st) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    constexpr std::size_t k_block = 4096;
    std::vector<float> buf(k_block);
    uint64_t left = reader.frame_count();
    while (left > 0) {
        const uint64_t n = std::min(static_cast<uint64_t>(k_block), left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        ebur128_add_frames_float(st.get(), buf.data(), static_cast<std::size_t>(got));
        left -= got;
    }

    double loudness = 0.0;
    if (ebur128_loudness_global(st.get(), &loudness) != EBUR128_SUCCESS) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return loudness;
}

} // namespace

int main() {
    const std::string norm_path =
        (std::filesystem::temp_directory_path() / "loudness_norm_test.wav").string();
    const std::string silence_path =
        (std::filesystem::temp_directory_path() / "loudness_silence_test.wav").string();

    // ── Test 1: normalise loud signal to -23 LUFS within 1.5 LU ─────────────
    // 440 Hz sine at -6 dBFS for 5 seconds — safely above gating threshold.
    if (!write_sine_wav(norm_path, 0.5F, 5.0)) {
        return EXIT_FAILURE;
    }

    const double before = measure_lufs(norm_path);
    if (!std::isfinite(before)) {
        return fail("failed to measure integrated loudness before normalization");
    }

    NullLog logs;
    {
        auto res = mradm::apply_loudness_norm(norm_path, -23.0F, logs);
        if (!res) {
            std::filesystem::remove(norm_path);
            return fail("apply_loudness_norm returned error: " + res.error().message);
        }
    }

    const double after = measure_lufs(norm_path);
    std::filesystem::remove(norm_path);

    if (!std::isfinite(after)) {
        return fail("failed to measure integrated loudness after normalization");
    }
    const double error_lu = std::abs(after - (-23.0));
    if (error_lu >= 1.5) {
        return fail("loudness error too large: |" + std::to_string(after) + " - (-23.0)| = " +
                    std::to_string(error_lu) + " LU (limit 1.5)");
    }

    // ── Test 2: silence is a no-op (no error) ────────────────────────────────
    if (!write_sine_wav(silence_path, 0.0F, 3.0)) {
        return EXIT_FAILURE;
    }
    {
        auto res = mradm::apply_loudness_norm(silence_path, -23.0F, logs);
        std::filesystem::remove(silence_path);
        if (!res) {
            return fail("apply_loudness_norm on silence returned error: " + res.error().message);
        }
    }

    std::cout << "loudness fixture test passed (after=" << after << " LUFS, error=" << error_lu << " LU)\n";
    return EXIT_SUCCESS;
}
