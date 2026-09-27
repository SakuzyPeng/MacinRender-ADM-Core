#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adm/audio_io.h"

#include "wav_output.h"

namespace {
class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

  private:
    std::filesystem::path path_;
};

struct Wave {
    std::string container;
    std::vector<uint8_t> format;
    std::vector<uint8_t> pcm;
    uint32_t fact_frames{};
    bool axml{};
    bool chna{};
};

uint32_t little32(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes.at(offset)) | (static_cast<uint32_t>(bytes.at(offset + 1)) << 8U) |
           (static_cast<uint32_t>(bytes.at(offset + 2)) << 16U) | (static_cast<uint32_t>(bytes.at(offset + 3)) << 24U);
}

Wave read_wave(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (bytes.size() < 12) {
        throw std::runtime_error("missing WAVE fixture header");
    }
    Wave result;
    result.container = std::string(bytes.begin(), bytes.begin() + 4);
    std::size_t offset = 12;
    uint64_t data_size = 0;
    while (offset + 8 <= bytes.size()) {
        const std::string id(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4));
        uint64_t size = little32(bytes, offset + 4);
        offset += 8;
        if (id == "ds64") {
            data_size = little32(bytes, offset + 8) | (static_cast<uint64_t>(little32(bytes, offset + 12)) << 32U);
        } else if (id == "data" && size == 0xFFFFFFFFU) {
            size = data_size;
        }
        if (offset + size > bytes.size()) {
            throw std::runtime_error("truncated WAVE fixture");
        }
        if (id == "fmt " || id == "data") {
            auto& destination = id == "fmt " ? result.format : result.pcm;
            destination.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                               bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
        } else if (id == "fact") {
            result.fact_frames = little32(bytes, offset);
        }
        result.axml = result.axml || id == "axml";
        result.chna = result.chna || id == "chna";
        offset += static_cast<std::size_t>(size + (size & 1U));
    }
    return result;
}

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool verify(const std::string& layout, uint16_t bits, mradm::engine::WavLayoutProfile profile) {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("mr_wav_profile_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    const FileGuard cleanup{path};
    const uint32_t channels = layout == "7.1.4" ? 12U : 16U;
    constexpr uint32_t k_frames = 32;
    std::vector<float> samples(static_cast<std::size_t>(k_frames) * channels);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(i) / 2048.0F;
    }
    {
        auto writer = mradm::audio::FloatWavWriter::open(path.string(), channels, 48000);
        if (!writer || writer->write(samples.data(), k_frames) != k_frames) {
            return false;
        }
    }
    if (bits == 24 && !mradm::audio::downconvert_to_int(path.string(), bits)) {
        return false;
    }
    const auto before = read_wave(path);
    const auto result = mradm::engine::finalize_rendered_wav(path.string(), layout, {}, nullptr, profile);
    if (!result) {
        std::cerr << result.error().message << '\n';
        return false;
    }
    const auto after = read_wave(path);
    if (profile == mradm::engine::WavLayoutProfile::adm_semantics) {
        return check(after.axml && after.chna && after.pcm == before.pcm,
                     "default spatial WAV retains ADM metadata and PCM");
    }
    bool ok = check(after.container == "RIFF" && !after.axml && !after.chna,
                    "speaker re-render profile uses small RIFF with no rendered-ADM track") &&
              check(after.format.size() == 40 && little32(after.format, 0) == ((channels << 16U) | 0xFFFEU),
                    "speaker re-render profile explicitly writes WAVEFORMATEXTENSIBLE") &&
              check(after.fact_frames == k_frames, "speaker re-render profile retains frame count for PCM and float");
    if (after.format.size() != 40) {
        return false;
    }
    ok &= check(little32(after.format, 24) == (bits == 32 ? 3U : 1U), "subformat retains the selected sample type");
    ok &= check(little32(after.format, 20) == (channels == 12 ? 0x2D63FU : 0U), "reference channel-mask behavior");
    if (channels == 16) {
        return check(after.pcm == before.pcm, "9.1.6 PCM bytes and native order remain unchanged") && ok;
    }
    constexpr std::array<std::size_t, 12> k_sources{0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11};
    const std::size_t width = bits / 8U;
    for (std::size_t frame = 0; frame < k_frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            for (std::size_t byte = 0; byte < width; ++byte) {
                if (after.pcm.at(((frame * channels + channel) * width) + byte) !=
                    before.pcm.at(((frame * channels + k_sources.at(channel)) * width) + byte)) {
                    return check(false, "7.1.4 preserves the reference WAVE side/rear order");
                }
            }
        }
    }
    return ok;
}
} // namespace

int main() {
    try {
        bool ok = true;
        for (const uint16_t bits : {uint16_t{24}, uint16_t{32}}) {
            for (const auto* layout : {"7.1.4", "9.1.6"}) {
                ok &= verify(layout, bits, mradm::engine::WavLayoutProfile::speaker_rerender);
            }
            ok &= verify("9.1.6", bits, mradm::engine::WavLayoutProfile::adm_semantics);
        }
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
