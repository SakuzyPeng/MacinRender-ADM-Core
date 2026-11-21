#pragma once

#include <cstdint>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "adm/errors.h"

namespace mradm::audio {

// Streaming writer for WAVE_FORMAT_IEEE_FLOAT (32-bit float) RF64 files; an existing file is replaced.
// Values outside [-1, 1] are preserved — no clipping. Suitable for mastering
// pipelines where rendering may produce headroom > 0 dBFS.
// finish() writes the final sizes and reports failure; the destructor finishes on a best-effort basis.
class FloatWavWriter {
  public:
    static Result<FloatWavWriter> open(const std::string& path, uint32_t channels, uint32_t sample_rate);
    ~FloatWavWriter();
    FloatWavWriter(FloatWavWriter&&) noexcept;
    FloatWavWriter& operator=(FloatWavWriter&&) noexcept;
    FloatWavWriter(const FloatWavWriter&) = delete;
    FloatWavWriter& operator=(const FloatWavWriter&) = delete;

    // Returns frame_count on success, 0 on failure.
    uint64_t write(const float* samples, uint64_t frame_count);
    // Repeated calls return the first finish result, including any failure.
    Result<void> finish();

  private:
    FloatWavWriter() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Streaming reader for RIFF/RF64/BW64 WAVE files with PCM16/24/32 or float32 samples, decoded to float32.
// read() returns 0 on I/O failure.
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
    uint16_t bits_per_sample() const;
    bool is_linear_pcm() const;
    bool is_ieee_float() const;
    uint64_t read(float* out, uint64_t frames);

  private:
    FloatWavReader() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Render-time input reader over the Rust WAVE reader; read/seek failures are returned as errors.
class RenderInputReader {
  public:
    static Result<std::unique_ptr<RenderInputReader>> open(const std::string& path);
    ~RenderInputReader();
    RenderInputReader(RenderInputReader&&) noexcept;
    RenderInputReader& operator=(RenderInputReader&&) noexcept;
    RenderInputReader(const RenderInputReader&) = delete;
    RenderInputReader& operator=(const RenderInputReader&) = delete;

    Result<uint64_t> read(float* out, uint64_t frames);
    Result<void> seek_frame(uint64_t frame);

  private:
    RenderInputReader();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Streaming writer for CAF (Core Audio Format) files with IEEE float32 samples
// and an Apple AudioChannelLayoutTag encoded in the 'chan' chunk.
// layout_id must be one of the supported output layout identifiers:
//   "0+2+0", "binaural", "0+5+0", "wav71", "2+5+0", "5.1.4", "7.1.4", "9.1.6", "22.2"
// PCM samples are stored as little-endian float32 with the matching layout tag so
// that afinfo, QuickTime, and CoreAudio consumers can read the channel assignment.
// finish() writes the data size and reports failure; the destructor finishes on a best-effort basis.
class FloatCafWriter {
  public:
    static Result<FloatCafWriter>
    open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id);
    ~FloatCafWriter();
    FloatCafWriter(FloatCafWriter&&) noexcept;
    FloatCafWriter& operator=(FloatCafWriter&&) noexcept;
    FloatCafWriter(const FloatCafWriter&) = delete;
    FloatCafWriter& operator=(const FloatCafWriter&) = delete;

    // Returns frame_count on success, 0 on failure.
    uint64_t write(const float* samples, uint64_t frame_count);
    // Repeated calls return the first finish result, including any failure.
    Result<void> finish();

  private:
    FloatCafWriter() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Type-erased audio file writer: CAF for a .caf path (FloatCafWriter), WAV otherwise
// (FloatWavWriter). For CAF, layout_id must be a known output layout identifier; it is
// ignored for WAV.
class WriterHandle {
  public:
    static Result<WriterHandle>
    open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id = {});

    WriterHandle(WriterHandle&&) noexcept = default;
    WriterHandle& operator=(WriterHandle&&) noexcept = default;
    WriterHandle(const WriterHandle&) = delete;
    WriterHandle& operator=(const WriterHandle&) = delete;

    uint64_t write(const float* samples, uint64_t frame_count);
    // Finalizes the output and reports failure.
    Result<void> finish();

  private:
    explicit WriterHandle(FloatWavWriter w) : impl_(std::move(w)) {}
    explicit WriterHandle(FloatCafWriter w) : impl_(std::move(w)) {}
    std::variant<FloatWavWriter, FloatCafWriter> impl_;
};

} // namespace mradm::audio
