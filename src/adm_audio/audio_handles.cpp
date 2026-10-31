#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "wav_backend.h"

namespace mradm::audio {

// ── RenderInputReader ────────────────────────────────────────────────────────

struct RenderInputReader::Impl {
    std::unique_ptr<RustWavReader> reader;
};

RenderInputReader::RenderInputReader() = default;
RenderInputReader::~RenderInputReader() = default;
RenderInputReader::RenderInputReader(RenderInputReader&&) noexcept = default;
RenderInputReader& RenderInputReader::operator=(RenderInputReader&&) noexcept = default;

Result<std::unique_ptr<RenderInputReader>> RenderInputReader::open(const std::string& path) {
    try {
        auto reader = RustWavReader::open(path);
        if (!reader) {
            return tl::unexpected{reader.error()};
        }
        auto out = std::unique_ptr<RenderInputReader>{new RenderInputReader{}};
        out->impl_ = std::make_unique<Impl>();
        out->impl_->reader = std::move(*reader);
        return out;
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string{"failed to open render input: "} + e.what(), "path=" + path);
    } catch (...) {
        return make_error(ErrorCode::internal_error, "failed to open render input", "path=" + path);
    }
}

Result<uint64_t> RenderInputReader::read(float* out, uint64_t frames) {
    return impl_->reader->read(out, frames);
}

Result<void> RenderInputReader::seek_frame(uint64_t frame) {
    // The Rust reader clamps targets past the end to the final frame boundary.
    return impl_->reader->seek_frame(frame);
}

// ── ReaderHandle ──────────────────────────────────────────────────────────────

Result<ReaderHandle> ReaderHandle::open(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c); });

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

// ── apply_gain_to_file / trim_file_frames ───────────────────────────────────
// Rust rewrites the file through the final-PCM writer and installs it (PCM consistency 4.8).

Result<void> apply_gain_to_file(const std::string& path,
                                float gain,
                                const std::string& layout_id,
                                const std::stop_token& cancel_token,
                                ProgressSink* progress,
                                RenderOperation operation) {
    const auto utf8_path = wav_path_utf8(path);
    return run_output_pass(path, cancel_token, progress, operation, [&](auto... out) {
        return mradm_output_gain_begin(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                       utf8_path.size(),
                                       gain,
                                       reinterpret_cast<const uint8_t*>(layout_id.data()),
                                       layout_id.size(),
                                       cancel_token.stop_requested() ? 1U : 0U,
                                       out...);
    });
}

Result<void> trim_file_frames(const std::string& path,
                              uint64_t start_frame,
                              uint64_t frame_count,
                              const std::string& layout_id,
                              const std::stop_token& cancel_token,
                              ProgressSink* progress,
                              RenderOperation operation) {
    if (cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
    }
    const auto utf8_path = wav_path_utf8(path);
    return run_output_pass(path, cancel_token, progress, operation, [&](auto... out) {
        return mradm_output_trim_begin(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                       utf8_path.size(),
                                       start_frame,
                                       frame_count,
                                       reinterpret_cast<const uint8_t*>(layout_id.data()),
                                       layout_id.size(),
                                       out...);
    });
}

// ── WriterHandle ──────────────────────────────────────────────────────────────

Result<WriterHandle>
WriterHandle::open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c); });

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

Result<void> WriterHandle::finish() {
    return std::visit(
        [](auto& w) -> Result<void> {
            if constexpr (requires { w.finish(); }) {
                return w.finish();
            } else {
                return {}; // The FLAC writer finalizes in its destructor.
            }
        },
        impl_);
}

} // namespace mradm::audio
