#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

#include <bw64/bw64.hpp>

#include "adm/logging.h"
#include "adm/peak.h"

namespace {

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() { std::filesystem::remove(path_); }

  private:
    std::filesystem::path path_;
};

bool check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return condition;
}

// Write a 1ch 48kHz WAV with a 440 Hz sine wave at the given amplitude.
// 440 Hz is well below Nyquist, so True Peak ≈ sample peak (negligible inter-sample overshoot).
void write_sine_wav(float amplitude, const std::filesystem::path& path) {
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = k_sr; // 1 second
    constexpr float k_freq = 440.0F;

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{});
    auto axml = std::make_shared<bw64::AxmlChunk>("");
    auto writer = bw64::writeFile(path.string(), 1U, k_sr, 24U, chna, axml);

    std::vector<float> samples(k_frames);
    for (uint32_t n = 0; n < k_frames; ++n) {
        samples[n] = amplitude * std::sin(2.0F * std::numbers::pi_v<float> * k_freq * static_cast<float>(n) / static_cast<float>(k_sr));
    }
    writer->write(samples.data(), static_cast<uint64_t>(k_frames));
}

double max_abs_sample(const std::filesystem::path& path) {
    auto reader = bw64::readFile(path.string());
    const auto n_ch = reader->channels();
    constexpr std::size_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(n_ch) * k_block);
    uint64_t left = reader->numberOfFrames();
    double max_val = 0.0;
    while (left > 0) {
        const uint64_t n = std::min(static_cast<uint64_t>(k_block), left);
        reader->read(buf.data(), n);
        for (std::size_t i = 0; i < static_cast<std::size_t>(n_ch) * static_cast<std::size_t>(n); ++i) {
            max_val = std::max(max_val, std::abs(static_cast<double>(buf[i])));
        }
        left -= n;
    }
    return max_val;
}

// Over-limit: 440 Hz sine at 0.95 amplitude → True Peak > -1.0 dBTP → limiter applies.
// After limiting the sample peak must be ≤ target and not have collapsed.
bool verify_limiter_active() {
    const auto path = std::filesystem::temp_directory_path() / "mr_peak_sine_high.wav";
    FileGuard guard{path};
    write_sine_wav(0.95F, path);

    mradm::NullLogSink logs;
    const auto res = mradm::apply_peak_limit(path.string(), -1.0F, logs);
    if (!res) {
        std::cerr << "FAIL [active]: " << res.error().message << "\n";
        return false;
    }

    const double max_sample = max_abs_sample(path);
    const double target_linear = std::pow(10.0, -1.0 / 20.0); // ≈ 0.8913

    bool ok = true;
    ok &= check(max_sample <= target_linear * 1.001, "[active] peak ≤ -1.0 dBTP after limiting");
    // For a sine wave True Peak ≈ sample peak, so gain ≈ target/0.95 ≈ 0.94 → output ≈ 0.89.
    ok &= check(max_sample > target_linear * 0.9, "[active] peak is near target (not collapsed)");
    return ok;
}

// No-op: 440 Hz sine at 0.10 amplitude → True Peak well below -1.0 dBTP → no change.
bool verify_limiter_noop() {
    const auto path = std::filesystem::temp_directory_path() / "mr_peak_sine_low.wav";
    FileGuard guard{path};
    write_sine_wav(0.10F, path);

    mradm::NullLogSink logs;
    const auto res = mradm::apply_peak_limit(path.string(), -1.0F, logs);
    if (!res) {
        std::cerr << "FAIL [noop]: " << res.error().message << "\n";
        return false;
    }

    const double max_sample = max_abs_sample(path);
    // No gain was applied; sample peak should remain near 0.10.
    return check(max_sample > 0.08 && max_sample < 0.12, "[noop] amplitude unchanged");
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_limiter_active();
    ok &= verify_limiter_noop();

    if (ok) {
        std::cout << "peak limiter fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
