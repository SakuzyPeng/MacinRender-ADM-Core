#pragma once

#include "adm/audio_io.h"

namespace mradm::audio {
// Type-erased audio file reader. Selects WAV, CAF, or FLAC from the path extension.
class ReaderHandle {
  public:
    static Result<ReaderHandle> open(const std::string& path);

    ReaderHandle(ReaderHandle&&) noexcept = default;
    ReaderHandle& operator=(ReaderHandle&&) noexcept = default;
    ReaderHandle(const ReaderHandle&) = delete;
    ReaderHandle& operator=(const ReaderHandle&) = delete;

    uint32_t channels() const;
    uint32_t sample_rate() const;
    uint64_t frame_count() const;
    Result<uint64_t> read(float* out, uint64_t frames);

  private:
    explicit ReaderHandle(FloatWavReader r) : impl_(std::move(r)) {}
    explicit ReaderHandle(FloatCafReader r) : impl_(std::move(r)) {}
    explicit ReaderHandle(FloatFlacReader r) : impl_(std::move(r)) {}
    std::variant<FloatWavReader, FloatCafReader, FloatFlacReader> impl_;
};

} // namespace mradm::audio
