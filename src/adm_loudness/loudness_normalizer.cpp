#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ebur128.h>
#include <fmt/format.h>
#include <tl/expected.hpp>

#include "adm/audio_io.h"
#include "adm/errors.h"
#include "adm/logging.h"
#include "adm/loudness.h"

namespace mradm {

namespace {

struct EburFree {
    void operator()(ebur128_state* st) const noexcept { ebur128_destroy(&st); }
};
using EburStatePtr = std::unique_ptr<ebur128_state, EburFree>;

// Pass 1: measure integrated loudness (BS.1770-4 gated) in LUFS.
// Returns nullopt for silence, files too short for gating, or I/O errors.
std::optional<double> measure_lufs(const std::string& path) {
    auto reader_res = audio::FloatWavReader::open(path);
    if (!reader_res) {
        return std::nullopt;
    }
    auto& reader = *reader_res;

    const auto num_ch = reader.channels();
    const auto sample_rate = reader.sample_rate();

    EburStatePtr st{ebur128_init(num_ch, sample_rate, EBUR128_MODE_I)};
    if (!st) {
        return std::nullopt;
    }

    constexpr std::size_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(num_ch) * k_block);
    uint64_t left = reader.frame_count();

    while (left > 0) {
        const uint64_t n = std::min(static_cast<uint64_t>(k_block), left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        ebur128_add_frames_float(st.get(), buf.data(), static_cast<std::size_t>(got));
        left -= got;
    }

    double loudness = 0.0;
    if (ebur128_loudness_global(st.get(), &loudness) != EBUR128_SUCCESS) {
        return std::nullopt;
    }
    if (!std::isfinite(loudness)) {
        return std::nullopt; // silence or fully gated out
    }
    return loudness;
}

// Pass 2: rewrite path with all samples scaled by gain (float32 → float32).
[[nodiscard]] Result<void> apply_gain(const std::string& path, float gain) {
    try {
        auto reader_res = audio::FloatWavReader::open(path);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        auto& reader = *reader_res;

        const auto num_ch = reader.channels();
        const auto num_frames = reader.frame_count();
        const auto sample_rate = reader.sample_rate();

        const auto tmp_path = path + ".lufs_tmp";
        {
            auto writer_res = audio::FloatWavWriter::open(tmp_path, num_ch, sample_rate);
            if (!writer_res) {
                return tl::unexpected{writer_res.error()};
            }
            auto& writer = *writer_res;

            constexpr std::size_t k_block = 4096;
            std::vector<float> buf(static_cast<std::size_t>(num_ch) * k_block);
            uint64_t left = num_frames;

            while (left > 0) {
                const uint64_t n = std::min(static_cast<uint64_t>(k_block), left);
                const uint64_t got = reader.read(buf.data(), n);
                if (got == 0) {
                    break;
                }
                const std::size_t samples = static_cast<std::size_t>(num_ch) * static_cast<std::size_t>(got);
                for (std::size_t i = 0; i < samples; ++i) {
                    buf[i] *= gain;
                }
                const uint64_t wrote = writer.write(buf.data(), got);
                if (wrote != got) {
                    return make_error(ErrorCode::io_error, "loudness rewrite: short write", "path=" + tmp_path);
                }
                left -= got;
            }
        }
        std::filesystem::rename(tmp_path, path);
        return {};

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error,
                          std::string("loudness gain rewrite failed: ") + e.what(),
                          "path=" + path);
    }
}

} // namespace

Result<void> apply_loudness_norm(const std::string& path, float target_lufs, LogSink& logs) {
    const auto measured = measure_lufs(path);
    if (!measured.has_value()) {
        logs.log(LogLevel::warning,
                 "loudness",
                 "integrated loudness measurement failed (silence or too short) — skipping normalization");
        return {};
    }

    const double target = static_cast<double>(target_lufs);
    logs.log(LogLevel::info,
             "loudness",
             fmt::format("integrated loudness: {:.1f} LUFS, target: {:.1f} LUFS", *measured, target));

    const double gain_db = target - *measured;
    if (std::abs(gain_db) < 0.1) {
        logs.log(LogLevel::info, "loudness", "integrated loudness within tolerance — no gain applied");
        return {};
    }

    const auto gain = static_cast<float>(std::pow(10.0, gain_db / 20.0));
    logs.log(LogLevel::info,
             "loudness",
             fmt::format("applying gain {:.4f} ({:.2f} dB)", gain, gain_db));

    return apply_gain(path, gain);
}

} // namespace mradm
