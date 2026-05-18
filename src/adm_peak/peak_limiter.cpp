#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ebur128.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/peak.h"

namespace mradm {

namespace {

struct EburFree {
    void operator()(ebur128_state* st) const noexcept { ebur128_destroy(&st); }
};
using EburStatePtr = std::unique_ptr<ebur128_state, EburFree>;

// Pass 1: feed all samples from path into a libebur128 state for True Peak.
// Returns the maximum True Peak in linear amplitude across all channels.
[[nodiscard]] std::optional<double> measure_true_peak(const std::string& path) {
    auto reader_res = audio::FloatWavReader::open(path);
    if (!reader_res) {
        return std::nullopt;
    }
    auto& reader = *reader_res;
    const auto num_ch = reader.channels();
    const auto sample_rate = reader.sample_rate();

    EburStatePtr st{ebur128_init(num_ch, sample_rate, EBUR128_MODE_TRUE_PEAK)};
    if (!st) {
        return std::nullopt;
    }

    constexpr std::size_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(num_ch) * k_block);
    uint64_t frames_left = reader.frame_count();

    while (frames_left > 0) {
        const uint64_t n = std::min(static_cast<uint64_t>(k_block), frames_left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        ebur128_add_frames_float(st.get(), buf.data(), static_cast<std::size_t>(got));
        frames_left -= got;
    }

    double max_peak = 0.0;
    for (unsigned int ch = 0; ch < num_ch; ++ch) {
        double ch_peak = 0.0;
        if (ebur128_true_peak(st.get(), ch, &ch_peak) == EBUR128_SUCCESS) {
            max_peak = std::max(max_peak, ch_peak);
        }
    }
    return max_peak;
}

// Pass 2: rewrite path with all samples scaled by gain.
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

        const auto tmp_path = path + ".peak_tmp";
        {
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
    if (!peak_linear.has_value()) {
        return make_error(ErrorCode::io_error, "True Peak measurement failed", "path=" + path);
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
