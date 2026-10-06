#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "adm/audio_io.h"

#include "../support/wav_fixture.h"

namespace {
void require(bool result, const char* message) {
    if (!result) {
        throw std::runtime_error(message);
    }
}
std::vector<char> load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write_float(const std::filesystem::path& path, uint32_t rate, const std::vector<float>& samples) {
    auto writer = mradm::audio::FloatWavWriter::open(path.string(), 1, rate);
    require(writer.has_value(), "float fixture open failed");
    require(writer->write(samples.data(), samples.size()) == samples.size(), "float fixture write failed");
}
void verify_native_path_round_trip(const std::filesystem::path& directory) {
    // These bytes are valid in both UTF-8 and legacy Windows code pages, where
    // they name different files. All WAVE readers/writers must use the native
    // spelling, including hosts whose manifest selects UTF-8 as the code page.
    const auto path = directory / "wave-\xC2\xA9.wav";
    write_float(path, 48000, {0.0F, 0.5F, -0.5F});
    require(std::filesystem::is_regular_file(path), "native-path fixture was not created");
    require(mradm::audio::downconvert_to_int(path.string(), 24).has_value(), "native-path integer conversion failed");
    auto probe = mradm::audio::FloatWavReader::open(path.string());
    require(probe && probe->is_linear_pcm() && probe->bits_per_sample() == 24,
            "native-path conversion did not replace the original file");
    auto reader = mradm::audio::RenderInputReader::open(path.string(), false);
    require(reader.has_value(), "native-path Rust reader open failed");
    std::array<float, 3> output{};
    const auto got = (*reader)->read(output.data(), output.size());
    require(got && *got == output.size(), "native-path integer read failed");
    require(output == std::array<float, 3>{0.0F, 4194303.0F / 8388608.0F, -4194303.0F / 8388608.0F},
            "native-path PCM mismatch");
}
void save(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "fixture save failed");
}
std::vector<char> legacy_format_wave(uint16_t tag, uint16_t bits) {
    // Mono 48 kHz, two samples. Accepted by the replaced dr_wav reader, rejected by mradm-wav.
    const auto le = [](std::vector<char>& out, uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
        }
    };
    const uint32_t align = bits / 8U;
    std::vector<char> out{'R', 'I', 'F', 'F'};
    le(out, 4U + 24U + 8U + 2U * align, 4);
    for (const char c : std::string{"WAVEfmt "}) {
        out.push_back(c);
    }
    le(out, 16U, 4);
    le(out, tag, 2);
    le(out, 1U, 2);
    le(out, 48000U, 4);
    le(out, 48000U * align, 4);
    le(out, align, 2);
    le(out, bits, 2);
    for (const char c : std::string{"data"}) {
        out.push_back(c);
    }
    le(out, 2U * align, 4);
    out.insert(out.end(), 2U * static_cast<std::size_t>(align), '\x40');
    return out;
}
void verify_float_writer_and_formats(const std::filesystem::path& directory) {
    const auto path = directory / "float.wav";
    save(path, {'s', 't', 'a', 'l', 'e'});
    const std::vector<float> input{0.25F, -1.5F, 2.0F};
    {
        auto writer = mradm::audio::FloatWavWriter::open(path.string(), 1, 48000);
        require(writer.has_value(), "float writer must replace an existing file");
        require(writer->write(input.data(), input.size()) == input.size(), "float write failed");
        require(writer->finish().has_value() && writer->finish().has_value(), "float finish failed");
        require(writer->write(input.data(), 1) == 0, "write after finish must fail");
    }
    auto bytes = load(path);
    require(bytes.size() > 4 && std::string(bytes.data(), 4) == "RF64", "float WAVE must stay RF64");

    // BW64-labelled float input previously needed a dr_wav header spoof.
    const auto bw64 = directory / "float-bw64.wav";
    std::copy_n("BW64", 4, bytes.begin());
    save(bw64, bytes);
    auto reader = mradm::audio::FloatWavReader::open(bw64.string());
    require(reader && reader->is_ieee_float() && reader->bits_per_sample() == 32 && reader->frame_count() == 3,
            "BW64 float fmt mismatch");
    std::vector<float> output(input.size());
    require(reader->read(output.data(), output.size()) == output.size() && output == input, "BW64 float samples");

    for (const auto& [tag, bits] : {std::pair<uint16_t, uint16_t>{1, 8}, {6, 8}, {3, 64}}) {
        const auto legacy = directory / ("legacy-" + std::to_string(tag) + "-" + std::to_string(bits) + ".wav");
        save(legacy, legacy_format_wave(tag, bits));
        auto rejected = mradm::audio::FloatWavReader::open(legacy.string());
        require(!rejected && rejected.error().code == mradm::ErrorCode::unsupported,
                "8-bit, A-law and float64 WAVE must be reported as unsupported");
    }
}
class CancelProgress final : public mradm::ProgressSink {
  public:
    explicit CancelProgress(std::stop_source& source) : source_(&source) {}
    void on_progress(const mradm::ProgressEvent& progress) override {
        if (progress.current_frame > 0) {
            source_->request_stop();
        }
    }

  private:
    std::stop_source* source_;
};
} // namespace

int main() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("mradm-wav-rust-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        require(std::filesystem::create_directory(directory), "fixture directory exists");
        for (const auto rate : {48000U, 96000U, 192000U}) {
            for (const auto bits : std::array<uint16_t, 3>{16, 24, 32}) {
                const auto path = directory / (std::to_string(rate) + "-" + std::to_string(bits) + ".wav");
                const std::vector<float> input{-2.0F, -1.0F, -0.5F, 0.0F, 0.5F, 1.0F, 2.0F};
                write_float(path, rate, input);
                require(mradm::audio::downconvert_to_int(path.string(), bits).has_value(), "integer conversion failed");
                auto probe = mradm::audio::FloatWavReader::open(path.string());
                require(probe && probe->sample_rate() == rate && probe->bits_per_sample() == bits,
                        "integer fmt mismatch");
                auto reader = mradm::audio::RenderInputReader::open(path.string(), false);
                require(reader.has_value(), "Rust-backed reader open failed");
                std::vector<float> output(input.size());
                const auto got = (*reader)->read(output.data(), output.size());
                require(got && *got == output.size(), "integer read failed");
                require(output.front() < -0.99F && output.back() > 0.99F, "full scale wrapped");
                require((*reader)->seek_frame(UINT64_MAX).has_value(), "64-bit EOF seek failed");
                const auto eof = (*reader)->read(output.data(), output.size());
                require(eof && *eof == 0, "reader passed audio EOF");
            }
        }
        verify_native_path_round_trip(directory);
        verify_float_writer_and_formats(directory);
        const auto invalid = directory / "nan.wav";
        write_float(invalid, 48000, {0.5F, std::numeric_limits<float>::quiet_NaN()});
        const auto original = load(invalid);
        const auto bad = mradm::audio::downconvert_to_int(invalid.string(), 24);
        require(!bad && bad.error().code == mradm::ErrorCode::invalid_argument, "NaN must return invalid_argument");
        require(load(invalid) == original, "failed conversion replaced original");

        const auto cancelled = directory / "cancel.wav";
        write_float(cancelled, 48000, std::vector<float>(12000, 0.25F));
        const auto before = load(cancelled);
        std::stop_source source;
        CancelProgress progress{source};
        const auto result = mradm::audio::downconvert_to_int(cancelled.string(), 24, source.get_token(), &progress);
        require(!result && result.error().code == mradm::ErrorCode::cancelled, "conversion did not cancel");
        require(load(cancelled) == before, "cancelled conversion replaced original");

        const auto short_path = directory / "short.wav";
        {
            auto writer = fixture::write_wave(short_path.string());
            writer->write(std::vector<float>(20000, 0.25F).data(), 20000);
        }
        auto reader = mradm::audio::RenderInputReader::open(short_path.string(), false);
        require(reader.has_value(), "truncation fixture open failed");
        std::filesystem::resize_file(short_path, 100);
        require((*reader)->seek_frame(15000).has_value(), "seek within declared input failed");
        float value{};
        require(!(*reader)->read(&value, 1), "truncated file was treated as normal EOF");
        reader->reset();
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            require(entry.path().filename().string().find("bitdepth_tmp") == std::string::npos,
                    "temporary output leaked");
        }
        std::filesystem::remove_all(directory);
        std::cout << "Rust WAVE integration, float RF64/BW64, high rates, cancellation and error preservation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }
}
