#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "adm/audio_io.h"

namespace {

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path p) : path_(std::move(p)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() { std::filesystem::remove(path_); }

  private:
    std::filesystem::path path_;
};

bool check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return cond;
}

// Check that file begins with EBML magic (0x1A 0x45 0xDF 0xA3)
bool has_ebml_magic(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    uint8_t hdr[4]{};
    f.read(reinterpret_cast<char*>(hdr), 4);
    return hdr[0] == 0x1AU && hdr[1] == 0x45U && hdr[2] == 0xDFU && hdr[3] == 0xA3U;
}

bool verify_mka_stereo() {
    constexpr uint32_t k_ch = 2U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 4800U;  // 100ms

    const std::string path = "/tmp/mr_opus_stereo_test.mka";
    FileGuard guard(path);

    std::vector<float> samples(static_cast<std::size_t>(k_ch) * k_frames);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = 0.5F * std::sin(static_cast<float>(i) * 0.05F);
    }

    {
        auto wr = mradm::audio::FloatOpusMkaWriter::open(path, k_ch, k_sr);
        if (!check(wr.has_value(), "FloatOpusMkaWriter::open (stereo) failed")) {
            return false;
        }
        wr->write(samples.data(), k_frames);
    }

    if (!check(std::filesystem::exists(path), "MKA file not created")) { return false; }
    if (!check(std::filesystem::file_size(path) > 1000U, "MKA file suspiciously small")) { return false; }
    if (!check(has_ebml_magic(path), "MKA file missing EBML magic")) { return false; }
    return true;
}

bool verify_mka_multichannel_24() {
    constexpr uint32_t k_ch = 24U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 4800U;

    const std::string path = "/tmp/mr_opus_24ch_test.mka";
    FileGuard guard(path);

    std::vector<float> samples(static_cast<std::size_t>(k_ch) * k_frames, 0.3F);

    {
        auto wr = mradm::audio::FloatOpusMkaWriter::open(path, k_ch, k_sr);
        if (!check(wr.has_value(), "FloatOpusMkaWriter::open (24ch) failed")) {
            return false;
        }
        wr->write(samples.data(), k_frames);
    }

    if (!check(std::filesystem::file_size(path) > 1000U, "24ch MKA suspiciously small")) { return false; }
    if (!check(has_ebml_magic(path), "24ch MKA missing EBML magic")) { return false; }
    return true;
}

bool verify_mka_metadata_appended() {
    constexpr uint32_t k_ch = 2U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 1920U;

    const std::string path = "/tmp/mr_opus_meta_test.mka";
    FileGuard guard(path);

    {
        std::vector<float> silence(static_cast<std::size_t>(k_ch) * k_frames, 0.0F);
        auto wr = mradm::audio::FloatOpusMkaWriter::open(path, k_ch, k_sr);
        if (!check(wr.has_value(), "FloatOpusMkaWriter::open (meta) failed")) {
            return false;
        }
        wr->write(silence.data(), k_frames);
    }

    const auto size_before = std::filesystem::file_size(path);

    mradm::audio::MetadataFields meta;
    meta.encoder = "TestEncoder";
    meta.date_utc = "2026-01-01T00:00:00Z";
    meta.renderer = "test";
    meta.output_layout = "0+2+0";
    meta.lufs = -23.0;
    meta.peak_dbtp = -1.0;

    auto res = mradm::audio::write_file_metadata(path, meta);
    if (!check(res.has_value(), "write_file_metadata on MKA failed")) { return false; }
    if (!check(std::filesystem::file_size(path) > size_before, "metadata did not grow MKA file")) { return false; }
    return true;
}

bool verify_mka_wrong_sample_rate_rejected() {
    auto res = mradm::audio::FloatOpusMkaWriter::open("/tmp/mr_opus_reject.mka", 2U, 44100U);
    return check(!res.has_value(), "44100 Hz should be rejected by Opus MKA writer");
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_mka_stereo();
    ok &= verify_mka_multichannel_24();
    ok &= verify_mka_metadata_appended();
    ok &= verify_mka_wrong_sample_rate_rejected();
    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "Opus MKA smoke tests passed (4/4)\n";
    return EXIT_SUCCESS;
}
