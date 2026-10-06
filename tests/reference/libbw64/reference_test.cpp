// Optional independent comparison against unmodified libbw64 0.10.0.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <bw64/bw64.hpp>

#include "wav_backend.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
std::vector<char> load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
uint32_t little32(const std::vector<char>& bytes, std::size_t offset) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 4U; ++i) {
        result |= static_cast<uint32_t>(static_cast<uint8_t>(bytes.at(offset + i))) << (8U * i);
    }
    return result;
}
void append(std::vector<char>& bytes, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        bytes.push_back(static_cast<char>((value >> (i * 8U)) & 255U));
    }
}
std::pair<std::size_t, std::size_t> data_extent(const std::vector<char>& bytes) {
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const auto length = little32(bytes, at + 4);
        if (std::string(bytes.data() + at, 4) == "data") {
            require(at + 8 + length <= bytes.size(), "data extent exceeds file");
            return {at + 8, length};
        }
        at += 8U + length + (length & 1U);
    }
    throw std::runtime_error("missing data");
}
std::vector<char> pcm(const std::filesystem::path& path) {
    const auto bytes = load(path);
    const auto [start, length] = data_extent(bytes);
    return {bytes.begin() + static_cast<std::ptrdiff_t>(start),
            bytes.begin() + static_cast<std::ptrdiff_t>(start + length)};
}
void promote_fixture(const std::filesystem::path& input,
                     const std::filesystem::path& output,
                     const std::string& container,
                     uint64_t frames) {
    auto bytes = load(input);
    const auto [start, length] = data_extent(bytes);
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(start - 4), 4, static_cast<char>(255));
    std::vector<char> header(container.begin(), container.end());
    append(header, UINT32_MAX, 4);
    for (char c : std::string{"WAVEds64"}) {
        header.push_back(c);
    }
    append(header, 28, 4);
    append(header, bytes.size() + 36 - 8, 8);
    append(header, length, 8);
    append(header, frames, 8);
    append(header, 0, 4);
    std::ofstream out(output, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    out.write(bytes.data() + 12, static_cast<std::streamsize>(bytes.size() - 12));
}
std::vector<float> samples(std::size_t count) {
    const std::vector<float> edge{-1.0F,
                                  -0.5F,
                                  -1.0F / 8388608.0F,
                                  0,
                                  1.0F / 8388608.0F,
                                  0.5F,
                                  1.0F,
                                  -2.0F,
                                  2.0F,
                                  std::numeric_limits<float>::infinity()};
    std::vector<float> result(count);
    uint32_t state = 0x12345678U;
    for (std::size_t i = 0; i < count; ++i) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        result[i] = i < edge.size() ? edge[i] : static_cast<float>(static_cast<int32_t>(state)) / 1073741824.0F;
    }
    return result;
}
} // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("mradm-bw64-reference-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        require(std::filesystem::create_directory(root), "reference temp directory already exists");
        for (const auto bits : std::array<uint16_t, 3>{16, 24, 32}) {
            for (const auto channels : std::array<uint16_t, 4>{1, 2, 11, 128}) {
                const auto prefix = std::to_string(bits) + "-" + std::to_string(channels);
                const auto riff = root / (prefix + "-RIFF.wav");
                const auto input = samples(static_cast<std::size_t>(channels) * 513);
                {
                    auto writer = bw64::writeFile(riff.string(), channels, 48000, bits);
                    writer->write(input.data(), 513);
                }
                for (const std::string container : {"RIFF", "RF64", "BW64"}) {
                    auto filename = prefix;
                    filename.append("-").append(container).append(".wav");
                    const auto path = root / filename;
                    if (container != "RIFF") {
                        promote_fixture(riff, path, container, 513);
                    }
                    auto legacy = bw64::readFile(path.string());
                    auto candidate = mradm::audio::RustWavReader::open(path.string());
                    require(candidate.has_value(), "Rust reader open failed");
                    std::vector<float> expected(input.size());
                    std::vector<float> actual(input.size());
                    require(legacy->read(expected.data(), 513) == 513, "reference short read");
                    const auto read = (*candidate)->read(actual.data(), 513);
                    require(read && *read == 513, "Rust short read");
                    require(std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)) == 0,
                            "decoded PCM differs");
                    require((*candidate)->seek_frame(123).has_value(), "Rust seek failed");
                    legacy->seek(123);
                    expected.resize(channels);
                    actual.resize(channels);
                    legacy->read(expected.data(), 1);
                    require((*candidate)->read(actual.data(), 1).has_value(), "Rust seek/read failed");
                    require(std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)) == 0,
                            "seeked PCM differs");
                }
            }
            const auto input = samples(65536);
            const auto legacy_path = root / ("legacy-" + std::to_string(bits) + ".wav");
            const auto new_path = root / ("rust-" + std::to_string(bits) + ".wav");
            {
                auto writer = bw64::writeFile(legacy_path.string(), 2, 48000, bits);
                writer->write(input.data(), input.size() / 2);
            }
            auto writer = mradm::audio::IntegerWavWriter::create(new_path.string(), 2, 48000, bits);
            require(writer.has_value(), "Rust writer create failed");
            require((*writer)->write(input.data(), input.size() / 2).has_value(), "Rust write failed");
            require((*writer)->finish().has_value(), "Rust finish failed");
            require(pcm(legacy_path) == pcm(new_path), "integer PCM bytes differ");
        }
        std::filesystem::remove_all(root);
        std::cout << "36 decode/seek cases and 196608 encoded samples match libbw64 0.10.0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
}
