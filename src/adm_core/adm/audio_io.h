#pragma once

#include <cstdint>
#include <ios>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include "adm/errors.h"
#include "adm/progress.h"

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

bool iamf_encoding_available();

// Encode a fully post-processed integer PCM WAV (src_path) to a standalone IAMF
// raw OBU stream (iamf_path, .iamf extension) using the official AOM iamf-tools
// bridge. The render pipeline converts its float staging WAV to 32-bit PCM
// before calling this. Builds without MR_ADM_ENABLE_IAMF return ErrorCode::unsupported.
// src_path must be 48000 Hz. bitrate_per_ch_kbps is an Opus VBR target hint.
// loudness_lufs / peak_dbtp are post-gain values written into the Mix Presentation.
// scalable_layers is optional; empty preserves the current single-layer output.
Result<void> convert_to_iamf(const std::string& src_path,
                             const std::string& iamf_path,
                             const std::string& layout_id = {},
                             uint32_t bitrate_per_ch_kbps = 0,
                             std::optional<double> loudness_lufs = std::nullopt,
                             std::optional<double> peak_dbtp = std::nullopt,
                             const std::stop_token& cancel_token = {},
                             const std::vector<std::string>& scalable_layers = {});

// IAMF-to-MP4 packaging (ISOBMFF Annex A encapsulation).
// Prefers mp4box (GPAC) over ffmpeg; the packager is selected once at
// detection time and used as-is — no runtime fallback on failure.
enum class IamfMp4PackagerKind : uint8_t { none, mp4box, ffmpeg };
struct IamfMp4PackagerInfo {
    IamfMp4PackagerKind kind{IamfMp4PackagerKind::none};
    int ffmpeg_major{-1};   // populated only when kind == ffmpeg
    std::string executable; // actual binary name found in PATH ("mp4box", "MP4Box", "ffmpeg")
};
// Detect the best available IAMF-to-MP4 packager in PATH (mp4box > ffmpeg).
IamfMp4PackagerInfo detect_iamf_mp4_packager();
bool iamf_mp4_packager_available();
// Package a raw .iamf OBU stream into an ISOBMFF container (.mp4).
// Returns unsupported if no suitable packager is found in PATH.
Result<void> package_iamf_to_mp4(const std::string& iamf_path,
                                 const std::string& mp4_path,
                                 const std::stop_token& cancel_token = {});

enum class ApacContainer {
    mpeg4,
    caf,
};

// Encode a fully post-processed float32 WAV (src_path) to APAC in the requested
// container (.m4a / .mp4 MPEG-4 by default, or .caf). Requires macOS
// (AudioToolbox); returns
// ErrorCode::unsupported on other platforms.
// layout_id controls channel mapping:
//   "binaural" → request Binaural 2ch input layout (no swap; afinfo reports APAC output as Stereo)
//   "0+2+0"    → MPEG_2_0 stereo (no swap)
//   "wav71"    → AudioUnit_7_1  (ch4↔ch6, ch5↔ch7 swap applied before encoding)
//   "hoa3"     → HOA_ACN_SN3D 16ch (ACN 0..15, no swap)
//   "5.1.4"    → Atmos_5_1_4   (no swap)
//   "7.1.4"    → Atmos_7_1_4   (no swap)
//   "9.1.6"    → Atmos_9_1_6   (no swap)
//   "22.2"     → CICP_13       (no swap)
// bitrate_kbps: total target/hint in kbps; 0 = layout default for spatial/HOA
// layouts, otherwise encoder default. The APAC encoder may produce a measured
// average bitrate that differs substantially.
// drc_music: true = Music DRC (cdrc=1), false = None (cdrc=0).
// progress: optional frame-level encode progress, emitted as RenderOperation::encode_apac by default.
// Whether APAC encoding is available in this build (macOS / AudioToolbox only).
// On non-Apple platforms convert_to_apac returns ErrorCode::unsupported.
bool apac_encoding_available();

Result<void> convert_to_apac(const std::string& src_path,
                             const std::string& apac_path,
                             const std::string& layout_id = {},
                             uint32_t bitrate_kbps = 0,
                             bool drc_music = true,
                             ApacContainer container = ApacContainer::mpeg4,
                             const std::stop_token& cancel_token = {},
                             ProgressSink* progress = nullptr,
                             RenderOperation operation = RenderOperation::encode_apac);

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
//   WAV  — appends a BWF v2 bext chunk (EBU Tech 3285) and updates RIFF/RF64/BW64
//           container sizes. Fields: Originator, OriginationDate/Time, LoudnessValue,
//           MaxTruePeakLevel (0x7FFF = not-indicated when value is absent).
//   CAF  — appends an info chunk with encodingapplication / date / comments
//           keys.  Comments encodes renderer, layout, loudness, peak.
//   M4A/MP4 — inserts iTunes-style metadata atoms (©too, ©day, ©cmt). Layout
//             semantics such as "binaural" are stored in comments because APAC
//             does not reliably preserve non-stereo CoreAudio layout tags.
//   Other extensions — silently ignored (not an error).
// Failure is non-fatal; callers should log a warning and continue.
Result<void> write_file_metadata(const std::string& path, const MetadataFields& meta);

} // namespace mradm::audio
