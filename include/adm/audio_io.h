#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "adm/errors.h"

namespace mradm::audio {

// Streaming writer for WAVE_FORMAT_IEEE_FLOAT (32-bit float) WAV files.
// Values outside [-1, 1] are preserved — no clipping. Suitable for mastering
// pipelines where rendering may produce headroom > 0 dBFS.
class FloatWavWriter {
  public:
    static Result<FloatWavWriter> open(const std::string& path, uint32_t channels, uint32_t sample_rate);
    ~FloatWavWriter();
    FloatWavWriter(FloatWavWriter&&) noexcept;
    FloatWavWriter& operator=(FloatWavWriter&&) noexcept;
    FloatWavWriter(const FloatWavWriter&) = delete;
    FloatWavWriter& operator=(const FloatWavWriter&) = delete;

    uint64_t write(const float* samples, uint64_t frame_count);

  private:
    FloatWavWriter() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Streaming reader for WAV files; decodes any PCM or float format to float32.
class FloatWavReader {
  public:
    static Result<FloatWavReader> open(const std::string& path);
    ~FloatWavReader();
    FloatWavReader(FloatWavReader&&) noexcept;
    FloatWavReader& operator=(FloatWavReader&&) noexcept;
    FloatWavReader(const FloatWavReader&) = delete;
    FloatWavReader& operator=(const FloatWavReader&) = delete;

    uint32_t channels() const;
    uint32_t sample_rate() const;
    uint64_t frame_count() const;
    uint64_t read(float* out, uint64_t frames);

  private:
    FloatWavReader() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Streaming writer for CAF (Core Audio Format) files with IEEE float32 samples
// and an Apple AudioChannelLayoutTag encoded in the 'chan' chunk.
// layout_id must be one of the supported BS.2051 identifiers:
//   "0+2+0", "0+5+0", "0+7+0", "4+5+0", "4+7+0", "9.1.6", "9+10+3"
// PCM samples are stored as little-endian float32 with the matching layout tag so
// that afinfo, QuickTime, and CoreAudio consumers can read the channel assignment.
class FloatCafWriter {
  public:
    static Result<FloatCafWriter>
    open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id);
    ~FloatCafWriter();
    FloatCafWriter(FloatCafWriter&&) noexcept;
    FloatCafWriter& operator=(FloatCafWriter&&) noexcept;
    FloatCafWriter(const FloatCafWriter&) = delete;
    FloatCafWriter& operator=(const FloatCafWriter&) = delete;

    uint64_t write(const float* samples, uint64_t frame_count);

  private:
    FloatCafWriter() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Type-erased audio file writer.  Selects WAV or CAF automatically from the
// output path extension (.wav → FloatWavWriter, .caf → FloatCafWriter).
// For CAF, layout_id must be a known BS.2051 layout identifier.
class WriterHandle {
  public:
    static Result<WriterHandle>
    open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id = {});

    WriterHandle(WriterHandle&&) noexcept = default;
    WriterHandle& operator=(WriterHandle&&) noexcept = default;
    WriterHandle(const WriterHandle&) = delete;
    WriterHandle& operator=(const WriterHandle&) = delete;

    uint64_t write(const float* samples, uint64_t frame_count);

  private:
    explicit WriterHandle(FloatWavWriter w) : impl_(std::move(w)) {}
    explicit WriterHandle(FloatCafWriter w) : impl_(std::move(w)) {}
    std::variant<FloatWavWriter, FloatCafWriter> impl_;
};

// Convert an existing float32 WAV to integer PCM in-place (temp + rename).
// bit_depth must be 16 or 24. Limited to sample rates <= 65535 Hz by the
// underlying bw64 integer writer (libbw64 0.10.0 API constraint).
Result<void> downconvert_to_int(const std::string& path, uint16_t bit_depth);

} // namespace mradm::audio
