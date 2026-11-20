#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "wav_backend.h"

namespace mradm::audio {

// ── RenderInputReader ────────────────────────────────────────────────────────

struct RenderInputReader::Impl {
    std::unique_ptr<WavFileReader> reader;
};

RenderInputReader::RenderInputReader() = default;
RenderInputReader::~RenderInputReader() = default;
RenderInputReader::RenderInputReader(RenderInputReader&&) noexcept = default;
RenderInputReader& RenderInputReader::operator=(RenderInputReader&&) noexcept = default;

Result<std::unique_ptr<RenderInputReader>> RenderInputReader::open(const std::string& path) {
    try {
        auto reader = WavFileReader::open(path);
        if (!reader) {
            return mradm::unexpected{reader.error()};
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

// ── WriterHandle ──────────────────────────────────────────────────────────────

Result<WriterHandle>
WriterHandle::open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c); });

    if (ext == ".caf") {
        auto res = FloatCafWriter::open(path, channels, sample_rate, layout_id);
        if (!res) {
            return mradm::unexpected{res.error()};
        }
        return WriterHandle{std::move(*res)};
    }
    auto res = FloatWavWriter::open(path, channels, sample_rate);
    if (!res) {
        return mradm::unexpected{res.error()};
    }
    return WriterHandle{std::move(*res)};
}

uint64_t WriterHandle::write(const float* samples, uint64_t frame_count) {
    return std::visit([&](auto& w) { return w.write(samples, frame_count); }, impl_);
}

Result<void> WriterHandle::finish() {
    return std::visit([](auto& w) { return w.finish(); }, impl_);
}

} // namespace mradm::audio
