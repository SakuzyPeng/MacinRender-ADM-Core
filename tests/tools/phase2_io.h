#pragma once
// Measurement-only canonical wire data. No native struct padding or decimal PCM.
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

namespace phase2 {
inline void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
inline void word(std::ostream& out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        out.put(static_cast<char>((value >> (i * 8U)) & 255U));
    }
}
inline void
pcm(const std::filesystem::path& path, std::uint32_t channels, std::uint32_t rate, std::span<const float> values) {
    require(channels != 0 && rate != 0 && values.size() % channels == 0, "invalid PCM shape");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write("MRPB", 4);
    word(out, 1, 4);
    word(out, channels, 4);
    word(out, rate, 4);
    word(out, values.size() / channels, 8);
    for (float value : values) {
        require(std::isfinite(value), "non-finite replay PCM");
        word(out, std::bit_cast<std::uint32_t>(value), 4);
    }
    out.close();
}
inline std::vector<float> signal(std::size_t size, std::uint32_t seed, float scale = 0.125F) {
    std::vector<float> result(size);
    for (auto& value : result) {
        seed = seed * 1664525U + 1013904223U;
        value = static_cast<float>(static_cast<std::int32_t>(seed >> 8U) - 8388608) / 8388608.0F * scale;
    }
    return result;
}
} // namespace phase2
