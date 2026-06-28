#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "adm/audio_io.h"

#include "test_portable.h"

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
    std::array<uint8_t, 4> hdr{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    f.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
    return hdr[0] == 0x1AU && hdr[1] == 0x45U && hdr[2] == 0xDFU && hdr[3] == 0xA3U;
}

std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::ptrdiff_t find_bytes(const std::vector<uint8_t>& data, const std::vector<uint8_t>& needle) {
    // NOLINTNEXTLINE(modernize-use-ranges)
    const auto it = std::search(data.begin(), data.end(), needle.begin(), needle.end());
    if (it == data.end()) {
        return -1;
    }
    return std::distance(data.begin(), it);
}

bool contains_ascii(const std::vector<uint8_t>& data, const std::string& text) {
    return find_bytes(data, {text.begin(), text.end()}) >= 0;
}

std::optional<std::size_t> find_opus_head(const std::vector<uint8_t>& data) {
    const auto pos = find_bytes(data, {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'});
    if (pos < 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(pos);
}

std::optional<uint8_t> opus_head_family(const std::vector<uint8_t>& data) {
    const auto pos = find_opus_head(data);
    if (!pos || *pos + 18U >= data.size()) {
        return std::nullopt;
    }
    return data[*pos + 18U];
}

bool verify_mka_stereo() {
    constexpr uint32_t k_ch = 2U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 4800U; // 100ms

    const std::string path = mr_test::temp_prefix() + "mr_opus_stereo_test.mka";
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
        if (!check(wr->write(samples.data(), k_frames) == k_frames, "stereo MKA short write")) {
            return false;
        }
        if (!check(wr->close().has_value(), "stereo MKA close failed")) {
            return false;
        }
    }

    if (!check(std::filesystem::exists(path), "MKA file not created")) {
        return false;
    }
    if (!check(std::filesystem::file_size(path) > 1000U, "MKA file suspiciously small")) {
        return false;
    }
    if (!check(has_ebml_magic(path), "MKA file missing EBML magic")) {
        return false;
    }
    return true;
}

bool verify_mka_multichannel_24() {
    constexpr uint32_t k_ch = 24U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 4800U;

    const std::string path = mr_test::temp_prefix() + "mr_opus_24ch_test.mka";
    FileGuard guard(path);

    std::vector<float> samples(static_cast<std::size_t>(k_ch) * k_frames, 0.3F);

    {
        auto wr = mradm::audio::FloatOpusMkaWriter::open(path, k_ch, k_sr);
        if (!check(wr.has_value(), "FloatOpusMkaWriter::open (24ch) failed")) {
            return false;
        }
        if (!check(wr->write(samples.data(), k_frames) == k_frames, "24ch MKA short write")) {
            return false;
        }
        if (!check(wr->close().has_value(), "24ch MKA close failed")) {
            return false;
        }
    }

    if (!check(std::filesystem::file_size(path) > 1000U, "24ch MKA suspiciously small")) {
        return false;
    }
    if (!check(has_ebml_magic(path), "24ch MKA missing EBML magic")) {
        return false;
    }
    return true;
}

bool verify_mka_wav71_uses_vorbis_mapping() {
    constexpr uint32_t k_ch = 8U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 4800U;

    const std::string path = mr_test::temp_prefix() + "mr_opus_wav71_mapping_test.mka";
    FileGuard guard(path);

    std::vector<float> samples(static_cast<std::size_t>(k_ch) * k_frames, 0.1F);

    {
        auto wr = mradm::audio::FloatOpusMkaWriter::open(path, k_ch, k_sr, 0U, "wav71");
        if (!check(wr.has_value(), "FloatOpusMkaWriter::open (wav71) failed")) {
            return false;
        }
        if (!check(wr->write(samples.data(), k_frames) == k_frames, "wav71 MKA short write")) {
            return false;
        }
        if (!check(wr->close().has_value(), "wav71 MKA close failed")) {
            return false;
        }
    }

    const auto data = read_binary(path);
    const auto pos = find_opus_head(data);
    if (!pos.has_value()) {
        check(false, "wav71 OpusHead missing");
        return false;
    }
    const auto head = pos.value();
    if (!check(opus_head_family(data) == 1U, "wav71 must use Opus mapping family 1")) {
        return false;
    }
    constexpr std::array<uint8_t, 8> k_expected_mapping{{0U, 6U, 1U, 2U, 3U, 4U, 5U, 7U}};
    if (!check(data[head + 19U] == 5U, "wav71 OpusHead stream count")) {
        return false;
    }
    if (!check(data[head + 20U] == 3U, "wav71 OpusHead coupled stream count")) {
        return false;
    }
    const auto mapping_begin = data.begin() + static_cast<std::ptrdiff_t>(head + 21U);
    return check(std::equal(k_expected_mapping.begin(), k_expected_mapping.end(), mapping_begin),
                 "wav71 OpusHead mapping table");
}

bool verify_mka_hoa3_uses_ambisonics_mapping() {
    constexpr uint32_t k_ch = 16U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 4800U;

    const std::string path = mr_test::temp_prefix() + "mr_opus_hoa3_mapping_test.mka";
    FileGuard guard(path);

    std::vector<float> samples(static_cast<std::size_t>(k_ch) * k_frames, 0.1F);

    {
        auto wr = mradm::audio::FloatOpusMkaWriter::open(path, k_ch, k_sr, 0U, "hoa3");
        if (!check(wr.has_value(), "FloatOpusMkaWriter::open (hoa3) failed")) {
            return false;
        }
        if (!check(wr->write(samples.data(), k_frames) == k_frames, "hoa3 MKA short write")) {
            return false;
        }
        if (!check(wr->close().has_value(), "hoa3 MKA close failed")) {
            return false;
        }
    }

    const auto data = read_binary(path);
    const auto pos = find_opus_head(data);
    if (!pos.has_value()) {
        check(false, "hoa3 OpusHead missing");
        return false;
    }
    const auto head = pos.value();
    if (!check(opus_head_family(data) == 2U, "hoa3 must use Opus ambisonics mapping family 2")) {
        return false;
    }
    if (!check(data[head + 19U] == 16U, "hoa3 OpusHead stream count")) {
        return false;
    }
    if (!check(data[head + 20U] == 0U, "hoa3 OpusHead coupled stream count")) {
        return false;
    }
    for (uint8_t i = 0; i < k_ch; ++i) {
        if (!check(data[head + 21U + i] == i, "hoa3 OpusHead ACN mapping table")) {
            return false;
        }
    }
    return true;
}

bool verify_mka_metadata_appended() {
    constexpr uint32_t k_ch = 2U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 1920U;

    const std::string path = mr_test::temp_prefix() + "mr_opus_meta_test.mka";
    FileGuard guard(path);

    {
        std::vector<float> silence(static_cast<std::size_t>(k_ch) * k_frames, 0.0F);
        auto wr = mradm::audio::FloatOpusMkaWriter::open(path, k_ch, k_sr);
        if (!check(wr.has_value(), "FloatOpusMkaWriter::open (meta) failed")) {
            return false;
        }
        if (!check(wr->write(silence.data(), k_frames) == k_frames, "metadata MKA short write")) {
            return false;
        }
        if (!check(wr->close().has_value(), "metadata MKA close failed")) {
            return false;
        }
    }

    mradm::audio::MetadataFields meta;
    meta.encoder = "TestEncoder";
    meta.date_utc = "2026-01-01T00:00:00Z";
    meta.renderer = "test";
    meta.output_layout = "0+2+0";
    meta.lufs = -23.0;
    meta.peak_dbtp = -1.0;

    auto res = mradm::audio::write_file_metadata(path, meta);
    if (!check(res.has_value(), "write_file_metadata on MKA failed")) {
        return false;
    }

    const auto data = read_binary(path);
    const auto tags_pos = find_bytes(data, {0x12U, 0x54U, 0xC3U, 0x67U});
    const auto cluster_pos =
        find_bytes(data, {0x1FU, 0x43U, 0xB6U, 0x75U, 0x01U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU});
    if (!check(tags_pos >= 0, "MKA Tags element missing")) {
        return false;
    }
    if (!check(cluster_pos >= 0, "MKA Cluster element missing")) {
        return false;
    }
    if (!check(tags_pos < cluster_pos, "MKA Tags must be written before first Cluster")) {
        return false;
    }
    if (!check(find_bytes(data, {0x45U, 0xA3U}) > tags_pos, "MKA TagName element missing")) {
        return false;
    }
    if (!check(find_bytes(data, {0x44U, 0x87U}) > tags_pos, "MKA TagString element missing")) {
        return false;
    }
    if (!check(contains_ascii(data, "TestEncoder"), "MKA ENCODER tag payload missing")) {
        return false;
    }
    if (!check(contains_ascii(data, "OUTPUT_LAYOUT"), "MKA OUTPUT_LAYOUT tag name missing")) {
        return false;
    }
    if (!check(contains_ascii(data, "0+2+0"), "MKA OUTPUT_LAYOUT tag value missing")) {
        return false;
    }
    return true;
}

bool verify_mka_wrong_sample_rate_rejected() {
    auto res = mradm::audio::FloatOpusMkaWriter::open(mr_test::temp_prefix() + "mr_opus_reject.mka", 2U, 44100U);
    return check(!res.has_value(), "44100 Hz should be rejected by Opus MKA writer");
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_mka_stereo();
    ok &= verify_mka_multichannel_24();
    ok &= verify_mka_wav71_uses_vorbis_mapping();
    ok &= verify_mka_hoa3_uses_ambisonics_mapping();
    ok &= verify_mka_metadata_appended();
    ok &= verify_mka_wrong_sample_rate_rejected();
    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "Opus MKA smoke tests passed (6/6)\n";
    return EXIT_SUCCESS;
}
