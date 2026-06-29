// DR_WAV_IMPLEMENTATION must be defined in exactly one TU.
#define DR_WAV_IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dr_wav.h>
#include <filesystem>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#ifndef _WIN32
#include <sys/types.h>
#endif

#include "adm/audio_io.h"
#include "adm/errors.h"

namespace mradm::audio {

namespace {

class TempPathGuard {
  public:
    explicit TempPathGuard(std::filesystem::path path) : path_(std::move(path)) {}
    TempPathGuard(const TempPathGuard&) = delete;
    TempPathGuard& operator=(const TempPathGuard&) = delete;
    TempPathGuard(TempPathGuard&&) = delete;
    TempPathGuard& operator=(TempPathGuard&&) = delete;
    ~TempPathGuard() {
        if (active_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    void dismiss() noexcept { active_ = false; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

void emit_wav_progress(ProgressSink* progress,
                       RenderOperation operation,
                       double fraction,
                       uint64_t current_frame,
                       uint64_t total_frames,
                       std::string_view message) {
    if (progress == nullptr) {
        return;
    }
    const double f = std::clamp(fraction, 0.0, 1.0);
    progress->on_progress({RenderStage::post_processing, operation, f, f, current_frame, total_frames, message});
}

} // namespace

// ── FloatWavWriter ────────────────────────────────────────────────────────────

struct FloatWavWriter::Impl {
    drwav wav{};
    bool open{false};
};

Result<FloatWavWriter> FloatWavWriter::open(const std::string& path, uint32_t channels, uint32_t sample_rate) {
    FloatWavWriter w;
    w.impl_ = std::make_unique<Impl>();

    drwav_data_format fmt{};
    fmt.container = drwav_container_rf64;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = channels;
    fmt.sampleRate = sample_rate;
    fmt.bitsPerSample = 32;

    if (drwav_init_file_write(&w.impl_->wav, path.c_str(), &fmt, nullptr) == 0U) {
        return make_error(ErrorCode::io_error, "failed to open float32 WAV for writing", "path=" + path);
    }
    w.impl_->open = true;
    return w;
}

FloatWavWriter::~FloatWavWriter() {
    if (impl_ && impl_->open) {
        drwav_uninit(&impl_->wav);
    }
}

FloatWavWriter::FloatWavWriter(FloatWavWriter&&) noexcept = default;
FloatWavWriter& FloatWavWriter::operator=(FloatWavWriter&&) noexcept = default;

uint64_t FloatWavWriter::write(const float* samples, uint64_t frame_count) {
    return drwav_write_pcm_frames(&impl_->wav, frame_count, samples);
}

// ── FloatWavReader ────────────────────────────────────────────────────────────

struct FloatWavReader::Impl {
    drwav wav{};
    bool open{false};
};

Result<FloatWavReader> FloatWavReader::open(const std::string& path) {
    FloatWavReader r;
    r.impl_ = std::make_unique<Impl>();

    if (drwav_init_file(&r.impl_->wav, path.c_str(), nullptr) == 0U) {
        return make_error(ErrorCode::io_error, "failed to open WAV for reading", "path=" + path);
    }
    r.impl_->open = true;
    return r;
}

FloatWavReader::~FloatWavReader() {
    if (impl_ && impl_->open) {
        drwav_uninit(&impl_->wav);
    }
}

FloatWavReader::FloatWavReader(FloatWavReader&&) noexcept = default;
FloatWavReader& FloatWavReader::operator=(FloatWavReader&&) noexcept = default;

uint32_t FloatWavReader::channels() const {
    return impl_->wav.channels;
}
uint32_t FloatWavReader::sample_rate() const {
    return impl_->wav.sampleRate;
}
uint64_t FloatWavReader::frame_count() const {
    return impl_->wav.totalPCMFrameCount;
}

uint64_t FloatWavReader::read(float* out, uint64_t frames) {
    return drwav_read_pcm_frames_f32(&impl_->wav, frames, out);
}

// ── downconvert_to_int ────────────────────────────────────────────────────────

Result<void> downconvert_to_int(const std::string& path,
                                uint16_t bit_depth,
                                const std::stop_token& cancel_token,
                                ProgressSink* progress,
                                RenderOperation operation) {
    try {
        if (cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
        }
        if (bit_depth != 16U && bit_depth != 24U && bit_depth != 32U) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("integer PCM bit depth must be 16, 24, or 32; got {}", bit_depth),
                              "path=" + path);
        }

        const auto tmp_path = path + ".bitdepth_tmp";
        TempPathGuard tmp_guard{tmp_path};
        uint64_t total_frames = 0;
        {
            // reader 与 writer 都置于此块内，块结束即关闭句柄；之后才能在 Windows 上
            // rename 顶替原文件（POSIX 可 rename 顶替正打开的文件，Windows 会 Access denied）。
            auto reader_res = FloatWavReader::open(path);
            if (!reader_res) {
                return tl::unexpected{reader_res.error()};
            }
            auto& reader = *reader_res;

            const uint32_t channels = reader.channels();
            const uint32_t sample_rate = reader.sample_rate();
            total_frames = reader.frame_count();
            emit_wav_progress(progress, operation, 0.0, 0, total_frames, "converting bit depth");

            // libbw64 0.10.0 writeFile takes uint16_t sampleRate.
            if (sample_rate > std::numeric_limits<uint16_t>::max()) {
                return make_error(
                    ErrorCode::unsupported,
                    fmt::format(
                        "sample rate {} Hz exceeds integer PCM writer limit (65535 Hz); use --output-bit-depth f32",
                        sample_rate),
                    "path=" + path);
            }

            auto writer = bw64::writeFile(
                tmp_path, static_cast<uint16_t>(channels), static_cast<uint16_t>(sample_rate), bit_depth);

            constexpr uint64_t k_block = 4096;
            std::vector<float> buf(static_cast<std::size_t>(channels) * k_block);
            uint64_t left = total_frames;
            uint64_t done = 0;

            while (left > 0) {
                if (cancel_token.stop_requested()) {
                    return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
                }
                const uint64_t n = std::min(k_block, left);
                const uint64_t got = reader.read(buf.data(), n);
                if (got == 0) {
                    break;
                }
                writer->write(buf.data(), got);
                left -= got;
                done += got;
                emit_wav_progress(progress,
                                  operation,
                                  static_cast<double>(done) / static_cast<double>(std::max<uint64_t>(1, total_frames)),
                                  done,
                                  total_frames,
                                  "converting bit depth");
            }
        }

        if (cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
        }
        std::filesystem::rename(tmp_path, path);
        tmp_guard.dismiss();
        emit_wav_progress(progress, operation, 1.0, total_frames, total_frames, "bit depth converted");
        return {};

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("bit depth conversion failed: ") + e.what(), "path=" + path);
    }
}


namespace {

[[nodiscard]] uint32_t read_le32(const char* p) {
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8U) | (static_cast<uint32_t>(b[2]) << 16U) |
           (static_cast<uint32_t>(b[3]) << 24U);
}

[[nodiscard]] uint64_t read_le64(const char* p) {
    return static_cast<uint64_t>(read_le32(p)) | (static_cast<uint64_t>(read_le32(p + 4)) << 32U);
}

[[nodiscard]] bool file_seek_abs(std::FILE* f, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

[[nodiscard]] bool file_seek_end(std::FILE* f) {
#ifdef _WIN32
    return _fseeki64(f, 0, SEEK_END) == 0;
#else
    return fseeko(f, 0, SEEK_END) == 0;
#endif
}

[[nodiscard]] bool file_tell(std::FILE* f, uint64_t& offset) {
#ifdef _WIN32
    const __int64 pos = _ftelli64(f);
    if (pos < 0) {
        return false;
    }
    offset = static_cast<uint64_t>(pos);
    return true;
#else
    const off_t pos = ftello(f);
    if (pos < 0) {
        return false;
    }
    offset = static_cast<uint64_t>(pos);
    return true;
#endif
}

[[nodiscard]] bool file_size(std::FILE* f, uint64_t& size) {
    return file_seek_end(f) && file_tell(f, size);
}

[[nodiscard]] bool read_exact(std::FILE* f, void* data, std::size_t size) {
    return std::fread(data, 1, size, f) == size;
}

[[nodiscard]] bool fourcc_eq(const std::array<char, 4>& id, std::string_view tag) {
    return tag.size() == 4U && id[0] == tag[0] && id[1] == tag[1] && id[2] == tag[2] && id[3] == tag[3];
}

enum class RiffContainer : uint8_t { riff, rf64, bw64 };

struct RiffInfo {
    RiffContainer container{RiffContainer::riff};
    uint64_t file_size{0};
    bool have_ds64{false};
    uint64_t ds64_payload_offset{0};
    uint64_t ds64_data_size{0};
};

struct RiffChunk {
    std::array<char, 4> id{};
    uint64_t header_offset{0};
    uint64_t payload_offset{0};
    uint64_t payload_size{0};
    uint32_t size32{0};
};

[[nodiscard]] bool uses_ds64(const RiffInfo& info) {
    return info.container == RiffContainer::rf64 || info.container == RiffContainer::bw64;
}

[[nodiscard]] Result<std::optional<RiffChunk>>
find_riff_chunk(std::FILE* f, const std::string& path, std::string_view target_id, RiffInfo* out_info = nullptr) {
    RiffInfo info;
    if (!file_size(f, info.file_size) || info.file_size < 12U || !file_seek_abs(f, 0)) {
        return make_error(ErrorCode::io_error, "cannot inspect WAV file size", "path=" + path);
    }

    std::array<char, 12> riff_hdr{};
    if (!read_exact(f, riff_hdr.data(), riff_hdr.size()) || std::string_view(riff_hdr.data() + 8, 4) != "WAVE") {
        return make_error(ErrorCode::io_error, "not a RIFF/RF64/BW64 WAVE file", "path=" + path);
    }
    const std::string_view riff_tag{riff_hdr.data(), 4};
    if (riff_tag == "RIFF") {
        info.container = RiffContainer::riff;
    } else if (riff_tag == "RF64") {
        info.container = RiffContainer::rf64;
    } else if (riff_tag == "BW64") {
        info.container = RiffContainer::bw64;
    } else {
        return make_error(ErrorCode::io_error, "not a RIFF/RF64/BW64 WAVE file", "path=" + path);
    }

    std::optional<RiffChunk> found;
    uint64_t pos = 12U;
    while (pos + 8U <= info.file_size) {
        std::array<char, 8> hdr{};
        if (!file_seek_abs(f, pos) || !read_exact(f, hdr.data(), hdr.size())) {
            return make_error(ErrorCode::io_error, "failed to read WAV chunk header", "path=" + path);
        }
        RiffChunk chunk;
        chunk.id = {hdr[0], hdr[1], hdr[2], hdr[3]};
        chunk.header_offset = pos;
        chunk.payload_offset = pos + 8U;
        chunk.size32 = read_le32(hdr.data() + 4);
        chunk.payload_size = chunk.size32;

        if (fourcc_eq(chunk.id, "ds64")) {
            if (chunk.payload_size < 28U) {
                return make_error(ErrorCode::io_error, "invalid WAV ds64 chunk", "path=" + path);
            }
            std::array<char, 28> ds64{};
            if (!read_exact(f, ds64.data(), ds64.size())) {
                return make_error(ErrorCode::io_error, "failed to read WAV ds64 chunk", "path=" + path);
            }
            info.have_ds64 = true;
            info.ds64_payload_offset = chunk.payload_offset;
            info.ds64_data_size = read_le64(ds64.data() + 8);
        } else if (uses_ds64(info) && fourcc_eq(chunk.id, "data") && chunk.size32 == 0xFFFFFFFFU) {
            if (!info.have_ds64) {
                return make_error(ErrorCode::io_error, "RF64/BW64 WAV data chunk appears before ds64", "path=" + path);
            }
            chunk.payload_size = info.ds64_data_size;
        }

        if (!found.has_value() && target_id.size() == 4U && fourcc_eq(chunk.id, target_id)) {
            found = chunk;
        }

        const uint64_t padding = chunk.payload_size & 1ULL;
        if (chunk.payload_offset > std::numeric_limits<uint64_t>::max() - chunk.payload_size - padding) {
            return make_error(ErrorCode::io_error, "WAV chunk table overflow", "path=" + path);
        }
        const uint64_t next = chunk.payload_offset + chunk.payload_size + padding;
        if (next <= pos) {
            return make_error(ErrorCode::io_error, "invalid WAV chunk table", "path=" + path);
        }
        pos = next;
    }

    if (uses_ds64(info) && !info.have_ds64) {
        return make_error(ErrorCode::io_error, "RF64/BW64 WAV file missing ds64 chunk", "path=" + path);
    }
    if (out_info != nullptr) {
        *out_info = info;
    }
    return found;
}

void write_le16_file(std::FILE* f, int16_t v) {
    const auto u = static_cast<uint16_t>(v);
    const std::array<uint8_t, 2> bytes{static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8U)};
    std::fwrite(bytes.data(), 1, bytes.size(), f);
}

void write_le32_file(std::FILE* f, uint32_t v) {
    const std::array<uint8_t, 4> bytes{static_cast<uint8_t>(v),
                                       static_cast<uint8_t>(v >> 8U),
                                       static_cast<uint8_t>(v >> 16U),
                                       static_cast<uint8_t>(v >> 24U)};
    std::fwrite(bytes.data(), 1, bytes.size(), f);
}

void write_le64_file(std::FILE* f, uint64_t v) {
    const std::array<uint8_t, 8> bytes{static_cast<uint8_t>(v),
                                       static_cast<uint8_t>(v >> 8U),
                                       static_cast<uint8_t>(v >> 16U),
                                       static_cast<uint8_t>(v >> 24U),
                                       static_cast<uint8_t>(v >> 32U),
                                       static_cast<uint8_t>(v >> 40U),
                                       static_cast<uint8_t>(v >> 48U),
                                       static_cast<uint8_t>(v >> 56U)};
    std::fwrite(bytes.data(), 1, bytes.size(), f);
}

Result<void> update_riff_sizes(std::FILE* f, const std::string& path, const RiffInfo& info) {
    uint64_t final_file_size = 0;
    if (!file_size(f, final_file_size) || final_file_size < 8U) {
        return make_error(ErrorCode::io_error, "cannot inspect final WAV file size", "path=" + path);
    }
    const uint64_t riff_size = final_file_size - 8U;
    if (uses_ds64(info)) {
        if (!info.have_ds64 || !file_seek_abs(f, info.ds64_payload_offset)) {
            return make_error(ErrorCode::io_error, "cannot update WAV ds64 chunk", "path=" + path);
        }
        write_le64_file(f, riff_size);
        return {};
    }
    if (riff_size > std::numeric_limits<uint32_t>::max()) {
        return make_error(
            ErrorCode::unsupported, "RIFF WAV metadata would exceed 4GB; use RF64/BW64 output", "path=" + path);
    }
    if (!file_seek_abs(f, 4U)) {
        return make_error(ErrorCode::io_error, "cannot update WAV RIFF size", "path=" + path);
    }
    write_le32_file(f, static_cast<uint32_t>(riff_size));
    return {};
}

Result<void> write_hoa3_ambi_chunk(std::FILE* f, const std::string& path) {
    // Source-compatible AmbiX marker for HOA WAV:
    // ambisonic_type=B-format(1), ordering=ACN(2), normalisation=SN3D(2),
    // n_channels=16. CoreAudio does not expose a native HOA tag for WAV,
    // so this chunk carries the semantic marker for tools that understand it.
    constexpr uint32_t k_ambi_payload = 16;
    constexpr std::array<uint32_t, 4> k_hoa3_ambi{1U, 2U, 2U, 16U};

    const auto existing_ambi_res = find_riff_chunk(f, path, "ambi");
    if (!existing_ambi_res) {
        return tl::unexpected{existing_ambi_res.error()};
    }
    const auto& existing_ambi = *existing_ambi_res;
    if (existing_ambi.has_value()) {
        std::array<char, 8> ambi_hdr{};
        if (!file_seek_abs(f, existing_ambi->header_offset) || !read_exact(f, ambi_hdr.data(), ambi_hdr.size()) ||
            read_le32(ambi_hdr.data() + 4) != k_ambi_payload) {
            return make_error(ErrorCode::io_error, "invalid existing WAV ambi chunk", "path=" + path);
        }
        if (!file_seek_abs(f, existing_ambi->payload_offset)) {
            return make_error(ErrorCode::io_error, "cannot seek WAV ambi chunk", "path=" + path);
        }
    } else {
        if (!file_seek_end(f)) {
            return make_error(ErrorCode::io_error, "cannot append WAV ambi chunk", "path=" + path);
        }
        std::fwrite("ambi", 1, 4, f);
        write_le32_file(f, k_ambi_payload);
    }
    for (const uint32_t v : k_hoa3_ambi) {
        write_le32_file(f, v);
    }
    return {};
}

Result<RiffInfo> inspect_wav_metadata_target(std::FILE* f, const std::string& path, std::string_view output_layout) {
    RiffInfo riff_info;
    auto ambi_lookup = find_riff_chunk(f, path, "ambi", &riff_info);
    if (!ambi_lookup) {
        return tl::unexpected{ambi_lookup.error()};
    }

    constexpr uint32_t k_bext_total = 8 + 602; // FourCC(4) + size(4) + payload
    constexpr uint32_t k_ambi_total = 8 + 16;  // FourCC(4) + size(4) + payload
    const bool appending_ambi = output_layout == "hoa3" && !ambi_lookup->has_value();
    const uint64_t appended_bytes = k_bext_total + (appending_ambi ? k_ambi_total : 0U);
    if (!uses_ds64(riff_info) && (riff_info.file_size < 8U ||
                                  riff_info.file_size - 8U > std::numeric_limits<uint32_t>::max() - appended_bytes)) {
        return make_error(
            ErrorCode::unsupported, "RIFF WAV metadata would exceed 4GB; use RF64/BW64 output", "path=" + path);
    }
    return riff_info;
}

} // namespace

// Append a BWF v2 bext chunk to an existing RIFF/RF64/BW64 WAVE file and update
// the container size. EBU Tech 3285 supplement 5 field layout (602-byte fixed payload).
Result<void> write_wav_metadata(const std::string& path, const MetadataFields& meta) {
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (f == nullptr) {
        return make_error(ErrorCode::io_error, "cannot open WAV for metadata write", "path=" + path);
    }

    auto riff_info_res = inspect_wav_metadata_target(f, path, meta.output_layout);
    if (!riff_info_res) {
        std::fclose(f);
        return tl::unexpected{riff_info_res.error()};
    }
    const RiffInfo riff_info = *riff_info_res;

    // Fixed bext payload size (no coding history).
    constexpr uint32_t k_payload = 602;

    // Null-padded fixed-width string helper.
    const auto write_str_field = [&](const std::string& s, std::size_t width) {
        std::vector<char> buf(width, '\0');
        std::memcpy(buf.data(), s.c_str(), std::min(s.size(), width - 1));
        std::fwrite(buf.data(), 1, width, f);
    };

    // Write bext chunk at EOF.
    if (!file_seek_end(f)) {
        std::fclose(f);
        return make_error(ErrorCode::io_error, "cannot append WAV metadata", "path=" + path);
    }

    // Chunk header (LE chunk size).
    std::fwrite("bext", 1, 4, f);
    write_le32_file(f, k_payload);

    // Description[256] — renderer + layout summary.
    const std::string desc = meta.renderer.empty() ? "" : "renderer=" + meta.renderer + " layout=" + meta.output_layout;
    write_str_field(desc, 256);

    // Originator[32].
    write_str_field(meta.encoder, 32);

    // OriginatorReference[32] — layout id.
    write_str_field(meta.output_layout, 32);

    // OriginationDate[10] "yyyy-mm-dd" and OriginationTime[8] "hh-mm-ss".
    std::array<char, 10> odate{};
    std::array<char, 8> otime{};
    if (meta.date_utc.size() >= 10) {
        std::memcpy(odate.data(), meta.date_utc.c_str(), 10);
    }
    if (meta.date_utc.size() >= 19) {
        // ISO 8601 "Thh:mm:ss" — EBU spec wants "hh-mm-ss".
        otime[0] = meta.date_utc[11];
        otime[1] = meta.date_utc[12];
        otime[2] = '-';
        otime[3] = meta.date_utc[14];
        otime[4] = meta.date_utc[15];
        otime[5] = '-';
        otime[6] = meta.date_utc[17];
        otime[7] = meta.date_utc[18];
    }
    std::fwrite(odate.data(), 1, odate.size(), f);
    std::fwrite(otime.data(), 1, otime.size(), f);

    // TimeReferenceLow + TimeReferenceHigh (8 bytes, zero).
    write_le32_file(f, 0);
    write_le32_file(f, 0);

    // Version = 2 (BWF v2 for loudness fields).
    write_le16_file(f, 2);

    // UMID[64] — zero.
    std::array<uint8_t, 64> umid{};
    std::fwrite(umid.data(), 1, umid.size(), f);

    // Loudness fields (int16_t LE, unit = 0.01; 0x7FFF = not-indicated).
    constexpr int16_t k_ni = 0x7FFF;
    const int16_t loudness_val = meta.lufs ? static_cast<int16_t>(std::lround(*meta.lufs * 100.0)) : k_ni;
    const int16_t peak_val = meta.peak_dbtp ? static_cast<int16_t>(std::lround(*meta.peak_dbtp * 100.0)) : k_ni;
    write_le16_file(f, loudness_val); // LoudnessValue
    write_le16_file(f, k_ni);         // LoudnessRange — not measured
    write_le16_file(f, peak_val);     // MaxTruePeakLevel
    write_le16_file(f, k_ni);         // MaxMomentaryLoudness — not measured
    write_le16_file(f, k_ni);         // MaxShortTermLoudness — not measured

    // Reserved[180] — zero.
    std::array<uint8_t, 180> reserved{};
    std::fwrite(reserved.data(), 1, reserved.size(), f);
    // Byte count: 256+32+32+10+8+4+4+2+64+2+2+2+2+2+180 = 602 ✓

    if (meta.output_layout == "hoa3") {
        auto ambi_res = write_hoa3_ambi_chunk(f, path);
        if (!ambi_res) {
            std::fclose(f);
            return tl::unexpected{ambi_res.error()};
        }
    }

    auto size_res = update_riff_sizes(f, path, riff_info);
    if (!size_res) {
        std::fclose(f);
        return tl::unexpected{size_res.error()};
    }

    std::fclose(f);
    return {};
}


} // namespace mradm::audio
