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
//   "0+2+0", "binaural", "0+5+0", "wav71", "4+5+0", "4+7+0", "9.1.6", "9+10+3"
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

// Streaming writer for FLAC files. Accepts float32 input and encodes at 24-bit
// integer depth (FLAC lossless compression). Channel count must be 1-8 (FLAC
// format limit). WAVEFORMATEXTENSIBLE_CHANNEL_MASK is written later by
// write_file_metadata() into the Vorbis Comment block.
class FloatFlacWriter {
  public:
    static Result<FloatFlacWriter> open(const std::string& path, uint32_t channels, uint32_t sample_rate);
    ~FloatFlacWriter();
    FloatFlacWriter(FloatFlacWriter&&) noexcept;
    FloatFlacWriter& operator=(FloatFlacWriter&&) noexcept;
    FloatFlacWriter(const FloatFlacWriter&) = delete;
    FloatFlacWriter& operator=(const FloatFlacWriter&) = delete;

    uint64_t write(const float* samples, uint64_t frame_count);

  private:
    FloatFlacWriter() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Streaming reader for FLAC files. Decodes any integer FLAC to float32.
class FloatFlacReader {
  public:
    static Result<FloatFlacReader> open(const std::string& path);
    ~FloatFlacReader();
    FloatFlacReader(FloatFlacReader&&) noexcept;
    FloatFlacReader& operator=(FloatFlacReader&&) noexcept;
    FloatFlacReader(const FloatFlacReader&) = delete;
    FloatFlacReader& operator=(const FloatFlacReader&) = delete;

    uint32_t channels() const;
    uint32_t sample_rate() const;
    uint64_t frame_count() const;
    uint64_t read(float* out, uint64_t frames);

  private:
    FloatFlacReader() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Streaming writer for Matroska Audio (.mka) files with Opus-encoded audio.
// Accepts float32 input at 48000 Hz (the only rate Opus supports from external
// input without resampling). Uses VBR at 128 kbps for stereo or 64 kbps × ch
// for multichannel (transparent for Opus VBR). Channel mapping: family 0 for
// mono/stereo, family 1 for 3-8 ch surround, family 255 for 9-255 ch.
// Call convert_to_opus_mka() rather than this class directly in the pipeline.
class FloatOpusMkaWriter {
  public:
    // bitrate_per_ch_kbps: VBR target per channel in kbps; 0 = auto
    // (64 kbps/ch, minimum 128 kbps for stereo).
    static Result<FloatOpusMkaWriter>
    open(const std::string& path, uint32_t channels, uint32_t sample_rate, uint32_t bitrate_per_ch_kbps = 0);
    ~FloatOpusMkaWriter();
    FloatOpusMkaWriter(FloatOpusMkaWriter&&) noexcept;
    FloatOpusMkaWriter& operator=(FloatOpusMkaWriter&&) noexcept;
    FloatOpusMkaWriter(const FloatOpusMkaWriter&) = delete;
    FloatOpusMkaWriter& operator=(const FloatOpusMkaWriter&) = delete;

    uint64_t write(const float* samples, uint64_t frame_count);
    Result<void> close();

  private:
    FloatOpusMkaWriter() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Type-erased audio file writer.  Selects WAV, CAF, or FLAC automatically from
// the output path extension (.wav → FloatWavWriter, .caf → FloatCafWriter,
// .flac → FloatFlacWriter). For CAF, layout_id must be a known output layout
// identifier; it is ignored for WAV and FLAC.
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
    explicit WriterHandle(FloatFlacWriter w) : impl_(std::move(w)) {}
    std::variant<FloatWavWriter, FloatCafWriter, FloatFlacWriter> impl_;
};

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
    uint64_t read(float* out, uint64_t frames);

  private:
    explicit ReaderHandle(FloatWavReader r) : impl_(std::move(r)) {}
    explicit ReaderHandle(FloatCafReader r) : impl_(std::move(r)) {}
    explicit ReaderHandle(FloatFlacReader r) : impl_(std::move(r)) {}
    std::variant<FloatWavReader, FloatCafReader, FloatFlacReader> impl_;
};

// Apply a linear gain to all samples in path, rewriting the file in-place via a
// temp file + rename. Supports WAV, CAF, and FLAC formats. layout_id is required
// when the path is a CAF file; ignored for WAV and FLAC. No-op when |gain - 1| < 1e-6.
Result<void> apply_gain_to_file(const std::string& path, float gain, const std::string& layout_id = {});

// Convert an existing float32 WAV to integer PCM in-place (temp + rename).
// bit_depth must be 16 or 24. Limited to sample rates <= 65535 Hz by the
// underlying bw64 integer writer (libbw64 0.10.0 API constraint).
Result<void> downconvert_to_int(const std::string& path, uint16_t bit_depth);

// Encode a fully post-processed float32 WAV (src_path) to 24-bit FLAC (flac_path).
// Use this as the final pipeline step for FLAC output — after apply_gain_to_file()
// and downconvert_to_int() — so the float32 domain is preserved through all
// adjustments before quantisation.  FLAC channel count must be 1-8.
Result<void> convert_to_flac(const std::string& src_path, const std::string& flac_path);

// Encode a fully post-processed float32 WAV (src_path) to Opus MKA (mka_path).
// src_path must be 48000 Hz (Opus requirement). layout_id is reserved for
// container metadata written by the engine layer. Use this as the final pipeline
// step after all apply_gain_to_file() adjustments — re-encoding degrades lossy
// quality.
// bitrate_per_ch_kbps: VBR target per channel in kbps; 0 = auto.
Result<void> convert_to_opus_mka(const std::string& src_path,
                                 const std::string& mka_path,
                                 const std::string& layout_id = {},
                                 uint32_t bitrate_per_ch_kbps = 0);

// Encode a fully post-processed float32 WAV (src_path) to APAC in an MPEG-4
// container (.m4a / mp4f).  Requires macOS (AudioToolbox); returns
// ErrorCode::unsupported on other platforms.
// layout_id controls channel mapping:
//   "binaural" → request Binaural 2ch input layout (no swap; afinfo reports APAC output as Stereo)
//   "0+2+0"    → MPEG_2_0 stereo (no swap)
//   "wav71"    → AudioUnit_7_1  (ch4↔ch6, ch5↔ch7 swap applied before encoding)
//   "4+5+0"    → Atmos_7_1_4   (no swap)
//   "4+7+0"    → Atmos_9_1_6   (no swap)
// bitrate_kbps: total VBR target/hint in kbps; 0 = encoder default. The APAC
// encoder may produce a measured average bitrate that differs substantially.
// drc_music: true = Music DRC (cdrc=1), false = None (cdrc=0).
Result<void> convert_to_apac(const std::string& src_path,
                             const std::string& apac_path,
                             const std::string& layout_id = {},
                             uint32_t bitrate_kbps = 0,
                             bool drc_music = true);

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
//   FLAC — inserts a Vorbis Comment block (ENCODER, DATE, COMMENT tags) and, for
//           recognised layouts, a WAVEFORMATEXTENSIBLE_CHANNEL_MASK tag.
//   M4A/MP4 — inserts iTunes-style metadata atoms (©too, ©day, ©cmt). Layout
//             semantics such as "binaural" are stored in comments because APAC
//             does not reliably preserve non-stereo CoreAudio layout tags.
//   Other extensions — silently ignored (not an error).
// Failure is non-fatal; callers should log a warning and continue.
Result<void> write_file_metadata(const std::string& path, const MetadataFields& meta);

} // namespace mradm::audio
