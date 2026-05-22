#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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
// layout_id must be one of the supported output layout identifiers:
//   "0+2+0", "0+5+0", "wav71", "4+5+0", "4+7+0", "9.1.6", "9+10+3"
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

// Streaming reader for CAF files written by FloatCafWriter.
// Validates format (float32 LE lpcm) and scans chunk headers to locate the
// data payload; robust to any CAF chunk ordering including a trailing info chunk.
class FloatCafReader {
  public:
    static Result<FloatCafReader> open(const std::string& path);
    ~FloatCafReader();
    FloatCafReader(FloatCafReader&&) noexcept;
    FloatCafReader& operator=(FloatCafReader&&) noexcept;
    FloatCafReader(const FloatCafReader&) = delete;
    FloatCafReader& operator=(const FloatCafReader&) = delete;

    uint32_t channels() const;
    uint32_t sample_rate() const;
    uint64_t frame_count() const;
    uint64_t read(float* out, uint64_t frames);

  private:
    FloatCafReader() = default;
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

// Type-erased audio file reader. Selects WAV or CAF from the path extension.
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
    uint64_t read(float* out, uint64_t frames);

  private:
    explicit ReaderHandle(FloatWavReader r) : impl_(std::move(r)) {}
    explicit ReaderHandle(FloatCafReader r) : impl_(std::move(r)) {}
    std::variant<FloatWavReader, FloatCafReader> impl_;
};

// Apply a linear gain to all samples in path, rewriting the file in-place via a
// temp file + rename. Supports WAV and CAF formats. layout_id is required when
// the path is a CAF file; ignored for WAV. No-op when |gain - 1| < 1e-6.
Result<void> apply_gain_to_file(const std::string& path, float gain, const std::string& layout_id = {});

// Convert an existing float32 WAV to integer PCM in-place (temp + rename).
// bit_depth must be 16 or 24. Limited to sample rates <= 65535 Hz by the
// underlying bw64 integer writer (libbw64 0.10.0 API constraint).
Result<void> downconvert_to_int(const std::string& path, uint16_t bit_depth);

// Format-agnostic render output metadata.  Assembled by the engine layer and
// passed to write_file_metadata(); format-specific encoding is handled there.
struct MetadataFields {
    std::string encoder;       // "MacinRender ADM Core Alpha"
    std::string date_utc;      // ISO 8601, e.g. "2026-05-22T14:30:00Z"
    std::string renderer;      // caps.backend_name — "ear" / "vbap-saf" / "hoa-encode"
    std::string output_layout; // "0+2+0", "2+7+4", etc.
    std::optional<double> lufs;
    std::optional<double> peak_dbtp;
};

// Write rendering metadata into an existing output file.
//   WAV  — appends a BWF v2 bext chunk (EBU Tech 3285) and updates the RIFF
//           size.  Fields: Originator, OriginationDate/Time, LoudnessValue,
//           MaxTruePeakLevel (0x7FFF = not-indicated when value is absent).
//   CAF  — appends an info chunk with encodingapplication / date / comments
//           keys.  Comments encodes renderer, layout, loudness, peak.
//   Other extensions — silently ignored (not an error).
// Failure is non-fatal; callers should log a warning and continue.
Result<void> write_file_metadata(const std::string& path, const MetadataFields& meta);

} // namespace mradm::audio
