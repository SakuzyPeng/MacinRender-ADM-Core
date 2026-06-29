#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

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

[[nodiscard]] uint64_t read_le64(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return static_cast<uint64_t>(read_le32(bytes, offset)) |
           (static_cast<uint64_t>(read_le32(bytes, offset + 4U)) << 32U);
}

[[nodiscard]] uint64_t read_be64(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint64_t>(read_be32(bytes, offset)) << 32U) | read_be32(bytes, offset + 4U);
}

[[nodiscard]] std::size_t find_riff_chunk(const std::vector<unsigned char>& bytes, std::string_view id) {
    if (bytes.size() < 12U) {
        return std::string::npos;
    }
    const auto riff_id = std::string_view{reinterpret_cast<const char*>(bytes.data()), 4U};
    const bool uses_ds64 = riff_id == "RF64" || riff_id == "BW64";
    bool have_ds64 = false;
    uint64_t ds64_data_size = 0;
    std::size_t offset = 12U;
    while (offset + 8U <= bytes.size()) {
        const auto chunk_id = std::string_view{reinterpret_cast<const char*>(bytes.data() + offset), 4U};
        const uint32_t size = read_le32(bytes, offset + 4U);
        uint64_t payload_size = size;
        if (uses_ds64 && chunk_id == "ds64" && size >= 28U && offset + 36U <= bytes.size()) {
            have_ds64 = true;
            ds64_data_size = read_le64(bytes, offset + 16U);
        } else if (uses_ds64 && chunk_id == "data" && size == 0xFFFFFFFFU) {
            if (!have_ds64) {
                return std::string::npos;
            }
            payload_size = ds64_data_size;
        }
        if (chunk_id == id) {
            return offset;
        }
        const uint64_t next = static_cast<uint64_t>(offset) + 8U + payload_size + (payload_size & 1U);
        if (next <= offset || next > bytes.size()) {
            break;
        }
        offset = static_cast<std::size_t>(next);
    }
    return std::string::npos;
}

[[nodiscard]] uint64_t read_le64_at(const std::filesystem::path& path, uint64_t offset) {
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, 8> bytes{};
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::vector<unsigned char> v(bytes.begin(), bytes.end());
    return read_le64(v, 0U);
}

void write_le64_stream(std::ostream& out, uint64_t value) {
    const std::array<unsigned char, 8> bytes{static_cast<unsigned char>(value),
                                             static_cast<unsigned char>(value >> 8U),
                                             static_cast<unsigned char>(value >> 16U),
                                             static_cast<unsigned char>(value >> 24U),
                                             static_cast<unsigned char>(value >> 32U),
                                             static_cast<unsigned char>(value >> 40U),
                                             static_cast<unsigned char>(value >> 48U),
                                             static_cast<unsigned char>(value >> 56U)};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

#ifdef _WIN32
bool mark_file_sparse(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.wstring().c_str(),
                              GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes_returned, nullptr);
    CloseHandle(file);
    return ok != FALSE;
}
#endif

// NTFS does not make resize_file sparse automatically (APFS/ext4 do); mark the file sparse
// first on Windows so a multi-GB resize stays a hole instead of physically allocating.
bool resize_sparse_file(const std::filesystem::path& path, uint64_t size) {
#ifdef _WIN32
    if (!mark_file_sparse(path)) {
        return false;
    }
#endif
    std::error_code ec;
    std::filesystem::resize_file(path, size, ec);
    return !ec;
}

[[nodiscard]] bool verify_wav_container_size(const std::vector<unsigned char>& bytes,
                                             uint64_t expected_data_bytes,
                                             std::string_view label) {
    if (bytes.size() < 12U) {
        std::cerr << label << ": WAV file too small\n";
        return false;
    }
    const auto riff_id = std::string_view{reinterpret_cast<const char*>(bytes.data()), 4U};
    if (riff_id == "RIFF") {
        if (read_le32(bytes, 4U) != bytes.size() - 8U) {
            std::cerr << label << ": RIFF size mismatch\n";
            return false;
        }
        return true;
    }
    if (riff_id != "RF64" && riff_id != "BW64") {
        std::cerr << label << ": WAV container tag mismatch\n";
        return false;
    }
    const auto ds64 = find_riff_chunk(bytes, "ds64");
    if (ds64 == std::string::npos || read_le32(bytes, ds64 + 4U) < 28U) {
        std::cerr << label << ": ds64 chunk missing\n";
        return false;
    }
    if (riff_id == "RF64" && read_le32(bytes, 4U) != 0xFFFFFFFFU) {
        std::cerr << label << ": RF64 size sentinel mismatch\n";
        return false;
    }
    if (read_le64(bytes, ds64 + 8U) != bytes.size() - 8U || read_le64(bytes, ds64 + 16U) != expected_data_bytes) {
        std::cerr << label << ": ds64 size mismatch\n";
        return false;
    }
    return true;
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
    const auto found = std::ranges::search(
        bytes, needle, {}, [](unsigned char b) { return static_cast<char>(b); }, [](char c) { return c; });
    return !found.empty();
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
    in.close();
    const auto bext = find_riff_chunk(bytes, "bext");
    if (bext == std::string::npos) {
        std::cerr << "WAV bext chunk missing\n";
        std::filesystem::remove(path);
        return false;
    }
    if (!verify_wav_container_size(bytes, uint64_t{2} * sizeof(float), "WAV metadata") ||
        read_le32(bytes, bext + 4U) != 602U) {
        std::cerr << "WAV bext or container size mismatch\n";
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

[[nodiscard]] bool verify_wav_hoa3_ambi_metadata() {
    const auto path = std::filesystem::temp_directory_path() / "mr_core_hoa3_ambi_test.wav";
    std::filesystem::remove(path);

    {
        auto writer_res = mradm::audio::WriterHandle::open(path.string(), 16U, 48000U, "hoa3");
        if (!writer_res) {
            std::cerr << "HOA3 WAV writer open failed: " << writer_res.error().message << "\n";
            return false;
        }
        const std::vector<float> samples(16U, 0.0F);
        if (writer_res->write(samples.data(), 1U) != 1U) {
            std::cerr << "HOA3 WAV writer short write\n";
            std::filesystem::remove(path);
            return false;
        }
    }

    auto meta = test_metadata();
    meta.output_layout = "hoa3";
    const auto meta_res = mradm::audio::write_file_metadata(path.string(), meta);
    if (!meta_res) {
        std::cerr << "HOA3 WAV metadata write failed: " << meta_res.error().message << "\n";
        std::filesystem::remove(path);
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    const auto ambi = find_riff_chunk(bytes, "ambi");
    if (ambi == std::string::npos || read_le32(bytes, ambi + 4U) != 16U) {
        std::cerr << "HOA3 WAV ambi chunk missing or wrong size\n";
        std::filesystem::remove(path);
        return false;
    }
    if (read_le32(bytes, ambi + 8U) != 1U || read_le32(bytes, ambi + 12U) != 2U || read_le32(bytes, ambi + 16U) != 2U ||
        read_le32(bytes, ambi + 20U) != 16U ||
        !verify_wav_container_size(bytes, uint64_t{16} * sizeof(float), "HOA3 WAV metadata")) {
        std::cerr << "HOA3 WAV ambi payload mismatch\n";
        std::filesystem::remove(path);
        return false;
    }

    std::filesystem::remove(path);
    return true;
}

[[nodiscard]] bool verify_rf64_sparse_metadata_write() {
    const auto path = std::filesystem::temp_directory_path() / "mr_core_rf64_sparse_metadata_test.wav";
    std::filesystem::remove(path);

    {
        auto writer_res = mradm::audio::WriterHandle::open(path.string(), 1U, 48000U, "0+2+0");
        if (!writer_res) {
            std::cerr << "RF64 sparse writer open failed: " << writer_res.error().message << "\n";
            return false;
        }
        float sample = 0.0F;
        if (writer_res->write(&sample, 1U) != 1U) {
            std::cerr << "RF64 sparse writer short write\n";
            std::filesystem::remove(path);
            return false;
        }
    }

    std::ifstream small_in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(small_in)), {});
    small_in.close();
    const auto ds64 = find_riff_chunk(bytes, "ds64");
    const auto data = find_riff_chunk(bytes, "data");
    if (ds64 == std::string::npos || data == std::string::npos) {
        std::cerr << "RF64 sparse seed missing ds64/data chunk\n";
        std::filesystem::remove(path);
        return false;
    }

    constexpr uint64_t k_big_data_bytes = (uint64_t{1} << 32U) + 4096U;
    const uint64_t data_payload_offset = static_cast<uint64_t>(data) + 8U;
    const uint64_t sparse_size = data_payload_offset + k_big_data_bytes;
    // NTFS does not make resize_file sparse automatically (APFS/ext4 do); without the
    // FSCTL_SET_SPARSE marker a multi-GB resize physically allocates on Windows and stalls CI.
    if (!resize_sparse_file(path, sparse_size)) {
        std::cerr << "RF64 sparse resize failed\n";
        std::filesystem::remove(path);
        return false;
    }

    {
        std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
        out.seekp(static_cast<std::streamoff>(ds64 + 8U));
        write_le64_stream(out, sparse_size - 8U);
        write_le64_stream(out, k_big_data_bytes);
        write_le64_stream(out, k_big_data_bytes / sizeof(float));
        if (!out) {
            std::cerr << "RF64 sparse ds64 patch failed\n";
            std::filesystem::remove(path);
            return false;
        }
    }

    const auto meta_res = mradm::audio::write_file_metadata(path.string(), test_metadata());
    if (!meta_res) {
        std::cerr << "RF64 sparse metadata write failed: " << meta_res.error().message << "\n";
        std::filesystem::remove(path);
        return false;
    }

    const uint64_t final_size = std::filesystem::file_size(path);
    constexpr uint64_t k_bext_total = 8U + 602U;
    if (final_size != sparse_size + k_bext_total) {
        std::cerr << "RF64 sparse final file size mismatch\n";
        std::filesystem::remove(path);
        return false;
    }
    if (read_le64_at(path, ds64 + 8U) != final_size - 8U || read_le64_at(path, ds64 + 16U) != k_big_data_bytes) {
        std::cerr << "RF64 sparse ds64 values mismatch after metadata\n";
        std::filesystem::remove(path);
        return false;
    }

    std::ifstream tail(path, std::ios::binary);
    std::array<char, 8> bext_hdr{};
    tail.seekg(static_cast<std::streamoff>(sparse_size));
    tail.read(bext_hdr.data(), static_cast<std::streamsize>(bext_hdr.size()));
    const auto bext_size = static_cast<uint32_t>(static_cast<unsigned char>(bext_hdr[4])) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(bext_hdr[5])) << 8U) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(bext_hdr[6])) << 16U) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(bext_hdr[7])) << 24U);
    if (!tail || std::string_view{bext_hdr.data(), 4U} != "bext" || bext_size != 602U) {
        std::cerr << "RF64 sparse bext not appended after data payload\n";
        std::filesystem::remove(path);
        return false;
    }

    std::filesystem::remove(path);
    return true;
}

[[nodiscard]] bool verify_caf_wav71_layout_tag() {
    const auto path = std::filesystem::temp_directory_path() / "mr_core_caf_wav71_test.caf";
    std::filesystem::remove(path);

    {
        auto writer_res = mradm::audio::WriterHandle::open(path.string(), 8U, 48000U, "wav71");
        if (!writer_res) {
            std::cerr << "CAF wav71 writer open failed: " << writer_res.error().message << "\n";
            return false;
        }
        const std::vector<float> samples(8U, 0.0F);
        if (writer_res->write(samples.data(), 1U) != 1U) {
            std::cerr << "CAF wav71 writer short write\n";
            std::filesystem::remove(path);
            return false;
        }
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    const auto chan = find_caf_chunk(bytes, "chan");
    constexpr uint32_t k_wave_71_tag = (189U << 16U) | 8U;
    if (chan == std::string::npos || read_be32(bytes, chan + 12U) != k_wave_71_tag) {
        std::cerr << "CAF wav71 channel layout tag mismatch\n";
        std::filesystem::remove(path);
        return false;
    }

    auto old_res = mradm::audio::WriterHandle::open(path.string(), 8U, 48000U, "0+7+0");
    if (old_res || old_res.error().code != mradm::ErrorCode::unsupported) {
        std::cerr << "CAF old 0+7+0 layout id should be unsupported\n";
        std::filesystem::remove(path);
        return false;
    }

    std::filesystem::remove(path);
    return true;
}

[[nodiscard]] bool verify_caf_binaural_layout_tag() {
    const auto path = std::filesystem::temp_directory_path() / "mr_core_caf_binaural_test.caf";
    std::filesystem::remove(path);

    {
        auto writer_res = mradm::audio::WriterHandle::open(path.string(), 2U, 48000U, "binaural");
        if (!writer_res) {
            std::cerr << "CAF binaural writer open failed: " << writer_res.error().message << "\n";
            return false;
        }
        const std::vector<float> samples(2U, 0.0F);
        if (writer_res->write(samples.data(), 1U) != 1U) {
            std::cerr << "CAF binaural writer short write\n";
            std::filesystem::remove(path);
            return false;
        }
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    const auto chan = find_caf_chunk(bytes, "chan");
    constexpr uint32_t k_binaural_tag = (106U << 16U) | 2U;
    if (chan == std::string::npos || read_be32(bytes, chan + 12U) != k_binaural_tag) {
        std::cerr << "CAF binaural channel layout tag mismatch\n";
        std::filesystem::remove(path);
        return false;
    }

    std::filesystem::remove(path);
    return true;
}

[[nodiscard]] bool verify_caf_hoa3_layout_tag() {
    const auto path = std::filesystem::temp_directory_path() / "mr_core_caf_hoa3_test.caf";
    std::filesystem::remove(path);

    {
        auto writer_res = mradm::audio::WriterHandle::open(path.string(), 16U, 48000U, "hoa3");
        if (!writer_res) {
            std::cerr << "CAF HOA3 writer open failed: " << writer_res.error().message << "\n";
            return false;
        }
        const std::vector<float> samples(16U, 0.0F);
        if (writer_res->write(samples.data(), 1U) != 1U) {
            std::cerr << "CAF HOA3 writer short write\n";
            std::filesystem::remove(path);
            return false;
        }
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    const auto chan = find_caf_chunk(bytes, "chan");
    constexpr uint32_t k_hoa3_tag = (190U << 16U) | 16U;
    if (chan == std::string::npos || read_be32(bytes, chan + 12U) != k_hoa3_tag) {
        std::cerr << "CAF HOA3 channel layout tag mismatch\n";
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
    in.close();
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

[[nodiscard]] bool verify_caf_writer() { // NOLINT(readability-function-size)
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
    in.close(); // 关闭后才能让下方 apply_gain_to_file 在 Windows 上 rename 顶替 path（Windows 不能替换打开的文件）

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
    const auto run_check = [](const char* name, auto&& fn) {
        std::cerr << "RUN: " << name << "\n";
        const bool ok = fn();
        std::cerr << (ok ? "PASS: " : "FAIL: ") << name << "\n";
        return ok;
    };

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

    if (!run_check("verify_caf_writer", verify_caf_writer)) {
        return EXIT_FAILURE;
    }
    if (!run_check("verify_wav_metadata_write", verify_wav_metadata_write)) {
        return EXIT_FAILURE;
    }
    if (!run_check("verify_wav_hoa3_ambi_metadata", verify_wav_hoa3_ambi_metadata)) {
        return EXIT_FAILURE;
    }
    if (!run_check("verify_rf64_sparse_metadata_write", verify_rf64_sparse_metadata_write)) {
        return EXIT_FAILURE;
    }
    if (!run_check("verify_caf_wav71_layout_tag", verify_caf_wav71_layout_tag)) {
        return EXIT_FAILURE;
    }
    if (!run_check("verify_caf_binaural_layout_tag", verify_caf_binaural_layout_tag)) {
        return EXIT_FAILURE;
    }
    if (!run_check("verify_caf_hoa3_layout_tag", verify_caf_hoa3_layout_tag)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
