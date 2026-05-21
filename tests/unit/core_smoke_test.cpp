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

[[nodiscard]] uint64_t read_be64(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint64_t>(read_be32(bytes, offset)) << 32U) | read_be32(bytes, offset + 4U);
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
    std::filesystem::remove(path);

    if (bytes.size() < 92U) {
        std::cerr << "CAF file too small\n";
        return false;
    }
    if (std::string_view{reinterpret_cast<const char*>(bytes.data()), 4U} != "caff") {
        std::cerr << "CAF magic mismatch\n";
        return false;
    }
    constexpr uint32_t k_stereo_tag = (101U << 16U) | 2U;
    if (read_be32(bytes, 64U) != k_stereo_tag) {
        std::cerr << "CAF stereo channel layout tag mismatch\n";
        return false;
    }
    if (read_be64(bytes, 80U) != 36U) {
        std::cerr << "CAF data chunk size mismatch\n";
        return false;
    }

    auto bad_res = mradm::audio::FloatCafWriter::open(path.string(), 6U, 48000U, "0+2+0");
    if (bad_res || bad_res.error().code != mradm::ErrorCode::invalid_argument) {
        std::cerr << "CAF layout/channel mismatch should return invalid_argument\n";
        return false;
    }
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

    return EXIT_SUCCESS;
}
