#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "../adm_core/rust_services_ffi.h"

namespace mradm::audio {
inline Error wav_error(int32_t code, const std::array<uint8_t, 512>& message, const std::string& path) {
    const auto error = code >= 1 && code <= 6 ? static_cast<ErrorCode>(code) : ErrorCode::internal_error;
    return {error, reinterpret_cast<const char*>(message.data()), "path=" + path};
}

inline std::string wav_path_utf8(const std::string& path) {
#ifdef _WIN32
    // Match the active code page used by the surrounding C++ WAVE I/O. A UTF-8
    // application manifest makes this conversion an identity operation. Do not
    // guess from the bytes: a native path can also happen to be valid UTF-8.
    const auto utf8 = std::filesystem::path(path).u8string();
    return {utf8.begin(), utf8.end()};
#else
    return path;
#endif
}

class WavFileReader {
  public:
    static Result<std::unique_ptr<WavFileReader>> open(const std::string& path) {
        auto reader = std::unique_ptr<WavFileReader>{new WavFileReader{path}};
        const auto utf8_path = wav_path_utf8(path);
        std::array<uint8_t, 512> message{};
        MradmWavReader* handle = nullptr;
        const auto code = mradm::rust().wav_reader_open(reinterpret_cast<const uint8_t*>(utf8_path.data()),
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
    }
    Result<uint64_t> read(float* output, uint64_t frames) {
        if (frames > std::numeric_limits<std::size_t>::max() / info_.channels) {
            return make_error(ErrorCode::invalid_argument, "WAVE read buffer size overflow", "path=" + path_);
        }
        std::array<uint8_t, 512> message{};
        uint64_t count = 0;
        const auto code = mradm::rust().wav_reader_read(handle_.get(),
                                                        output,
                                                        static_cast<std::size_t>(frames) * info_.channels,
                                                        &count,
                                                        message.data(),
                                                        message.size());
        if (code != 0) {
            return mradm::unexpected{wav_error(code, message, path_)};
        }
        return count;
    }
    Result<void> seek_frame(uint64_t frame) {
        std::array<uint8_t, 512> message{};
        const auto code = mradm::rust().wav_reader_seek(handle_.get(), frame, message.data(), message.size());
        if (code != 0) {
            return mradm::unexpected{wav_error(code, message, path_)};
        }
        return {};
    }
    [[nodiscard]] const MradmWavInfo& info() const noexcept { return info_; }

  private:
    explicit WavFileReader(std::string path) : path_(std::move(path)) {}
    std::string path_;
    MradmWavInfo info_{};
    std::unique_ptr<MradmWavReader, decltype(MradmRustServices::wav_reader_destroy)> handle_{
        nullptr, mradm::rust().wav_reader_destroy};
};

// Final float PCM output, RF64 WAVE or CAF (layout non-empty); numbers its writer/pass* checkpoints.
class PcmOutputWriter {
  public:
    static Result<std::unique_ptr<PcmOutputWriter>>
    create(const std::string& path, uint32_t channels, uint32_t rate, std::optional<std::string_view> caf_layout) {
        auto writer = std::unique_ptr<PcmOutputWriter>{new PcmOutputWriter{path, channels}};
        const auto utf8_path = wav_path_utf8(path);
        const auto layout = caf_layout.value_or(std::string_view{});
        std::array<uint8_t, 512> message{};
        MradmOutputWriter* handle = nullptr;
        const auto code = mradm::rust().output_writer_create(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                                             utf8_path.size(),
                                                             channels,
                                                             rate,
                                                             caf_layout.has_value() ? 1U : 0U,
                                                             reinterpret_cast<const uint8_t*>(layout.data()),
                                                             layout.size(),
                                                             &handle,
                                                             message.data(),
                                                             message.size());
        if (code != 0) {
            return mradm::unexpected{wav_error(code, message, path)};
        }
        writer->handle_.reset(handle);
        return writer;
    }
    // Whole frames, all or nothing.
    [[nodiscard]] bool write(const float* samples, uint64_t frames) {
        return frames <= std::numeric_limits<std::size_t>::max() / channels_ &&
               mradm::rust().output_writer_write(
                   handle_.get(), samples, static_cast<std::size_t>(frames) * channels_, nullptr, 0) == 0;
    }
    Result<void> finish() {
        std::array<uint8_t, 512> message{};
        const auto code = mradm::rust().output_writer_finish(handle_.get(), message.data(), message.size());
        if (code != 0) {
            return mradm::unexpected{wav_error(code, message, path_)};
        }
        return {};
    }

  private:
    PcmOutputWriter(std::string path, uint32_t channels) : path_(std::move(path)), channels_(channels) {}
    std::string path_;
    uint32_t channels_;
    std::unique_ptr<MradmOutputWriter, decltype(MradmRustServices::output_writer_destroy)> handle_{
        nullptr, mradm::rust().output_writer_destroy};
};

} // namespace mradm::audio
