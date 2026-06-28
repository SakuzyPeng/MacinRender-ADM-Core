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
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <bw64/bw64.hpp>
#include <fmt/format.h>

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
    fmt.container = drwav_container_riff;
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

[[nodiscard]] long find_riff_chunk(std::FILE* f, std::string_view id) {
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    long offset = 12;
    while (offset + 8 <= file_size) {
        std::array<char, 8> hdr{};
        std::fseek(f, offset, SEEK_SET);
        if (std::fread(hdr.data(), 1, hdr.size(), f) != hdr.size()) {
            return -1;
        }
        const uint32_t size = read_le32(hdr.data() + 4);
        if (std::string_view(hdr.data(), 4) == id) {
            return offset;
        }
        offset += 8 + static_cast<long>(size) + static_cast<long>(size & 1U);
    }
    return -1;
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

Result<void> write_hoa3_ambi_chunk(std::FILE* f, const std::string& path) {
    // Source-compatible AmbiX marker for HOA WAV:
    // ambisonic_type=B-format(1), ordering=ACN(2), normalisation=SN3D(2),
    // n_channels=16. CoreAudio does not expose a native HOA tag for WAV,
    // so this chunk carries the semantic marker for tools that understand it.
    constexpr uint32_t k_ambi_payload = 16;
    constexpr std::array<uint32_t, 4> k_hoa3_ambi{1U, 2U, 2U, 16U};

    const long existing_ambi = find_riff_chunk(f, "ambi");
    if (existing_ambi >= 0) {
        std::array<char, 8> ambi_hdr{};
        std::fseek(f, existing_ambi, SEEK_SET);
        if (std::fread(ambi_hdr.data(), 1, ambi_hdr.size(), f) != ambi_hdr.size() ||
            read_le32(ambi_hdr.data() + 4) != k_ambi_payload) {
            return make_error(ErrorCode::io_error, "invalid existing WAV ambi chunk", "path=" + path);
        }
        std::fseek(f, existing_ambi + 8, SEEK_SET);
    } else {
        std::fseek(f, 0, SEEK_END);
        std::fwrite("ambi", 1, 4, f);
        write_le32_file(f, k_ambi_payload);
    }
    for (const uint32_t v : k_hoa3_ambi) {
        write_le32_file(f, v);
    }
    return {};
}

} // namespace

// Append a BWF v2 bext chunk to an existing RIFF/WAVE file and update the RIFF
// size.  EBU Tech 3285 supplement 5 field layout (602-byte fixed payload).
Result<void> write_wav_metadata(const std::string& path, const MetadataFields& meta) {
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (f == nullptr) {
        return make_error(ErrorCode::io_error, "cannot open WAV for metadata write", "path=" + path);
    }

    // Verify RIFF/WAVE magic.
    std::array<char, 12> hdr{};
    if (std::fread(hdr.data(), 1, hdr.size(), f) != hdr.size() || std::string_view(hdr.data(), 4) != "RIFF" ||
        std::string_view(hdr.data() + 8, 4) != "WAVE") {
        std::fclose(f);
        return make_error(ErrorCode::io_error, "not a RIFF/WAVE file", "path=" + path);
    }
    const uint32_t riff_size = read_le32(hdr.data() + 4);

    // Fixed bext payload size (no coding history).
    constexpr uint32_t k_payload = 602;
    constexpr uint32_t k_chunk_total = 8 + k_payload; // FourCC(4) + size(4) + payload

    // Null-padded fixed-width string helper.
    const auto write_str_field = [&](const std::string& s, std::size_t width) {
        std::vector<char> buf(width, '\0');
        std::memcpy(buf.data(), s.c_str(), std::min(s.size(), width - 1));
        std::fwrite(buf.data(), 1, width, f);
    };

    // Write bext chunk at EOF.
    std::fseek(f, 0, SEEK_END);

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

    // Update RIFF size (bytes 4-7, LE).
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    const uint32_t new_riff_size = file_size >= 8 ? static_cast<uint32_t>(file_size - 8) : riff_size + k_chunk_total;
    std::fseek(f, 4, SEEK_SET);
    write_le32_file(f, new_riff_size);

    std::fclose(f);
    return {};
}


} // namespace mradm::audio
