#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>

#include <sys/resource.h>
#endif

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
void save(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "fixture save failed");
}
// Mono integer PCM written directly: the reader integration is under test, not a converter.
std::vector<char> integer_wave(uint32_t rate, uint16_t bits, const std::vector<int64_t>& values) {
    const auto le = [](std::vector<char>& out, uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
        }
    };
    const uint32_t align = bits / 8U;
    const auto data = static_cast<uint32_t>(values.size()) * align;
    std::vector<char> out{'R', 'I', 'F', 'F'};
    le(out, 4U + 24U + 8U + data + (data & 1U), 4);
    for (const char c : std::string{"WAVEfmt "}) {
        out.push_back(c);
    }
    le(out, 16U, 4);
    le(out, 1U, 2);
    le(out, 1U, 2);
    le(out, rate, 4);
    le(out, uint64_t{rate} * align, 4);
    le(out, align, 2);
    le(out, bits, 2);
    for (const char c : std::string{"data"}) {
        out.push_back(c);
    }
    le(out, data, 4);
    for (const auto value : values) {
        le(out, static_cast<uint64_t>(value), static_cast<int>(align));
    }
    if ((data & 1U) != 0U) {
        out.push_back('\0');
    }
    return out;
}
void verify_native_path_round_trip(const std::filesystem::path& directory) {
    // These bytes are valid in both UTF-8 and legacy Windows code pages, where
    // they name different files. All WAVE readers/writers must use the native
    // spelling, including hosts whose manifest selects UTF-8 as the code page.
    const auto path = directory / "wave-\xC2\xA9.wav";
    write_float(path, 48000, {0.0F, 0.5F, -0.5F});
    require(std::filesystem::is_regular_file(path), "native-path fixture was not created");
    save(path, integer_wave(48000, 24, {0, 4194303, -4194303}));
    auto probe = mradm::audio::FloatWavReader::open(path.string());
    require(probe && probe->is_linear_pcm() && probe->bits_per_sample() == 24,
            "native-path integer file was not replaced");
    auto reader = mradm::audio::RenderInputReader::open(path.string());
    require(reader.has_value(), "native-path Rust reader open failed");
    std::array<float, 3> output{};
    const auto got = (*reader)->read(output.data(), output.size());
    require(got && *got == output.size(), "native-path integer read failed");
    require(output == std::array<float, 3>{0.0F, 4194303.0F / 8388608.0F, -4194303.0F / 8388608.0F},
            "native-path PCM mismatch");
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
    le(out, 4U + 24U + 8U + (2U * align), 4);
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

#ifndef _WIN32
class FileSizeLimitGuard {
  public:
    FileSizeLimitGuard() : saved_(read_limit()), previous_handler_(std::signal(SIGXFSZ, SIG_IGN)) {
        require(previous_handler_ != SIG_ERR, "cannot ignore file size signal");
        auto limited = saved_;
        limited.rlim_cur = 0;
        if (setrlimit(RLIMIT_FSIZE, &limited) != 0) {
            std::signal(SIGXFSZ, previous_handler_);
            throw std::runtime_error("cannot inject file size failure");
        }
    }
    FileSizeLimitGuard(const FileSizeLimitGuard&) = delete;
    FileSizeLimitGuard& operator=(const FileSizeLimitGuard&) = delete;
    FileSizeLimitGuard(FileSizeLimitGuard&&) = delete;
    FileSizeLimitGuard& operator=(FileSizeLimitGuard&&) = delete;
    ~FileSizeLimitGuard() {
        static_cast<void>(setrlimit(RLIMIT_FSIZE, &saved_));
        std::signal(SIGXFSZ, previous_handler_);
    }

  private:
    static rlimit read_limit() {
        rlimit limit{};
        require(getrlimit(RLIMIT_FSIZE, &limit) == 0, "cannot read file size limit");
        return limit;
    }
    rlimit saved_;
    void (*previous_handler_)(int);
};

void verify_failed_finish_is_sticky(const std::filesystem::path& directory) {
    const auto path = directory / "failed-finish.wav";
    auto writer = mradm::audio::FloatWavWriter::open(path.string(), 1, 48000);
    require(writer.has_value(), "finish failure fixture open failed");
    const float sample = 0.25F;
    require(writer->write(&sample, 1) == 1, "finish failure fixture write failed");
    const auto first = [&] {
        // Fail the header rewrite after all sample writes have succeeded, then restore I/O.
        const FileSizeLimitGuard limit;
        return writer->finish();
    }();
    require(!first && first.error().code == mradm::ErrorCode::io_error, "finish must report the I/O failure");
    const auto second = writer->finish();
    require(!second && second.error().code == first.error().code && second.error().message == first.error().message &&
                second.error().context == first.error().context,
            "repeated finish must preserve the original failure");
    require(writer->write(&sample, 1) == 0, "write after failed finish must fail");
    require(!mradm::audio::FloatWavReader::open(path.string()), "failed finish must leave an incomplete file");
}
#endif

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
                const int64_t top = (int64_t{1} << (bits - 1U)) - 1;
                const std::vector<int64_t> input{-top - 1, -top, -top / 2, 0, top / 2, top, top};
                save(path, integer_wave(rate, bits, input));
                auto probe = mradm::audio::FloatWavReader::open(path.string());
                require(probe && probe->sample_rate() == rate && probe->bits_per_sample() == bits,
                        "integer fmt mismatch");
                auto reader = mradm::audio::RenderInputReader::open(path.string());
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
#ifndef _WIN32
        verify_failed_finish_is_sticky(directory);
#endif
        const auto short_path = directory / "short.wav";
        {
            auto writer = fixture::write_wave(short_path);
            writer->write(std::vector<float>(20000, 0.25F).data(), 20000);
        }
        auto reader = mradm::audio::RenderInputReader::open(short_path.string());
        require(reader.has_value(), "truncation fixture open failed");
        std::filesystem::resize_file(short_path, 100);
        require((*reader)->seek_frame(15000).has_value(), "seek within declared input failed");
        float value{};
        require(!(*reader)->read(&value, 1), "truncated file was treated as normal EOF");
        reader->reset();
        std::filesystem::remove_all(directory);
        std::cout << "Rust WAVE integration, float RF64/BW64, high rates and truncation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }
}
