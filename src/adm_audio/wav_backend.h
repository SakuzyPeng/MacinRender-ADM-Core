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
#include <vector>

#include "adm/audio_io.h"
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
    [[nodiscard]] const MradmWavInfo& info() const noexcept { return info_; }

  private:
    explicit RustWavReader(std::string path) : path_(std::move(path)) {}
    std::string path_;
    MradmWavInfo info_{};
    std::unique_ptr<MradmWavReader, decltype(&mradm_wav_reader_destroy)> handle_{nullptr, mradm_wav_reader_destroy};
};

// Metadata reader: validates the container without requiring a supported audio encoding.
class RustWavChunks {
  public:
    static Result<std::unique_ptr<RustWavChunks>> open(const std::string& path) {
        auto reader = std::unique_ptr<RustWavChunks>{new RustWavChunks{path}};
        const auto utf8_path = wav_path_utf8(path);
        std::array<uint8_t, 512> message{};
        MradmWavChunks* handle = nullptr;
        const auto code = mradm_wav_chunks_open(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                                utf8_path.size(),
                                                &handle,
                                                message.data(),
                                                message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path)};
        }
        reader->handle_.reset(handle);
        return reader;
    }
    // Payload of the first chunk with this four-character id.
    Result<std::optional<std::string>> chunk(std::string_view id) {
        if (id.size() != 4U) {
            return make_error(ErrorCode::invalid_argument, "WAVE chunk id must have four characters", "path=" + path_);
        }
        const auto* raw_id = reinterpret_cast<const uint8_t*>(id.data());
        std::array<uint8_t, 512> message{};
        uint64_t size = 0;
        uint8_t found = 0;
        auto code =
            mradm_wav_chunks_read(handle_.get(), raw_id, nullptr, 0, &size, &found, message.data(), message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path_)};
        }
        if (found == 0U) {
            return std::optional<std::string>{};
        }
        std::string payload;
        if (size > payload.max_size()) {
            return make_error(ErrorCode::unsupported, "WAVE chunk is too large", "path=" + path_);
        }
        payload.resize(static_cast<std::size_t>(size));
        if (size != 0U) {
            code = mradm_wav_chunks_read(handle_.get(),
                                         raw_id,
                                         reinterpret_cast<uint8_t*>(payload.data()),
                                         payload.size(),
                                         &size,
                                         &found,
                                         message.data(),
                                         message.size());
            if (code != 0) {
                return tl::unexpected{wav_error(code, message, path_)};
            }
        }
        return std::optional<std::string>{std::move(payload)};
    }
    // Import-side CHNA records; empty when the file has no chna chunk.
    Result<std::vector<WavChnaUid>> chna() {
        std::array<uint8_t, 512> message{};
        std::size_t count = 0;
        uint8_t found = 0;
        auto code =
            mradm_wav_chunks_chna(handle_.get(), nullptr, nullptr, 0, &count, &found, message.data(), message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path_)};
        }
        std::vector<WavChnaUid> records;
        if (found == 0U || count == 0U) {
            return records;
        }
        std::vector<uint16_t> indices(count);
        std::vector<uint8_t> uids(count * 12U);
        code = mradm_wav_chunks_chna(
            handle_.get(), indices.data(), uids.data(), count, &count, &found, message.data(), message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path_)};
        }
        records.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto* uid = reinterpret_cast<const char*>(uids.data() + (index * 12U));
            records.push_back({indices[index], std::string{uid, 12U}});
        }
        return records;
    }

  private:
    explicit RustWavChunks(std::string path) : path_(std::move(path)) {}
    std::string path_;
    std::unique_ptr<MradmWavChunks, decltype(&mradm_wav_chunks_destroy)> handle_{nullptr, mradm_wav_chunks_destroy};
};

// Streaming WAVE writer. Integer PCM starts as RIFF and promotes to BW64; float32 is always RF64.
// Exclusive writers refuse to replace an existing file; others truncate it like fopen("wb").
// The file is only valid after finish() succeeds; destroying an unfinished writer leaves it incomplete.
class RustWavWriter {
  public:
    static Result<std::unique_ptr<RustWavWriter>> create(
        const std::string& path, uint32_t channels, uint32_t rate, uint16_t bits, bool float_output, bool exclusive) {
        auto writer = std::unique_ptr<RustWavWriter>{new RustWavWriter{path, channels}};
        const auto utf8_path = wav_path_utf8(path);
        std::array<uint8_t, 512> message{};
        MradmWavWriter* handle = nullptr;
        const auto code = mradm_wav_writer_create(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                                  utf8_path.size(),
                                                  channels,
                                                  rate,
                                                  bits,
                                                  float_output ? 1U : 0U,
                                                  exclusive ? 1U : 0U,
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
    RustWavWriter(std::string path, uint32_t channels) : path_(std::move(path)), channels_(channels) {}
    std::string path_;
    uint32_t channels_;
    std::unique_ptr<MradmWavWriter, decltype(&mradm_wav_writer_destroy)> handle_{nullptr, mradm_wav_writer_destroy};
};

// Integer PCM conversion output: exclusive create, installed by the caller only after finish().
struct IntegerWavWriter {
    static Result<std::unique_ptr<RustWavWriter>>
    create(const std::string& path, uint32_t channels, uint32_t rate, uint16_t bits) {
        return RustWavWriter::create(path, channels, rate, bits, false, true);
    }
};
} // namespace mradm::audio
