// Compares the Rust-backed FloatWavReader/FloatWavWriter with dr_wav, the implementation it replaced.
// Built only with MR_ADM_BUILD_DRWAV_REFERENCE_TESTS; see docs/architecture/RUST_DR_WAV_MIGRATION.md.
#define DR_WAV_IMPLEMENTATION
#include <array>
#include <cstdint>
#include <cstring>
#include <dr_wav.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Encoding {
    std::string_view name{};
    uint16_t tag{}; // 1 = PCM, 3 = IEEE float
    uint16_t bits{};
};

constexpr std::array<Encoding, 4> k_encodings{{{"pcm16", 1, 16}, {"pcm24", 1, 24}, {"pcm32", 1, 32}, {"f32", 3, 32}}};
constexpr uint32_t k_channels = 3;
constexpr uint32_t k_frames = 1031;

void put16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8U));
}
void put32(std::vector<uint8_t>& out, uint32_t value) {
    put16(out, value & 0xFFFFU);
    put16(out, value >> 16U);
}
void put64(std::vector<uint8_t>& out, uint64_t value) {
    put32(out, static_cast<uint32_t>(value));
    put32(out, static_cast<uint32_t>(value >> 32U));
}
void fourcc(std::vector<uint8_t>& out, std::string_view id) {
    out.insert(out.end(), id.begin(), id.end());
}

// Deterministic samples covering full-scale integers, zero and out-of-range floats.
std::vector<uint8_t> sample_bytes(const Encoding& encoding) {
    std::vector<uint8_t> data;
    uint32_t state = 0x2545F491U;
    for (uint32_t i = 0; i < k_frames * k_channels; ++i) {
        state = state * 1664525U + 1013904223U;
        uint32_t raw = state;
        if (i < 6U) {
            raw = std::array<uint32_t, 6>{0U, 0x7FFFFFFFU, 0x80000000U, 1U, 0xFFFFFFFFU, 0x40000000U}[i];
        }
        if (encoding.tag == 3U) {
            float value = static_cast<float>(static_cast<int32_t>(raw)) / 1073741824.0F; // [-2, 2)
            std::memcpy(&raw, &value, sizeof(raw));
        }
        for (uint16_t byte = 0; byte < encoding.bits / 8U; ++byte) {
            // Integer encodings keep the most significant bytes of the 32-bit draw.
            const uint32_t shift = encoding.tag == 3U ? byte * 8U : 32U - encoding.bits + byte * 8U;
            data.push_back(static_cast<uint8_t>(raw >> shift));
        }
    }
    return data;
}

std::vector<uint8_t>
wave_file(std::string_view container, const Encoding& encoding, bool extensible, const std::vector<uint8_t>& data) {
    const uint32_t align = k_channels * encoding.bits / 8U;
    std::vector<uint8_t> fmt;
    put16(fmt, extensible ? 0xFFFEU : encoding.tag);
    put16(fmt, k_channels);
    put32(fmt, 48000U);
    put32(fmt, 48000U * align);
    put16(fmt, align);
    put16(fmt, encoding.bits);
    if (extensible) {
        put16(fmt, 22U);
        put16(fmt, encoding.bits);
        put32(fmt, 0x0007U); // FL | FR | FC
        put16(fmt, encoding.tag);
        const std::array<uint8_t, 14> guid_tail{0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113};
        fmt.insert(fmt.end(), guid_tail.begin(), guid_tail.end());
    }
    const bool wide = container != "RIFF";
    const uint64_t body = 4U + (wide ? 8U + 28U : 0U) + 8U + fmt.size() + 8U + data.size() + (data.size() & 1U);
    std::vector<uint8_t> out;
    fourcc(out, container);
    put32(out, wide ? 0xFFFFFFFFU : static_cast<uint32_t>(body));
    fourcc(out, "WAVE");
    if (wide) {
        fourcc(out, "ds64");
        put32(out, 28U);
        put64(out, body);
        put64(out, data.size());
        put64(out, k_frames);
        put32(out, 0U);
    }
    fourcc(out, "fmt ");
    put32(out, static_cast<uint32_t>(fmt.size()));
    out.insert(out.end(), fmt.begin(), fmt.end());
    fourcc(out, "data");
    put32(out, wide ? 0xFFFFFFFFU : static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    if ((data.size() & 1U) != 0U) {
        out.push_back(0);
    }
    return out;
}

void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(out.good(), "fixture write failed: " + path.string());
}

class DrWav {
  public:
    explicit DrWav(const std::filesystem::path& path) {
        require(drwav_init_file(&wav_, path.string().c_str(), nullptr) != 0U, "dr_wav open failed: " + path.string());
    }
    DrWav(const DrWav&) = delete;
    DrWav& operator=(const DrWav&) = delete;
    DrWav(DrWav&&) = delete;
    DrWav& operator=(DrWav&&) = delete;
    ~DrWav() { drwav_uninit(&wav_); }
    drwav& get() { return wav_; }

  private:
    drwav wav_{};
};

// Reads with an odd block size, then seeks back, so buffering and seek paths are both compared.
std::vector<float> read_all(mradm::audio::FloatWavReader& reader) {
    std::vector<float> out(static_cast<std::size_t>(reader.frame_count()) * reader.channels());
    uint64_t done = 0;
    while (done < reader.frame_count()) {
        const uint64_t got = reader.read(out.data() + done * reader.channels(), 97U);
        require(got != 0U, "Rust short read");
        done += got;
    }
    std::vector<float> tail(static_cast<std::size_t>(reader.channels()) * 5U);
    require(reader.seek(123U) && reader.read(tail.data(), 5U) == 5U, "Rust seek/read failed");
    require(std::memcmp(tail.data(), out.data() + 123U * reader.channels(), tail.size() * sizeof(float)) == 0,
            "Rust seek returned different samples");
    return out;
}

std::vector<float> read_all(drwav& wav) {
    std::vector<float> out(static_cast<std::size_t>(wav.totalPCMFrameCount) * wav.channels);
    require(drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, out.data()) == wav.totalPCMFrameCount,
            "dr_wav short read");
    return out;
}

void compare_reads(const std::filesystem::path& root, int& cases) {
    for (const auto& encoding : k_encodings) {
        const auto data = sample_bytes(encoding);
        for (const bool extensible : {false, true}) {
            for (const std::string_view container : {"RIFF", "RF64", "BW64"}) {
                const auto name = std::string{encoding.name} + (extensible ? "-ext-" : "-") + std::string{container};
                const auto path = root / (name + ".wav");
                write_file(path, wave_file(container, encoding, extensible, data));
                // dr_wav has no BW64 support; the replaced reader spoofed BW64 as RF64. Do the same here.
                const auto reference_path = root / (name + "-reference.wav");
                write_file(reference_path,
                           wave_file(container == "BW64" ? "RF64" : container, encoding, extensible, data));

                auto candidate = mradm::audio::FloatWavReader::open(path.string());
                require(candidate.has_value(), name + ": Rust open failed");
                DrWav reference{reference_path};
                auto& wav = reference.get();
                require(candidate->channels() == wav.channels && candidate->sample_rate() == wav.sampleRate &&
                            candidate->frame_count() == wav.totalPCMFrameCount &&
                            candidate->bits_per_sample() == wav.bitsPerSample &&
                            candidate->channel_mask() == wav.fmt.channelMask &&
                            candidate->is_linear_pcm() == (wav.translatedFormatTag == DR_WAVE_FORMAT_PCM) &&
                            candidate->is_ieee_float() == (wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT),
                        name + ": format metadata differs");
                const auto expected = read_all(wav);
                const auto actual = read_all(*candidate);
                require(std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)) == 0,
                        name + ": decoded samples differ");
                ++cases;
            }
        }
    }
}

std::vector<float> float_samples() {
    std::vector<float> samples(static_cast<std::size_t>(k_frames) * k_channels);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(static_cast<double>(i % 977U) / 300.0 - 1.5);
    }
    return samples;
}

void compare_writes(const std::filesystem::path& root) {
    const auto samples = float_samples();

    // Rust writer -> dr_wav reader.
    const auto rust_path = root / "rust-writer.wav";
    write_file(rust_path, {'s', 't', 'a', 'l', 'e'}); // FloatWavWriter must replace existing output
    {
        auto writer = mradm::audio::FloatWavWriter::open(rust_path.string(), k_channels, 48000U);
        require(writer.has_value(), "Rust writer open failed");
        require(writer->write(samples.data(), k_frames) == k_frames, "Rust write failed");
        require(writer->finish().has_value(), "Rust finish failed");
    }
    {
        DrWav reference{rust_path};
        auto& wav = reference.get();
        require(wav.container == drwav_container_rf64 && wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT &&
                    wav.totalPCMFrameCount == k_frames,
                "Rust writer output is not the expected float32 RF64");
        const auto decoded = read_all(wav);
        require(std::memcmp(decoded.data(), samples.data(), samples.size() * sizeof(float)) == 0,
                "dr_wav reads different samples from Rust output");
    }

    // dr_wav writer (the replaced FloatWavWriter configuration) -> Rust reader.
    const auto drwav_path = root / "drwav-writer.wav";
    {
        drwav_data_format format{};
        format.container = drwav_container_rf64;
        format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
        format.channels = k_channels;
        format.sampleRate = 48000U;
        format.bitsPerSample = 32U;
        drwav wav{};
        require(drwav_init_file_write(&wav, drwav_path.string().c_str(), &format, nullptr) != 0U,
                "dr_wav writer open failed");
        const auto written = drwav_write_pcm_frames(&wav, k_frames, samples.data());
        drwav_uninit(&wav);
        require(written == k_frames, "dr_wav write failed");
    }
    auto reader = mradm::audio::FloatWavReader::open(drwav_path.string());
    require(reader.has_value() && reader->is_ieee_float() && reader->frame_count() == k_frames,
            "Rust reader rejects dr_wav float32 RF64 output");
    const auto decoded = read_all(*reader);
    require(std::memcmp(decoded.data(), samples.data(), samples.size() * sizeof(float)) == 0,
            "Rust reads different samples from dr_wav output");
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "mradm-drwav-reference";
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        int cases = 0;
        compare_reads(root, cases);
        compare_writes(root);
        std::filesystem::remove_all(root);
        std::cout << cases << " decode cases and float32 RF64 write/read round trips match dr_wav\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
}
