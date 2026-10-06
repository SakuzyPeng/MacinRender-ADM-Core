#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

#include "adm/errors.h"

#include "wav_ffi.h"

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

class RustWavReader {
  public:
    static Result<std::unique_ptr<RustWavReader>> open(const std::string& path) {
        auto reader = std::unique_ptr<RustWavReader>{new RustWavReader{path}};
        const auto utf8_path = wav_path_utf8(path);
        std::array<uint8_t, 512> message{};
        MradmWavReader* handle = nullptr;
        const auto code = mradm_wav_reader_open(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                                utf8_path.size(),
                                                &handle,
                                                &reader->info_,
                                                message.data(),
                                                message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path)};
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
        const auto code = mradm_wav_reader_read(handle_.get(),
                                                output,
                                                static_cast<std::size_t>(frames) * info_.channels,
                                                &count,
                                                message.data(),
                                                message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path_)};
        }
        return count;
    }
    Result<void> seek_frame(uint64_t frame) {
        std::array<uint8_t, 512> message{};
        const auto code = mradm_wav_reader_seek(handle_.get(), frame, message.data(), message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path_)};
        }
        return {};
    }

  private:
    explicit RustWavReader(std::string path) : path_(std::move(path)) {}
    std::string path_;
    MradmWavInfo info_{};
    std::unique_ptr<MradmWavReader, decltype(&mradm_wav_reader_destroy)> handle_{nullptr, mradm_wav_reader_destroy};
};

class IntegerWavWriter {
  public:
    static Result<std::unique_ptr<IntegerWavWriter>>
    create(const std::string& path, uint32_t channels, uint32_t rate, uint16_t bits) {
        auto writer = std::unique_ptr<IntegerWavWriter>{new IntegerWavWriter{path, channels}};
        const auto utf8_path = wav_path_utf8(path);
        std::array<uint8_t, 512> message{};
        MradmWavWriter* handle = nullptr;
        const auto code = mradm_wav_writer_create(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                                  utf8_path.size(),
                                                  channels,
                                                  rate,
                                                  bits,
                                                  &handle,
                                                  message.data(),
                                                  message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path)};
        }
        writer->handle_.reset(handle);
        return writer;
    }
    Result<void> write(const float* samples, uint64_t frames) {
        if (frames > std::numeric_limits<std::size_t>::max() / channels_) {
            return make_error(ErrorCode::invalid_argument, "WAVE write buffer size overflow", "path=" + path_);
        }
        std::array<uint8_t, 512> message{};
        const auto code = mradm_wav_writer_write(
            handle_.get(), samples, static_cast<std::size_t>(frames) * channels_, message.data(), message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path_)};
        }
        return {};
    }
    Result<void> finish() {
        std::array<uint8_t, 512> message{};
        const auto code = mradm_wav_writer_finish(handle_.get(), message.data(), message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path_)};
        }
        return {};
    }

  private:
    IntegerWavWriter(std::string path, uint32_t channels) : path_(std::move(path)), channels_(channels) {}
    std::string path_;
    uint32_t channels_;
    std::unique_ptr<MradmWavWriter, decltype(&mradm_wav_writer_destroy)> handle_{nullptr, mradm_wav_writer_destroy};
};
} // namespace mradm::audio
