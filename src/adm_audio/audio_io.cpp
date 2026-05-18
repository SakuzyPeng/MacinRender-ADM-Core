// DR_WAV_IMPLEMENTATION must be defined in exactly one translation unit.
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/errors.h"

namespace mradm::audio {

// ── FloatWavWriter ────────────────────────────────────────────────────────────

struct FloatWavWriter::Impl {
    drwav wav{};
    bool open{false};
};

Result<FloatWavWriter> FloatWavWriter::open(const std::string& path, uint32_t channels, uint32_t sample_rate) {
    FloatWavWriter w;
    w.impl_ = std::make_unique<Impl>();

    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = channels;
    fmt.sampleRate = sample_rate;
    fmt.bitsPerSample = 32;

    if (!drwav_init_file_write(&w.impl_->wav, path.c_str(), &fmt, nullptr)) {
        return make_error(ErrorCode::io_error, "failed to open float32 WAV for writing", "path=" + path);
    }
    w.impl_->open = true;
    return w;
}

FloatWavWriter::~FloatWavWriter() {
    if (impl_ && impl_->open) {
        drwav_uninit(&impl_->wav);
    }
}

FloatWavWriter::FloatWavWriter(FloatWavWriter&&) noexcept = default;
FloatWavWriter& FloatWavWriter::operator=(FloatWavWriter&&) noexcept = default;

uint64_t FloatWavWriter::write(const float* samples, uint64_t frame_count) {
    return drwav_write_pcm_frames(&impl_->wav, frame_count, samples);
}

// ── FloatWavReader ────────────────────────────────────────────────────────────

struct FloatWavReader::Impl {
    drwav wav{};
    bool open{false};
};

Result<FloatWavReader> FloatWavReader::open(const std::string& path) {
    FloatWavReader r;
    r.impl_ = std::make_unique<Impl>();

    if (!drwav_init_file(&r.impl_->wav, path.c_str(), nullptr)) {
        return make_error(ErrorCode::io_error, "failed to open WAV for reading", "path=" + path);
    }
    r.impl_->open = true;
    return r;
}

FloatWavReader::~FloatWavReader() {
    if (impl_ && impl_->open) {
        drwav_uninit(&impl_->wav);
    }
}

FloatWavReader::FloatWavReader(FloatWavReader&&) noexcept = default;
FloatWavReader& FloatWavReader::operator=(FloatWavReader&&) noexcept = default;

uint32_t FloatWavReader::channels() const { return impl_->wav.channels; }
uint32_t FloatWavReader::sample_rate() const { return impl_->wav.sampleRate; }
uint64_t FloatWavReader::frame_count() const { return impl_->wav.totalPCMFrameCount; }

uint64_t FloatWavReader::read(float* out, uint64_t frames) {
    return drwav_read_pcm_frames_f32(&impl_->wav, frames, out);
}

// ── downconvert_to_int ────────────────────────────────────────────────────────

Result<void> downconvert_to_int(const std::string& path, uint16_t bit_depth) {
    try {
        auto reader_res = FloatWavReader::open(path);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        auto& reader = *reader_res;

        const uint32_t channels = reader.channels();
        const uint32_t sample_rate = reader.sample_rate();
        const uint64_t total_frames = reader.frame_count();

        // libbw64 0.10.0 writeFile takes uint16_t sampleRate.
        if (sample_rate > std::numeric_limits<uint16_t>::max()) {
            return make_error(
                ErrorCode::unsupported,
                fmt::format("sample rate {} Hz exceeds integer PCM writer limit (65535 Hz); use --output-bit-depth f32",
                            sample_rate),
                "path=" + path);
        }

        const auto tmp_path = path + ".bitdepth_tmp";
        {
            auto writer = bw64::writeFile(tmp_path,
                                          static_cast<uint16_t>(channels),
                                          static_cast<uint16_t>(sample_rate),
                                          bit_depth);

            constexpr uint64_t k_block = 4096;
            std::vector<float> buf(static_cast<std::size_t>(channels) * k_block);
            uint64_t left = total_frames;

            while (left > 0) {
                const uint64_t n = std::min(k_block, left);
                const uint64_t got = reader.read(buf.data(), n);
                if (got == 0) {
                    break;
                }
                writer->write(buf.data(), got);
                left -= got;
            }
        }

        std::filesystem::rename(tmp_path, path);
        return {};

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error,
                          std::string("bit depth conversion failed: ") + e.what(),
                          "path=" + path);
    }
}

} // namespace mradm::audio
