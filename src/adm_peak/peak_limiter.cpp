#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/peak.h"

#include "meter.h"

namespace mradm {

namespace {

// Pass 1: feed all samples from path into a Rust meter for True Peak.
// Returns the maximum True Peak in linear amplitude across all channels.
[[nodiscard]] Result<double> measure_true_peak(const std::string& path) {
    auto reader_res = audio::FloatWavReader::open(path);
    if (!reader_res) {
        return tl::unexpected{reader_res.error()};
    }
    auto& reader = *reader_res;
    const auto num_ch = reader.channels();
    const auto sample_rate = reader.sample_rate();

    auto st = dsp::Meter::create(num_ch, sample_rate, dsp::MeterMode::true_peak);
    if (!st) {
        return tl::unexpected{st.error()};
    }

    constexpr std::size_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(num_ch) * k_block);
    uint64_t frames_left = reader.frame_count();

    while (frames_left > 0) {
        const uint64_t n = std::min(static_cast<uint64_t>(k_block), frames_left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            return make_error(ErrorCode::io_error, "short read while measuring True Peak", "path=" + path);
        }
        if (const auto result = st->add_frames(buf.data(), static_cast<std::size_t>(got)); !result) {
            return tl::unexpected{result.error()};
        }
        frames_left -= got;
    }

    return st->max_true_peak();
}

// Pass 2: rewrite path with all samples scaled by gain.
[[nodiscard]] Result<void> apply_gain(const std::string& path, float gain) {
    try {
        const auto tmp_path = path + ".peak_tmp";
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
            uint64_t frames_left = num_frames;

            while (frames_left > 0) {
                const uint64_t n = std::min(static_cast<uint64_t>(k_block), frames_left);
                const uint64_t got = reader.read(buf.data(), n);
                if (got == 0) {
                    return make_error(ErrorCode::io_error, "short read while applying peak gain", "path=" + path);
                }
                const std::size_t samples = static_cast<std::size_t>(num_ch) * static_cast<std::size_t>(got);
                for (std::size_t i = 0; i < samples; ++i) {
                    buf[i] *= gain;
                }
                if (writer.write(buf.data(), got) != got) {
                    return make_error(ErrorCode::io_error, "short write while applying peak gain", "path=" + path);
                }
                frames_left -= got;
            }
        }
        std::filesystem::rename(tmp_path, path);
        return {};

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("peak limit rewrite failed: ") + e.what(), "path=" + path);
    }
}

} // namespace

Result<void> apply_peak_limit(const std::string& path, float target_dbtp, LogSink& logs) {
    const auto peak_linear = measure_true_peak(path);
    if (!peak_linear) {
        return tl::unexpected{peak_linear.error()};
    }

    const double peak_dbtp = 20.0 * std::log10(std::max(1.0e-10, *peak_linear));
    logs.log(LogLevel::info,
             "peak-limit",
             fmt::format("True Peak: {:.2f} dBTP, target: {:.1f} dBTP", peak_dbtp, static_cast<double>(target_dbtp)));

    const double target_linear = std::pow(10.0, static_cast<double>(target_dbtp) / 20.0);
    if (*peak_linear <= target_linear * 1.001) {
        logs.log(LogLevel::info, "peak-limit", "True Peak within target — no gain applied");
        return {};
    }

    const auto gain = static_cast<float>(target_linear / *peak_linear);
    logs.log(
        LogLevel::info, "peak-limit", fmt::format("applying gain {:.4f} ({:.2f} dB)", gain, 20.0F * std::log10(gain)));

    return apply_gain(path, gain);
}

} // namespace mradm
