#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <tl/expected.hpp>

#include "adm/audio_io.h"
#include "adm/errors.h"
#include "adm/logging.h"
#include "adm/loudness.h"

#include "../adm_render_common/consistency_trace.h"
#include "meter.h"

namespace mradm {

namespace {

// Pass 1: measure integrated loudness (BS.1770-4 gated) in LUFS.
// Returns nullopt for silence or files too short for gating.
Result<std::optional<double>> measure_lufs(const std::string& path) {
    auto reader_res = audio::FloatWavReader::open(path);
    if (!reader_res) {
        return tl::unexpected{reader_res.error()};
    }
    auto& reader = *reader_res;

    const auto num_ch = reader.channels();
    const auto sample_rate = reader.sample_rate();

    auto st = dsp::Meter::create(num_ch, sample_rate, dsp::MeterMode::integrated);
    if (!st) {
        return tl::unexpected{st.error()};
    }

    constexpr std::size_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(num_ch) * k_block);
    uint64_t left = reader.frame_count();

    while (left > 0) {
        const uint64_t n = std::min(static_cast<uint64_t>(k_block), left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            return make_error(ErrorCode::io_error, "short read while measuring integrated loudness", "path=" + path);
        }
        if (const auto result = st->add_frames(buf.data(), static_cast<std::size_t>(got)); !result) {
            return tl::unexpected{result.error()};
        }
        left -= got;
    }

    const auto loudness = st->integrated();
    if (!loudness) {
        return tl::unexpected{loudness.error()};
    }
    if (!std::isfinite(*loudness)) {
        return std::nullopt; // silence or fully gated out
    }
    return *loudness;
}

// Pass 2: rewrite path with all samples scaled by gain (float32 → float32).
[[nodiscard]] Result<void> apply_gain(const std::string& path, float gain) {
    try {
        const auto tmp_path = path + ".lufs_tmp";
        {
            // reader 与 writer 都置于此块内，块结束即关闭句柄；之后才能在 Windows 上
            // rename 顶替原文件（POSIX 可 rename 顶替正打开的文件，Windows 会 Access denied）。
            auto reader_res = audio::FloatWavReader::open(path);
            if (!reader_res) {
                return tl::unexpected{reader_res.error()};
            }
            auto& reader = *reader_res;

            const auto num_ch = reader.channels();
            const auto num_frames = reader.frame_count();
            const auto sample_rate = reader.sample_rate();

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
                    return make_error(ErrorCode::io_error, "short read while applying loudness gain", "path=" + path);
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
            if (auto finished = writer.finish(); !finished) {
                return tl::unexpected{finished.error()};
            }
        }
        std::filesystem::rename(tmp_path, path);
        return {};

    } catch (const std::exception& e) {
        return make_error(
            ErrorCode::io_error, std::string("loudness gain rewrite failed: ") + e.what(), "path=" + path);
    }
}

} // namespace

Result<void> apply_loudness_norm(const std::string& path, float target_lufs, LogSink& logs) {
    const auto measured_res = measure_lufs(path);
    if (!measured_res) {
        return tl::unexpected{measured_res.error()};
    }
    const auto measured_opt = *measured_res;
    if (!measured_opt.has_value()) {
        logs.log(LogLevel::warning,
                 "loudness",
                 "integrated loudness unavailable (silence or too short) — skipping normalization");
        return {};
    }
    const double measured = measured_opt.value();

    const auto target = static_cast<double>(target_lufs);
    logs.log(LogLevel::info,
             "loudness",
             fmt::format("integrated loudness: {:.1f} LUFS, target: {:.1f} LUFS", measured, target));

    const double gain_db = target - measured;
    if (std::abs(gain_db) < 0.1) {
        logs.log(LogLevel::info, "loudness", "integrated loudness within tolerance — no gain applied");
        return {};
    }

    const auto gain = static_cast<float>(std::pow(10.0, gain_db / 20.0));
    logs.log(LogLevel::info, "loudness", fmt::format("applying gain {:.4f} ({:.2f} dB)", gain, gain_db));

    consistency::dump("post/loudness.10-measurement.f64", {measured, target, gain_db});
    consistency::dump("post/loudness.20-gain.f32", {gain});
    return apply_gain(path, gain);
}

} // namespace mradm
