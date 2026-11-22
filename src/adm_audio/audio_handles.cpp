#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "adm/audio_io.h"

#include "../adm_core/rust_services_ffi.h"

namespace mradm::audio {
namespace {

Error wav_error(int32_t code, const std::array<uint8_t, 512>& message, const std::string& path) {
    const auto error = code >= 1 && code <= 6 ? static_cast<ErrorCode>(code) : ErrorCode::internal_error;
    return {error, reinterpret_cast<const char*>(message.data()), "path=" + path};
}

std::string wav_path_utf8(const std::string& path) {
#ifdef _WIN32
    // The incoming path uses the active Windows code page, even when its bytes happen to be valid UTF-8.
    const auto utf8 = std::filesystem::path(path).u8string();
    return {utf8.begin(), utf8.end()};
#else
    return path;
#endif
}

} // namespace

RenderInputReader::RenderInputReader(std::string path)
    : path_(std::move(path)), handle_(nullptr, rust().wav_reader_destroy) {}

Result<std::unique_ptr<RenderInputReader>> RenderInputReader::open(const std::string& path) {
    try {
        auto reader = std::unique_ptr<RenderInputReader>{new RenderInputReader{path}};
        const auto utf8_path = wav_path_utf8(path);
        std::array<uint8_t, 512> message{};
        MradmWavReader* handle = nullptr;
        const auto code = rust().wav_reader_open(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                                 utf8_path.size(),
                                                 &handle,
                                                 &reader->info_,
                                                 message.data(),
                                                 message.size());
        if (code != 0) {
            return mradm::unexpected{wav_error(code, message, path)};
        }
        reader->handle_.reset(handle);
        return reader;
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string{"failed to open render input: "} + e.what(), "path=" + path);
    } catch (...) {
        return make_error(ErrorCode::internal_error, "failed to open render input", "path=" + path);
    }
}

Result<uint64_t> RenderInputReader::read(float* out, uint64_t frames) {
    if (frames > std::numeric_limits<std::size_t>::max() / info_.channels) {
        return make_error(ErrorCode::invalid_argument, "WAVE read buffer size overflow", "path=" + path_);
    }
    std::array<uint8_t, 512> message{};
    uint64_t count = 0;
    const auto code = rust().wav_reader_read(
        handle_.get(), out, static_cast<std::size_t>(frames) * info_.channels, &count, message.data(), message.size());
    if (code != 0) {
        return mradm::unexpected{wav_error(code, message, path_)};
    }
    return count;
}

Result<void> RenderInputReader::seek_frame(uint64_t frame) {
    std::array<uint8_t, 512> message{};
    const auto code = rust().wav_reader_seek(handle_.get(), frame, message.data(), message.size());
    if (code != 0) {
        return mradm::unexpected{wav_error(code, message, path_)};
    }
    return {};
}

WriterHandle::WriterHandle(std::string path, uint32_t channels)
    : path_(std::move(path)), channels_(channels), handle_(nullptr, rust().output_writer_destroy) {}

Result<WriterHandle>
WriterHandle::open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c); });
    const bool caf = ext == ".caf";
    const std::string_view layout = caf ? std::string_view{layout_id} : std::string_view{};
    const auto utf8_path = wav_path_utf8(path);
    WriterHandle writer{path, channels};
    MradmOutputWriter* handle = nullptr;
    std::array<uint8_t, 512> message{};
    const auto code = rust().output_writer_create(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                                  utf8_path.size(),
                                                  channels,
                                                  sample_rate,
                                                  caf ? 1U : 0U,
                                                  reinterpret_cast<const uint8_t*>(layout.data()),
                                                  layout.size(),
                                                  &handle,
                                                  message.data(),
                                                  message.size());
    if (code != 0) {
        return mradm::unexpected{wav_error(code, message, path)};
    }
    writer.handle_.reset(handle);
    return writer;
}

uint64_t WriterHandle::write(const float* samples, uint64_t frame_count) {
    if (frame_count > std::numeric_limits<std::size_t>::max() / channels_) {
        return 0;
    }
    const auto code = rust().output_writer_write(
        handle_.get(), samples, static_cast<std::size_t>(frame_count) * channels_, nullptr, 0);
    return code == 0 ? frame_count : 0;
}

Result<void> WriterHandle::finish() {
    std::array<uint8_t, 512> message{};
    const auto code = rust().output_writer_finish(handle_.get(), message.data(), message.size());
    if (code != 0) {
        return mradm::unexpected{wav_error(code, message, path_)};
    }
    return {};
}

} // namespace mradm::audio
