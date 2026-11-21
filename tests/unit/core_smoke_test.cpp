#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"

#include "../support/rust_services_test.h"

namespace {

[[nodiscard]] uint32_t read_be32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24U) | (static_cast<uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 2U]) << 8U) | static_cast<uint32_t>(bytes[offset + 3U]);
}

[[nodiscard]] uint32_t read_le32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<uint32_t>(bytes[offset + 2U]) << 16U) | (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] uint64_t read_be64(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint64_t>(read_be32(bytes, offset)) << 32U) | read_be32(bytes, offset + 4U);
}

[[nodiscard]] std::size_t find_caf_chunk(const std::vector<unsigned char>& bytes, std::string_view id) {
    std::size_t offset = 8U;
    while (offset + 12U <= bytes.size()) {
        const auto chunk_id = std::string_view{reinterpret_cast<const char*>(bytes.data() + offset), 4U};
        const uint64_t size = read_be64(bytes, offset + 4U);
        if (size > bytes.size() - offset - 12U) {
            break;
        }
        if (chunk_id == id) {
            return offset;
        }
        offset += 12U + static_cast<std::size_t>(size);
    }
    return std::string::npos;
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
    in.close();

    if (bytes.size() < 8U || std::string_view{reinterpret_cast<const char*>(bytes.data()), 4U} != "caff") {
        std::cerr << "CAF magic mismatch\n";
        std::filesystem::remove(path);
        return false;
    }
    const auto desc = find_caf_chunk(bytes, "desc");
    if (desc == std::string::npos || read_be64(bytes, desc + 4U) != 32U ||
        std::bit_cast<double>(read_be64(bytes, desc + 12U)) != 48000.0 ||
        std::string_view{reinterpret_cast<const char*>(bytes.data() + desc + 20U), 4U} != "lpcm" ||
        (read_be32(bytes, desc + 24U) & 3U) != 3U || read_be32(bytes, desc + 28U) != 8U ||
        read_be32(bytes, desc + 32U) != 1U || read_be32(bytes, desc + 36U) != 2U ||
        read_be32(bytes, desc + 40U) != 32U) {
        std::cerr << "CAF desc must describe 48 kHz stereo little-endian float32 PCM\n";
        std::filesystem::remove(path);
        return false;
    }
    constexpr uint32_t k_stereo_tag = (101U << 16U) | 2U;
    const auto chan = find_caf_chunk(bytes, "chan");
    if (chan == std::string::npos || read_be64(bytes, chan + 4U) < 4U || read_be32(bytes, chan + 12U) != k_stereo_tag) {
        std::cerr << "CAF stereo channel layout tag mismatch\n";
        std::filesystem::remove(path);
        return false;
    }
    const auto data = find_caf_chunk(bytes, "data");
    if (data == std::string::npos || read_be64(bytes, data + 4U) != 36U) {
        std::cerr << "CAF data chunk size mismatch\n";
        std::filesystem::remove(path);
        return false;
    }

    // Interleaved float32 samples follow the data chunk's four-byte edit count, little-endian.
    const auto sample = [&](std::size_t index) {
        return std::bit_cast<float>(read_le32(bytes, data + 16U + (index * 4U)));
    };
    if (sample(2U) != 0.25F || sample(7U) != -0.75F) {
        std::cerr << "CAF sample mismatch\n";
        std::filesystem::remove(path);
        return false;
    }

    auto bad_res = mradm::audio::FloatCafWriter::open(path.string(), 6U, 48000U, "0+2+0");
    if (bad_res || bad_res.error().code != mradm::ErrorCode::invalid_argument) {
        std::cerr << "CAF layout/channel mismatch should return invalid_argument\n";
        std::filesystem::remove(path);
        return false;
    }
    std::filesystem::remove(path);
    return true;
}

} // namespace

int main() {
    mradm_test::bind_rust_services();
    const auto run_check = [](const char* name, auto&& fn) {
        std::cerr << "RUN: " << name << "\n";
        const bool ok = fn();
        std::cerr << (ok ? "PASS: " : "FAIL: ") << name << "\n";
        return ok;
    };

    if (!run_check("verify_caf_writer", verify_caf_writer)) {
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
