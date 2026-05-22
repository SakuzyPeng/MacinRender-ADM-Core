#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/version.h"

namespace {

[[nodiscard]] uint32_t read_be32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24U) | (static_cast<uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 2U]) << 8U) | static_cast<uint32_t>(bytes[offset + 3U]);
}

[[nodiscard]] uint16_t read_le16(const std::vector<unsigned char>& bytes, std::size_t offset) {
    const auto value = static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1U]) << 8U);
    return static_cast<uint16_t>(value);
}

[[nodiscard]] uint32_t read_le32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<uint32_t>(bytes[offset + 2U]) << 16U) | (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] uint64_t read_be64(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint64_t>(read_be32(bytes, offset)) << 32U) | read_be32(bytes, offset + 4U);
}

[[nodiscard]] std::size_t find_riff_chunk(const std::vector<unsigned char>& bytes, std::string_view id) {
    std::size_t offset = 12U;
    while (offset + 8U <= bytes.size()) {
        const auto chunk_id = std::string_view{reinterpret_cast<const char*>(bytes.data() + offset), 4U};
        const uint32_t size = read_le32(bytes, offset + 4U);
        if (chunk_id == id) {
            return offset;
        }
        offset += 8U + size + (size & 1U);
    }
    return std::string::npos;
}

[[nodiscard]] std::size_t find_caf_chunk(const std::vector<unsigned char>& bytes, std::string_view id) {
    std::size_t offset = 8U;
    while (offset + 12U <= bytes.size()) {
        const auto chunk_id = std::string_view{reinterpret_cast<const char*>(bytes.data() + offset), 4U};
        const uint64_t size = read_be64(bytes, offset + 4U);
        if (chunk_id == id) {
            return offset;
        }
        if (size > bytes.size() || offset + 12U + static_cast<std::size_t>(size) > bytes.size()) {
            break;
        }
        offset += 12U + static_cast<std::size_t>(size);
    }
    return std::string::npos;
}

[[nodiscard]] bool contains_blob(const std::vector<unsigned char>& bytes, std::string_view needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

[[nodiscard]] std::string caf_info_pair(std::string_view key, std::string_view value) {
    std::string out;
    out.append(key);
    out.push_back('\0');
    out.append(value);
    out.push_back('\0');
    return out;
}

[[nodiscard]] mradm::audio::MetadataFields test_metadata() {
    mradm::audio::MetadataFields meta;
    meta.encoder = "MacinRender Test";
    meta.date_utc = "2026-05-22T14:30:15Z";
    meta.renderer = "vbap-saf";
    meta.output_layout = "0+2+0";
    meta.lufs = -23.0;
    meta.peak_dbtp = -1.0;
    return meta;
}

[[nodiscard]] bool verify_wav_metadata_write() {
    const auto path = std::filesystem::temp_directory_path() / "mr_core_metadata_test.WAV";
    std::filesystem::remove(path);

    {
        auto writer_res = mradm::audio::WriterHandle::open(path.string(), 2U, 48000U, "0+2+0");
        if (!writer_res) {
            std::cerr << "WAV writer open failed: " << writer_res.error().message << "\n";
            return false;
        }
        std::vector<float> samples{0.125F, -0.125F};
        if (writer_res->write(samples.data(), 1U) != 1U) {
            std::cerr << "WAV writer short write\n";
            std::filesystem::remove(path);
            return false;
        }
    }

    auto meta = test_metadata();
    meta.renderer = "ear";
    meta.lufs = -23.5;
    const auto meta_res = mradm::audio::write_file_metadata(path.string(), meta);
    if (!meta_res) {
        std::cerr << "WAV metadata write failed: " << meta_res.error().message << "\n";
        std::filesystem::remove(path);
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    const auto bext = find_riff_chunk(bytes, "bext");
    if (bext == std::string::npos) {
        std::cerr << "WAV bext chunk missing\n";
        std::filesystem::remove(path);
        return false;
    }
    if (read_le32(bytes, 4U) != bytes.size() - 8U || read_le32(bytes, bext + 4U) != 602U) {
        std::cerr << "WAV bext or RIFF size mismatch\n";
        std::filesystem::remove(path);
        return false;
    }
    const auto payload = bext + 8U;
    const auto loudness = static_cast<int16_t>(read_le16(bytes, payload + 412U));
    const auto true_peak = static_cast<int16_t>(read_le16(bytes, payload + 416U));
    if (!contains_blob(bytes, "renderer=ear layout=0+2+0") || !contains_blob(bytes, "MacinRender Test") ||
        !contains_blob(bytes, "2026-05-22") || !contains_blob(bytes, "14-30-15") ||
        read_le16(bytes, payload + 346U) != 2U || loudness != -2350 || true_peak != -100) {
        std::cerr << "WAV bext metadata fields mismatch\n";
        std::filesystem::remove(path);
        return false;
    }

    std::filesystem::remove(path);
    return true;
}

[[nodiscard]] bool verify_caf_metadata_write(const std::filesystem::path& path) {
    const auto meta_res = mradm::audio::write_file_metadata(path.string(), test_metadata());
    if (!meta_res) {
        std::cerr << "CAF metadata write failed: " << meta_res.error().message << "\n";
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    const auto info = find_caf_chunk(bytes, "info");
    if (info == std::string::npos) {
        std::cerr << "CAF info chunk missing\n";
        return false;
    }
    const uint64_t size = read_be64(bytes, info + 4U);
    if (size < 4U || read_be32(bytes, info + 12U) != 3U) {
        std::cerr << "CAF info chunk entry count mismatch\n";
        return false;
    }

    const auto encoder = caf_info_pair("encodingapplication", "MacinRender Test");
    const auto date = caf_info_pair("date", "2026-05-22T14:30:15Z");
    const auto comments = caf_info_pair("comments", "renderer=vbap-saf layout=0+2+0 loudness=-23.0LUFS peak=-1.00dBTP");
    if (!contains_blob(bytes, encoder) || !contains_blob(bytes, date) || !contains_blob(bytes, comments)) {
        std::cerr << "CAF info metadata fields mismatch\n";
        return false;
    }

    auto reader_res = mradm::audio::ReaderHandle::open(path.string());
    if (!reader_res || reader_res->channels() != 2U || reader_res->frame_count() != 4U) {
        std::cerr << "CAF reader failed after info chunk append\n";
        return false;
    }
    return true;
}

[[nodiscard]] bool verify_caf_writer() {
    const auto path = std::filesystem::temp_directory_path() / "mr_core_caf_writer_test.caf";
    std::filesystem::remove(path);

    {
        auto writer_res = mradm::audio::WriterHandle::open(path.string(), 2U, 48000U, "0+2+0");
        if (!writer_res) {
            std::cerr << "CAF writer open failed: " << writer_res.error().message << "\n";
            return false;
        }
        auto& writer = *writer_res;
        const std::vector<float> samples{
            0.0F,
            0.0F,
            0.25F,
            -0.25F,
            0.5F,
            -0.5F,
            0.75F,
            -0.75F,
        };
        if (writer.write(samples.data(), 4U) != 4U) {
            std::cerr << "CAF writer short write\n";
            return false;
        }
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});

    if (bytes.size() < 92U) {
        std::cerr << "CAF file too small\n";
        std::filesystem::remove(path);
        return false;
    }
    if (std::string_view{reinterpret_cast<const char*>(bytes.data()), 4U} != "caff") {
        std::cerr << "CAF magic mismatch\n";
        std::filesystem::remove(path);
        return false;
    }
    constexpr uint32_t k_stereo_tag = (101U << 16U) | 2U;
    if (read_be32(bytes, 64U) != k_stereo_tag) {
        std::cerr << "CAF stereo channel layout tag mismatch\n";
        std::filesystem::remove(path);
        return false;
    }
    if (read_be64(bytes, 80U) != 36U) {
        std::cerr << "CAF data chunk size mismatch\n";
        std::filesystem::remove(path);
        return false;
    }

    {
        auto reader_res = mradm::audio::ReaderHandle::open(path.string());
        if (!reader_res) {
            std::cerr << "CAF reader open failed: " << reader_res.error().message << "\n";
            std::filesystem::remove(path);
            return false;
        }
        auto& reader = *reader_res;
        if (reader.channels() != 2U || reader.sample_rate() != 48000U || reader.frame_count() != 4U) {
            std::cerr << "CAF reader metadata mismatch\n";
            std::filesystem::remove(path);
            return false;
        }
        std::vector<float> readback(8U);
        if (reader.read(readback.data(), 4U) != 4U || readback[2] != 0.25F || readback[7] != -0.75F) {
            std::cerr << "CAF reader sample mismatch\n";
            std::filesystem::remove(path);
            return false;
        }
    }

    auto gain_res = mradm::audio::apply_gain_to_file(path.string(), 2.0F, "0+2+0");
    if (!gain_res) {
        std::cerr << "CAF apply_gain failed: " << gain_res.error().message << "\n";
        std::filesystem::remove(path);
        return false;
    }
    const auto stale_tmp = path.parent_path() / (path.stem().string() + ".gain_tmp" + path.extension().string());
    {
        std::ofstream stale(stale_tmp, std::ios::binary);
        stale << "stale";
    }
    gain_res = mradm::audio::apply_gain_to_file(path.string(), 0.5F, "0+2+0");
    if (!gain_res) {
        std::cerr << "CAF apply_gain with stale tmp failed: " << gain_res.error().message << "\n";
        std::filesystem::remove(stale_tmp);
        std::filesystem::remove(path);
        return false;
    }
    if (!std::filesystem::exists(stale_tmp)) {
        std::cerr << "CAF apply_gain reused legacy fixed tmp name\n";
        std::filesystem::remove(path);
        return false;
    }
    std::filesystem::remove(stale_tmp);
    {
        std::ifstream gained(path, std::ios::binary);
        std::vector<unsigned char> gained_bytes((std::istreambuf_iterator<char>(gained)), {});
        if (gained_bytes.size() < 4U ||
            std::string_view{reinterpret_cast<const char*>(gained_bytes.data()), 4U} != "caff") {
            std::cerr << "CAF apply_gain rewrote file as non-CAF\n";
            std::filesystem::remove(path);
            return false;
        }
    }
    {
        auto reader_res = mradm::audio::ReaderHandle::open(path.string());
        if (!reader_res) {
            std::cerr << "CAF reader reopen after gain failed: " << reader_res.error().message << "\n";
            std::filesystem::remove(path);
            return false;
        }
        std::vector<float> readback(8U);
        auto& reader = *reader_res;
        if (reader.read(readback.data(), 4U) != 4U || readback[2] != 0.25F || readback[7] != -0.75F) {
            std::cerr << "CAF gain sample mismatch\n";
            std::filesystem::remove(path);
            return false;
        }
    }

    auto bad_res = mradm::audio::FloatCafWriter::open(path.string(), 6U, 48000U, "0+2+0");
    if (bad_res || bad_res.error().code != mradm::ErrorCode::invalid_argument) {
        std::cerr << "CAF layout/channel mismatch should return invalid_argument\n";
        std::filesystem::remove(path);
        return false;
    }
    if (!verify_caf_metadata_write(path)) {
        std::filesystem::remove(path);
        return false;
    }
    std::filesystem::remove(path);
    return true;
}

} // namespace

int main() {
    if (mradm::version().empty()) {
        std::cerr << "version should not be empty\n";
        return EXIT_FAILURE;
    }

    mradm::RenderService service;
    mradm::RenderRequest request;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;

    mradm::RenderResult result = service.render(request, progress, logs);
    if (result.error.code != mradm::ErrorCode::invalid_argument) {
        std::cerr << "empty input should return invalid_argument\n";
        return EXIT_FAILURE;
    }

    if (!verify_caf_writer()) {
        return EXIT_FAILURE;
    }
    if (!verify_wav_metadata_write()) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
