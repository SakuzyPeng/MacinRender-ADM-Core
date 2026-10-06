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
        std::cout << "Rust WAVE integration, high rates, cancellation and error preservation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }
}
