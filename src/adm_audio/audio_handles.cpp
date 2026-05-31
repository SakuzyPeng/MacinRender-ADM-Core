#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/errors.h"

namespace mradm::audio {

namespace {

[[nodiscard]] std::filesystem::path unique_sidecar_path(const std::filesystem::path& original_path,
                                                        std::string_view purpose) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;

    const auto parent = original_path.parent_path();
    const auto stem = original_path.stem().string();
    const auto ext = original_path.extension().string();

    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto token = dist(rng);
        auto candidate = parent / fmt::format("{}.{}.{:016x}{}", stem, purpose, token, ext);
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return parent / fmt::format("{}.{}.{:016x}{}", stem, purpose, dist(rng), ext);
}

} // namespace

// ── ReaderHandle ──────────────────────────────────────────────────────────────

Result<ReaderHandle> ReaderHandle::open(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    if (ext == ".caf") {
        auto res = FloatCafReader::open(path);
        if (!res) {
            return tl::unexpected{res.error()};
        }
        return ReaderHandle{std::move(*res)};
    }
    if (ext == ".flac") {
        auto res = FloatFlacReader::open(path);
        if (!res) {
            return tl::unexpected{res.error()};
        }
        return ReaderHandle{std::move(*res)};
    }
    auto res = FloatWavReader::open(path);
    if (!res) {
        return tl::unexpected{res.error()};
    }
    return ReaderHandle{std::move(*res)};
}

uint32_t ReaderHandle::channels() const {
    return std::visit([](const auto& r) { return r.channels(); }, impl_);
}
uint32_t ReaderHandle::sample_rate() const {
    return std::visit([](const auto& r) { return r.sample_rate(); }, impl_);
}
uint64_t ReaderHandle::frame_count() const {
    return std::visit([](const auto& r) { return r.frame_count(); }, impl_);
}
uint64_t ReaderHandle::read(float* out, uint64_t frames) {
    return std::visit([&](auto& r) { return r.read(out, frames); }, impl_);
}

// ── apply_gain_to_file ────────────────────────────────────────────────────────

Result<void> apply_gain_to_file(const std::string& path, float gain, const std::string& layout_id) {
    if (std::abs(gain - 1.0F) < 1e-6F) {
        return {};
    }

    const std::filesystem::path original_path{path};
    const auto tmp_path = unique_sidecar_path(original_path, "gain_tmp");

    {
        auto reader_res = ReaderHandle::open(path);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        auto& reader = *reader_res;

        const uint32_t num_ch = reader.channels();
        const uint32_t sr = reader.sample_rate();
        const uint64_t total_frames = reader.frame_count();

        auto writer_res = WriterHandle::open(tmp_path.string(), num_ch, sr, layout_id);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        constexpr uint64_t k_block = 4096;
        std::vector<float> buf(static_cast<std::size_t>(num_ch) * k_block);
        uint64_t left = total_frames;

        while (left > 0) {
            const uint64_t n = std::min(k_block, left);
            const uint64_t got = reader.read(buf.data(), n);
            if (got == 0) {
                break;
            }
            const std::size_t samples = static_cast<std::size_t>(num_ch) * static_cast<std::size_t>(got);
            for (std::size_t i = 0; i < samples; ++i) {
                buf[i] *= gain;
            }
            if (writer.write(buf.data(), got) != got) {
                return make_error(
                    ErrorCode::io_error, "short write in apply_gain_to_file", "path=" + tmp_path.string());
            }
            left -= got;
        }
        if (left != 0) {
            return make_error(ErrorCode::io_error, "short read in apply_gain_to_file", "path=" + path);
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, original_path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(original_path, remove_ec);
        ec.clear();
        std::filesystem::rename(tmp_path, original_path, ec);
        if (ec) {
            return make_error(ErrorCode::io_error,
                              "failed to replace output after apply_gain_to_file: " + ec.message(),
                              "path=" + path);
        }
    }
    return {};
}

// ── trim_file_frames ──────────────────────────────────────────────────────────

Result<void>
trim_file_frames(const std::string& path, uint64_t start_frame, uint64_t frame_count, const std::string& layout_id) {
    const std::filesystem::path original_path{path};
    const auto tmp_path = unique_sidecar_path(original_path, "trim_tmp");

    {
        auto reader_res = ReaderHandle::open(path);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        auto& reader = *reader_res;

        const uint32_t num_ch = reader.channels();
        const uint32_t sr = reader.sample_rate();
        const uint64_t total_frames = reader.frame_count();

        // Clamp the range to the file: a start past the end yields no frames, and
        // frame_count is capped at what remains after start_frame.
        const uint64_t clamped_start = std::min(start_frame, total_frames);
        const uint64_t avail = total_frames - clamped_start;
        const uint64_t out_frames = std::min(frame_count, avail);
        if (out_frames == 0) {
            return make_error(ErrorCode::invalid_argument,
                              "trim range selects no audio frames",
                              fmt::format("path={} start_frame={} frame_count={} total_frames={}",
                                          path,
                                          start_frame,
                                          frame_count,
                                          total_frames));
        }
        // Whole-file range: nothing to do, leave the file untouched.
        if (clamped_start == 0 && out_frames == total_frames) {
            return {};
        }

        auto writer_res = WriterHandle::open(tmp_path.string(), num_ch, sr, layout_id);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        constexpr uint64_t k_block = 4096;
        std::vector<float> buf(static_cast<std::size_t>(num_ch) * k_block);

        // The readers are forward-only (no seek), so discard the head by reading it.
        uint64_t to_skip = clamped_start;
        while (to_skip > 0) {
            const uint64_t n = std::min(k_block, to_skip);
            const uint64_t got = reader.read(buf.data(), n);
            if (got == 0) {
                return make_error(ErrorCode::io_error, "short read while skipping to trim start", "path=" + path);
            }
            to_skip -= got;
        }

        uint64_t left = out_frames;
        while (left > 0) {
            const uint64_t n = std::min(k_block, left);
            const uint64_t got = reader.read(buf.data(), n);
            if (got == 0) {
                return make_error(ErrorCode::io_error, "short read in trim_file_frames", "path=" + path);
            }
            if (writer.write(buf.data(), got) != got) {
                return make_error(ErrorCode::io_error, "short write in trim_file_frames", "path=" + tmp_path.string());
            }
            left -= got;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, original_path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(original_path, remove_ec);
        ec.clear();
        std::filesystem::rename(tmp_path, original_path, ec);
        if (ec) {
            return make_error(ErrorCode::io_error,
                              "failed to replace output after trim_file_frames: " + ec.message(),
                              "path=" + path);
        }
    }
    return {};
}

// ── WriterHandle ──────────────────────────────────────────────────────────────

Result<WriterHandle>
WriterHandle::open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    if (ext == ".caf") {
        auto res = FloatCafWriter::open(path, channels, sample_rate, layout_id);
        if (!res) {
            return tl::unexpected{res.error()};
        }
        return WriterHandle{std::move(*res)};
    }
    if (ext == ".flac") {
        auto res = FloatFlacWriter::open(path, channels, sample_rate);
        if (!res) {
            return tl::unexpected{res.error()};
        }
        return WriterHandle{std::move(*res)};
    }
    auto res = FloatWavWriter::open(path, channels, sample_rate);
    if (!res) {
        return tl::unexpected{res.error()};
    }
    return WriterHandle{std::move(*res)};
}

uint64_t WriterHandle::write(const float* samples, uint64_t frame_count) {
    return std::visit([&](auto& w) { return w.write(samples, frame_count); }, impl_);
}

} // namespace mradm::audio
