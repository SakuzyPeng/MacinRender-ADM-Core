// Independent, test-only RIFF/PCM author. Deliberately does not use production WAV code.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fixture {
struct WaveAudioId {
    uint16_t track;
    std::string uid;
    std::string format;
    std::string pack;
    WaveAudioId(uint16_t index, std::string u, std::string f, std::string p)
        : track(index), uid(std::move(u)), format(std::move(f)), pack(std::move(p)) {}
};
struct WaveChna {
    std::vector<WaveAudioId> entries;
    explicit WaveChna(std::vector<WaveAudioId> values) : entries(std::move(values)) {}
};
struct WaveAxml {
    std::string xml;
    explicit WaveAxml(std::string value) : xml(std::move(value)) {}
};

class WaveWriter {
  public:
    WaveWriter(const std::string& path,
               uint16_t channels,
               uint32_t rate,
               uint16_t bits,
               const std::shared_ptr<WaveChna>& chna,
               const std::shared_ptr<WaveAxml>& axml)
        : output_(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary),
          channels_(channels), bits_(bits) {
        output_.exceptions(std::ios::failbit | std::ios::badbit);
        if (channels == 0 || rate == 0 || (bits != 16 && bits != 24 && bits != 32)) {
            throw std::runtime_error("invalid fixture WAVE format");
        }
        const auto align = static_cast<uint32_t>(channels) * (bits / 8U);
        if (align > UINT16_MAX || static_cast<uint64_t>(align) * rate > UINT32_MAX) {
            throw std::runtime_error("fixture WAVE format overflow");
        }
        output_.write("RIFF", 4);
        little(0, 4);
        output_.write("WAVEfmt ", 8);
        little(16, 4);
        little(1, 2);
        little(channels, 2);
        little(rate, 4);
        little(align * rate, 4);
        little(align, 2);
        little(bits, 2);
        if (chna) {
            if (chna->entries.size() > UINT16_MAX) {
                throw std::runtime_error("too many fixture CHNA entries");
            }
            output_.write("chna", 4);
            little(4U + chna->entries.size() * 40U, 4);
            const uint16_t tracks = std::accumulate(
                chna->entries.begin(), chna->entries.end(), uint16_t{0}, [](uint16_t count, const WaveAudioId& entry) {
                    return std::max(count, entry.track);
                });
            little(tracks, 2);
            little(chna->entries.size(), 2);
            for (const auto& entry : chna->entries) {
                little(entry.track, 2);
                fixed(entry.uid, 12);
                fixed(entry.format, 14);
                fixed(entry.pack, 11);
                little(0, 1);
            }
        }
        if (axml) {
            if (axml->xml.size() >= UINT32_MAX) {
                throw std::runtime_error("fixture AXML too large");
            }
            output_.write("axml", 4);
            little(axml->xml.size(), 4);
            output_.write(axml->xml.data(), static_cast<std::streamsize>(axml->xml.size()));
            if ((axml->xml.size() & 1U) != 0) {
                little(0, 1);
            }
        }
        output_.write("data", 4);
        little(0, 4);
        start_ = static_cast<uint64_t>(output_.tellp());
        finish();
    }
    void write(const float* samples, uint64_t frames) {
        if (frames > UINT32_MAX) {
            throw std::runtime_error("fixture frame count too large");
        }
        const auto count = frames * channels_;
        const auto byte_count = count * (bits_ / 8U);
        if (count > UINT32_MAX || byte_count > UINT32_MAX || start_ + length_ + byte_count >= UINT32_MAX) {
            throw std::runtime_error("fixture author only supports small RIFF files");
        }
        std::vector<char> packed(static_cast<std::size_t>(byte_count));
        const double maximum = static_cast<double>((uint64_t{1} << (bits_ - 1U)) - 1U);
        for (uint64_t i = 0; i < count; ++i) {
            if (std::isnan(samples[i])) {
                throw std::runtime_error("NaN fixture sample");
            }
            const auto value = static_cast<int32_t>(static_cast<double>(std::clamp(samples[i], -1.0F, 1.0F)) * maximum);
            for (uint16_t byte = 0; byte < bits_ / 8U; ++byte) {
                packed[static_cast<std::size_t>(i * (bits_ / 8U) + byte)] =
                    static_cast<char>((static_cast<uint32_t>(value) >> (byte * 8U)) & 0xffU);
            }
        }
        output_.seekp(static_cast<std::streamoff>(start_ + length_));
        output_.write(packed.data(), static_cast<std::streamsize>(packed.size()));
        length_ += byte_count;
        finish(); // Every observed write is finalized and flushed; the destructor only closes.
    }
    void finish() {
        output_.seekp(static_cast<std::streamoff>(start_ + length_));
        if ((length_ & 1U) != 0) {
            little(0, 1);
        }
        output_.seekp(4);
        little(start_ + length_ + (length_ & 1U) - 8U, 4);
        output_.seekp(static_cast<std::streamoff>(start_ - 4U));
        little(length_, 4);
        output_.flush();
    }

  private:
    void little(uint64_t value, unsigned width) {
        std::array<char, 8> data{};
        for (unsigned i = 0; i < width; ++i) {
            data[i] = static_cast<char>((value >> (i * 8U)) & 0xffU);
        }
        output_.write(data.data(), static_cast<std::streamsize>(width));
    }
    void fixed(const std::string& text, std::size_t width) {
        if (text.size() > width) {
            throw std::runtime_error("fixture CHNA identifier too long");
        }
        output_.write(text.data(), static_cast<std::streamsize>(text.size()));
        for (std::size_t i = text.size(); i < width; ++i) {
            output_.put(' ');
        }
    }
    std::ofstream output_;
    uint16_t channels_;
    uint16_t bits_;
    uint64_t start_{};
    uint64_t length_{};
};
inline std::unique_ptr<WaveWriter> write_wave(const std::string& path,
                                              uint16_t channels = 1,
                                              uint32_t rate = 48000,
                                              uint16_t bits = 24,
                                              const std::shared_ptr<WaveChna>& chna = {},
                                              const std::shared_ptr<WaveAxml>& axml = {}) {
    return std::make_unique<WaveWriter>(path, channels, rate, bits, chna, axml);
}
} // namespace fixture
