#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "adm/audio_io.h"
#include "adm/errors.h"

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

std::filesystem::path temp_path(std::string_view stem) {
    static std::atomic<int> s_seq{0};
    const auto name = std::string(stem) + "_" + std::to_string(static_cast<int>(::getpid())) + "_" +
                      std::to_string(s_seq.fetch_add(1)) + ".wav";
    return std::filesystem::temp_directory_path() / name;
}

// Write a WAV where channel 0 of frame i holds the value i, so trimmed ranges
// are trivially identifiable by their first/last sample.
bool write_ramp_wav(const std::string& path, uint32_t channels, uint32_t sample_rate, uint64_t frames) {
    auto writer_res = mradm::audio::FloatWavWriter::open(path, channels, sample_rate);
    if (!writer_res) {
        return false;
    }
    auto& writer = *writer_res;
    std::vector<float> buf(static_cast<std::size_t>(channels) * frames, 0.0F);
    for (uint64_t f = 0; f < frames; ++f) {
        buf[static_cast<std::size_t>(f) * channels] = static_cast<float>(f);
    }
    return writer.write(buf.data(), frames) == frames;
}

bool verify_trim_middle_range() {
    const auto path = temp_path("trim_mid");
    const FileGuard guard(path);
    constexpr uint32_t channels = 4;
    constexpr uint32_t sample_rate = 48000;
    constexpr uint64_t frames = 100;
    if (!check(write_ramp_wav(path.string(), channels, sample_rate, frames), "write ramp wav")) {
        return false;
    }

    // Trim to [25, 75): 50 frames, first sample value 25, last value 74.
    const auto res = mradm::audio::trim_file_frames(path.string(), 25, 50);
    if (!check(static_cast<bool>(res), "trim_file_frames middle range succeeds")) {
        return false;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!check(static_cast<bool>(reader_res), "reopen trimmed wav")) {
        return false;
    }
    auto& reader = *reader_res;
    if (!check(reader.frame_count() == 50, "trimmed frame count is 50")) {
        return false;
    }
    std::vector<float> buf(static_cast<std::size_t>(channels) * 50, -1.0F);
    const uint64_t got = reader.read(buf.data(), 50);
    if (!check(got == 50, "read back all trimmed frames")) {
        return false;
    }
    bool ok = true;
    ok &= check(buf[0] == 25.0F, "first trimmed frame value is 25");
    ok &= check(buf[static_cast<std::size_t>(49) * channels] == 74.0F, "last trimmed frame value is 74");
    return ok;
}

bool verify_trim_clamps_count_to_end() {
    const auto path = temp_path("trim_clamp");
    const FileGuard guard(path);
    if (!check(write_ramp_wav(path.string(), 2, 48000, 100), "write ramp wav")) {
        return false;
    }
    // Request 1000 frames from offset 90: only 10 remain, count is clamped.
    const auto res = mradm::audio::trim_file_frames(path.string(), 90, 1000);
    if (!check(static_cast<bool>(res), "trim_file_frames clamps count")) {
        return false;
    }
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!check(static_cast<bool>(reader_res), "reopen clamped wav")) {
        return false;
    }
    return check(reader_res->frame_count() == 10, "clamped frame count is 10");
}

bool verify_trim_start_beyond_end_fails() {
    const auto path = temp_path("trim_oob");
    const FileGuard guard(path);
    if (!check(write_ramp_wav(path.string(), 1, 48000, 50), "write ramp wav")) {
        return false;
    }
    const auto res = mradm::audio::trim_file_frames(path.string(), 50, 10);
    if (!check(!res, "trim past end fails")) {
        return false;
    }
    return check(res.error().code == mradm::ErrorCode::invalid_argument, "trim past end returns invalid_argument");
}

bool verify_trim_whole_file_is_noop() {
    const auto path = temp_path("trim_whole");
    const FileGuard guard(path);
    if (!check(write_ramp_wav(path.string(), 2, 48000, 64), "write ramp wav")) {
        return false;
    }
    const auto res = mradm::audio::trim_file_frames(path.string(), 0, 64);
    if (!check(static_cast<bool>(res), "whole-file trim succeeds")) {
        return false;
    }
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!check(static_cast<bool>(reader_res), "reopen untouched wav")) {
        return false;
    }
    return check(reader_res->frame_count() == 64, "whole-file trim preserves frame count");
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_trim_middle_range();
    ok &= verify_trim_clamps_count_to_end();
    ok &= verify_trim_start_beyond_end_fails();
    ok &= verify_trim_whole_file_is_noop();
    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "trim_file_frames smoke tests passed (4/4)\n";
    return EXIT_SUCCESS;
}
