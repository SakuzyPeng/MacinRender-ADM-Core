#include "audio_reader.h"

#include <algorithm>
#include <filesystem>

namespace mradm::audio {
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
Result<uint64_t> ReaderHandle::read(float* out, uint64_t frames) {
    return std::visit([&](auto& r) -> Result<uint64_t> { return r.read(out, frames); }, impl_);
}

} // namespace mradm::audio
