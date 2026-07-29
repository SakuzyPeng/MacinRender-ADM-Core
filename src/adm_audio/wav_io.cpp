// DR_WAV_IMPLEMENTATION must be defined in exactly one TU.
#define DR_WAV_IMPLEMENTATION
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dr_wav.h>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#ifndef _WIN32
#include <sys/types.h>
#endif

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "audio_io_internal.h"

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
    std::FILE* file{nullptr};
    bool spoof_bw64_as_rf64{false};
    bool open{false};

    static int64_t tell_file(std::FILE* file) {
#ifdef _WIN32
        return static_cast<int64_t>(_ftelli64(file));
#else
        return static_cast<int64_t>(ftello(file));
#endif
    }

    static bool seek_file(std::FILE* file, int64_t offset, int whence) {
#ifdef _WIN32
        return _fseeki64(file, static_cast<__int64>(offset), whence) == 0;
#else
        return fseeko(file, static_cast<off_t>(offset), whence) == 0;
#endif
    }

    static size_t read(void* user_data, void* out, size_t bytes) {
        auto* self = static_cast<Impl*>(user_data);
        const int64_t start = tell_file(self->file);
        const size_t got = std::fread(out, 1U, bytes, self->file);
        if (self->spoof_bw64_as_rf64 && start == 0 && got >= 4U && out != nullptr) {
            auto* data = static_cast<char*>(out);
            if (std::memcmp(data, "BW64", 4U) == 0) {
                std::memcpy(data, "RF64", 4U);
            }
        }
        return got;
    }

    static drwav_bool32 seek(void* user_data, int offset, drwav_seek_origin origin) {
        auto* self = static_cast<Impl*>(user_data);
        int whence = SEEK_SET;
        if (origin == DRWAV_SEEK_CUR) {
            whence = SEEK_CUR;
        } else if (origin == DRWAV_SEEK_END) {
            whence = SEEK_END;
        }
        return seek_file(self->file, offset, whence) ? DRWAV_TRUE : DRWAV_FALSE;
    }

    static drwav_bool32 tell(void* user_data, drwav_int64* cursor) {
        auto* self = static_cast<Impl*>(user_data);
        const int64_t position = tell_file(self->file);
        if (position < 0) {
            return DRWAV_FALSE;
        }
        *cursor = static_cast<drwav_int64>(position);
        return DRWAV_TRUE;
    }
};

Result<FloatWavReader> FloatWavReader::open(const std::string& path) {
    FloatWavReader r;
    r.impl_ = std::make_unique<Impl>();

    r.impl_->file = std::fopen(path.c_str(), "rb");
    if (r.impl_->file == nullptr) {
        return make_error(ErrorCode::io_error, "failed to open WAV for reading", "path=" + path);
    }
    std::array<char, 4> signature{};
    const size_t signature_size = std::fread(signature.data(), 1U, signature.size(), r.impl_->file);
    r.impl_->spoof_bw64_as_rf64 = signature_size == signature.size() && std::memcmp(signature.data(), "BW64", 4U) == 0;
    if (!Impl::seek_file(r.impl_->file, 0, SEEK_SET)) {
        std::fclose(r.impl_->file);
        r.impl_->file = nullptr;
        return make_error(ErrorCode::io_error, "failed to seek WAV for reading", "path=" + path);
    }
    if (drwav_init(&r.impl_->wav, Impl::read, Impl::seek, Impl::tell, r.impl_.get(), nullptr) == 0U) {
        std::fclose(r.impl_->file);
        r.impl_->file = nullptr;
        return make_error(ErrorCode::io_error, "failed to open WAV for reading", "path=" + path);
    }
    r.impl_->open = true;
    return r;
}

FloatWavReader::~FloatWavReader() {
    if (impl_ && impl_->open) {
        drwav_uninit(&impl_->wav);
    }
    if (impl_ && impl_->file != nullptr) {
        std::fclose(impl_->file);
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
uint32_t FloatWavReader::channel_mask() const {
    return impl_->wav.fmt.channelMask;
}
uint16_t FloatWavReader::bits_per_sample() const {
    return impl_->wav.bitsPerSample;
}
bool FloatWavReader::is_linear_pcm() const {
    return impl_->wav.translatedFormatTag == DR_WAVE_FORMAT_PCM;
}
bool FloatWavReader::is_ieee_float() const {
    return impl_->wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT;
}

uint64_t FloatWavReader::read(float* out, uint64_t frames) {
    return drwav_read_pcm_frames_f32(&impl_->wav, frames, out);
}

bool FloatWavReader::seek(uint64_t frame) {
    return drwav_seek_to_pcm_frame(&impl_->wav, frame) != 0U;
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

struct WavSourceFormat {
    uint32_t channels{0};
    uint32_t sample_rate{0};
    uint64_t frames{0};
    uint16_t bits_per_sample{0};
    bool is_pcm{false};
    bool is_float{false};
    RiffInfo riff;
    RiffChunk data;
};

[[nodiscard]] std::optional<std::filesystem::path> unique_wav_sidecar_path(const std::filesystem::path& original,
                                                                           std::string_view purpose) {
    const auto parent = original.parent_path();
    const auto stem = original.stem().string();
    const auto extension = original.extension().string();
    for (uint32_t index = 0; index < 1024U; ++index) {
        auto candidate = parent / fmt::format("{}.{}.{:04}{}", stem, purpose, index, extension);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool write_exact(std::FILE* f, const void* data, std::size_t size) {
    return size == 0U || std::fwrite(data, 1U, size, f) == size;
}

[[nodiscard]] bool write_fourcc(std::FILE* f, std::string_view id) {
    return id.size() == 4U && write_exact(f, id.data(), id.size());
}

[[nodiscard]] bool write_chunk_header(std::FILE* f, std::string_view id, uint32_t payload_size) {
    if (!write_fourcc(f, id)) {
        return false;
    }
    write_le32_file(f, payload_size);
    return std::ferror(f) == 0;
}

void append_le16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8U));
}

void append_fixed_string(std::vector<uint8_t>& out, std::string_view value, std::size_t width) {
    const auto count = std::min(value.size(), width);
    std::ranges::transform(value.begin(),
                           value.begin() + static_cast<std::ptrdiff_t>(count),
                           std::back_inserter(out),
                           [](char c) { return static_cast<uint8_t>(c); });
    out.insert(out.end(), width - count, static_cast<uint8_t>(' '));
}

[[nodiscard]] Result<std::vector<uint8_t>>
build_chna_payload(uint32_t channels, const std::vector<WavChnaEntry>& entries, const std::string& path) {
    if (channels > std::numeric_limits<uint16_t>::max() || entries.size() > std::numeric_limits<uint16_t>::max()) {
        return make_error(ErrorCode::unsupported, "WAV CHNA track count exceeds 65535", "path=" + path);
    }
    std::vector<bool> used_track_indices(static_cast<std::size_t>(channels), false);
    std::vector<uint8_t> payload;
    payload.reserve(4U + (entries.size() * 40U));
    append_le16(payload, static_cast<uint16_t>(channels));
    append_le16(payload, static_cast<uint16_t>(entries.size()));
    for (const auto& entry : entries) {
        if (entry.track_index == 0U || entry.track_index > channels) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("CHNA track index {} is outside 1..{}", entry.track_index, channels),
                              "path=" + path);
        }
        const auto track_slot = static_cast<std::size_t>(entry.track_index - 1U);
        if (used_track_indices[track_slot]) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("duplicate CHNA track index {}", entry.track_index),
                              "path=" + path);
        }
        used_track_indices[track_slot] = true;
        if (entry.track_uid.size() > 12U || entry.track_format.size() > 14U || entry.pack_format.size() > 11U) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("CHNA identifiers exceed fixed field widths for track {}", entry.track_index),
                              "path=" + path);
        }
        append_le16(payload, entry.track_index);
        append_fixed_string(payload, entry.track_uid, 12U);
        append_fixed_string(payload, entry.track_format, 14U);
        append_fixed_string(payload, entry.pack_format, 11U);
        payload.push_back(static_cast<uint8_t>(' '));
    }
    return payload;
}

[[nodiscard]] Result<WavSourceFormat> inspect_wav_for_layout_rewrite(const std::string& path) {
    WavSourceFormat source;
    {
        auto reader_res = FloatWavReader::open(path);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        const auto& reader = *reader_res;
        source.channels = reader.channels();
        source.sample_rate = reader.sample_rate();
        source.frames = reader.frame_count();
        source.bits_per_sample = reader.bits_per_sample();
        source.is_pcm = reader.is_linear_pcm();
        source.is_float = reader.is_ieee_float();
    }

    if (source.channels == 0U || source.sample_rate == 0U || source.frames == 0U) {
        return make_error(
            ErrorCode::invalid_argument, "WAV layout finalization requires non-empty audio", "path=" + path);
    }
    const bool supported_pcm = source.is_pcm && (source.bits_per_sample == 16U || source.bits_per_sample == 24U ||
                                                 source.bits_per_sample == 32U);
    const bool supported_float = source.is_float && source.bits_per_sample == 32U;
    if (!supported_pcm && !supported_float) {
        return make_error(
            ErrorCode::unsupported,
            fmt::format("WAV layout finalization does not support this {}-bit sample format", source.bits_per_sample),
            "path=" + path);
    }

    using FilePtr = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    FilePtr file{std::fopen(path.c_str(), "rb"), &std::fclose};
    if (!file) {
        return make_error(ErrorCode::io_error, "cannot open WAV for layout inspection", "path=" + path);
    }
    auto data_res = find_riff_chunk(file.get(), path, "data", &source.riff);
    if (!data_res) {
        return tl::unexpected{data_res.error()};
    }
    auto data_chunk = *data_res;
    if (!data_chunk.has_value()) {
        return make_error(ErrorCode::io_error, "WAV data chunk is missing", "path=" + path);
    }
    source.data = *data_chunk;

    const uint64_t bytes_per_sample = source.bits_per_sample / 8U;
    if (source.frames > std::numeric_limits<uint64_t>::max() / source.channels ||
        source.frames * source.channels > std::numeric_limits<uint64_t>::max() / bytes_per_sample) {
        return make_error(ErrorCode::unsupported, "WAV sample data size overflows 64-bit range", "path=" + path);
    }
    const uint64_t expected_data_size = source.frames * source.channels * bytes_per_sample;
    if (source.data.payload_size != expected_data_size) {
        return make_error(ErrorCode::io_error,
                          fmt::format("WAV data size {} does not match {} frames x {} channels x {} bytes",
                                      source.data.payload_size,
                                      source.frames,
                                      source.channels,
                                      bytes_per_sample),
                          "path=" + path);
    }
    return source;
}

[[nodiscard]] Result<std::vector<uint16_t>>
validate_wav_permutation(uint32_t channels, const std::vector<uint16_t>& requested, const std::string& path) {
    if (requested.empty()) {
        std::vector<uint16_t> permutation(channels);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            permutation[channel] = static_cast<uint16_t>(channel);
        }
        return permutation;
    }
    if (requested.size() != channels) {
        return make_error(
            ErrorCode::invalid_argument,
            fmt::format("WAV channel permutation contains {} entries for {} channels", requested.size(), channels),
            "path=" + path);
    }
    std::vector<bool> used(channels, false);
    for (const uint16_t source : requested) {
        if (source >= channels || used[source]) {
            return make_error(ErrorCode::invalid_argument, "WAV channel permutation is not bijective", "path=" + path);
        }
        used[source] = true;
    }
    return requested;
}

[[nodiscard]] uint64_t riff_chunk_disk_size(uint64_t payload_size) {
    return 8U + payload_size + (payload_size & 1U);
}

[[nodiscard]] bool
write_wave_format(std::FILE* out, const WavSourceFormat& source, uint32_t channel_mask, bool extensible) {
    const uint32_t bytes_per_sample = source.bits_per_sample / 8U;
    const uint32_t block_alignment = source.channels * bytes_per_sample;
    const uint64_t bytes_per_second_64 = static_cast<uint64_t>(source.sample_rate) * block_alignment;
    if (source.channels > std::numeric_limits<uint16_t>::max() ||
        block_alignment > std::numeric_limits<uint16_t>::max() ||
        bytes_per_second_64 > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    const uint32_t payload_size = extensible ? 40U : 16U;
    if (!write_chunk_header(out, "fmt ", payload_size)) {
        return false;
    }
    uint16_t format_tag = source.is_float ? 3U : 1U;
    if (extensible) {
        format_tag = 0xFFFEU;
    }
    write_le16_file(out, static_cast<int16_t>(format_tag));
    write_le16_file(out, static_cast<int16_t>(source.channels));
    write_le32_file(out, source.sample_rate);
    write_le32_file(out, static_cast<uint32_t>(bytes_per_second_64));
    write_le16_file(out, static_cast<int16_t>(block_alignment));
    write_le16_file(out, static_cast<int16_t>(source.bits_per_sample));
    if (extensible) {
        write_le16_file(out, 22); // cbSize
        write_le16_file(out, static_cast<int16_t>(source.bits_per_sample));
        write_le32_file(out, channel_mask);
        // KSDATAFORMAT_SUBTYPE_PCM / KSDATAFORMAT_SUBTYPE_IEEE_FLOAT.
        write_le32_file(out, source.is_float ? 3U : 1U);
        write_le16_file(out, 0);
        write_le16_file(out, 0x0010);
        constexpr std::array<uint8_t, 8> k_wave_subformat_tail{0x80U, 0x00U, 0x00U, 0xAAU, 0x00U, 0x38U, 0x9BU, 0x71U};
        if (!write_exact(out, k_wave_subformat_tail.data(), k_wave_subformat_tail.size())) {
            return false;
        }
    }
    return std::ferror(out) == 0;
}

[[nodiscard]] bool write_payload_chunk(std::FILE* out, std::string_view id, const void* data, uint64_t size) {
    if (size > std::numeric_limits<uint32_t>::max() || !write_chunk_header(out, id, static_cast<uint32_t>(size)) ||
        (size > 0U && !write_exact(out, data, static_cast<std::size_t>(size)))) {
        return false;
    }
    if ((size & 1U) != 0U) {
        constexpr uint8_t k_padding = 0;
        return write_exact(out, &k_padding, 1U);
    }
    return true;
}

[[nodiscard]] Result<void> replace_wav_with_rewrite(const std::filesystem::path& original,
                                                    const std::filesystem::path& rewritten,
                                                    TempPathGuard& rewritten_guard) {
    const auto backup = unique_wav_sidecar_path(original, "layout_backup");
    if (!backup.has_value()) {
        return make_error(ErrorCode::io_error, "cannot allocate WAV layout backup path", "path=" + original.string());
    }
    TempPathGuard backup_guard{*backup};
    std::error_code ec;
    std::filesystem::rename(original, *backup, ec);
    if (ec) {
        return make_error(ErrorCode::io_error,
                          "cannot move original WAV before layout replacement: " + ec.message(),
                          "path=" + original.string());
    }
    std::filesystem::rename(rewritten, original, ec);
    if (ec) {
        std::error_code restore_ec;
        std::filesystem::rename(*backup, original, restore_ec);
        if (restore_ec) {
            backup_guard.dismiss();
            return make_error(ErrorCode::io_error,
                              "cannot install layout-finalized WAV and could not restore original: " + ec.message() +
                                  "; original preserved at " + backup->string(),
                              "path=" + original.string());
        }
        return make_error(
            ErrorCode::io_error, "cannot install layout-finalized WAV: " + ec.message(), "path=" + original.string());
    }
    rewritten_guard.dismiss();
    return {};
}

} // namespace

// NOLINTNEXTLINE(readability-function-size): binary container rewrite is intentionally kept transactional in one flow.
Result<void> finalize_wav_layout(const std::string& path,
                                 const WavLayoutFinalization& layout,
                                 const std::stop_token& cancel_token,
                                 ProgressSink* progress,
                                 RenderOperation operation) {
    if (cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
    }
    auto source_res = inspect_wav_for_layout_rewrite(path);
    if (!source_res) {
        return tl::unexpected{source_res.error()};
    }
    const WavSourceFormat source = *source_res;

    if (layout.channel_mask != 0U && std::cmp_not_equal(std::popcount(layout.channel_mask), source.channels)) {
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("WAV channel mask 0x{:X} has {} positions for {} channels",
                                      layout.channel_mask,
                                      std::popcount(layout.channel_mask),
                                      source.channels),
                          "path=" + path);
    }
    auto permutation_res = validate_wav_permutation(source.channels, layout.output_channel_sources, path);
    if (!permutation_res) {
        return tl::unexpected{permutation_res.error()};
    }
    const auto& permutation = *permutation_res;

    const bool has_axml = !layout.axml.empty();
    const bool has_chna = !layout.chna.empty();
    if (has_axml != has_chna) {
        return make_error(
            ErrorCode::invalid_argument, "ADM WAV finalization requires both AXML and CHNA", "path=" + path);
    }
    auto chna_res = build_chna_payload(source.channels, layout.chna, path);
    if (!chna_res) {
        return tl::unexpected{chna_res.error()};
    }
    const auto& chna_payload = *chna_res;

    const bool extensible = layout.channel_mask != 0U;
    const uint64_t fmt_payload_size = extensible ? 40U : 16U;
    const uint64_t fact_disk_size = source.is_float ? riff_chunk_disk_size(4U) : 0U;
    const uint64_t chna_disk_size = has_chna ? riff_chunk_disk_size(chna_payload.size()) : 0U;
    const uint64_t axml_disk_size = has_axml ? riff_chunk_disk_size(layout.axml.size()) : 0U;
    const uint64_t base_body_size = 4U + riff_chunk_disk_size(fmt_payload_size) + fact_disk_size + chna_disk_size +
                                    riff_chunk_disk_size(source.data.payload_size) + axml_disk_size;
    const bool use_bw64 = has_axml && source.is_pcm;
    const bool preserve_64_bit_container = uses_ds64(source.riff);
    const bool use_64_bit_container =
        use_bw64 || preserve_64_bit_container || base_body_size > std::numeric_limits<uint32_t>::max();
    RiffContainer output_container = RiffContainer::riff;
    if (use_bw64) {
        output_container = RiffContainer::bw64;
    } else if (use_64_bit_container) {
        output_container = RiffContainer::rf64;
    }
    const uint64_t ds64_disk_size = use_64_bit_container ? riff_chunk_disk_size(28U) : 0U;
    const uint64_t riff_size = base_body_size + ds64_disk_size;
    if (output_container == RiffContainer::riff && riff_size > std::numeric_limits<uint32_t>::max()) {
        return make_error(ErrorCode::unsupported, "layout-finalized RIFF WAV exceeds 4GB", "path=" + path);
    }
    if (layout.axml.size() > std::numeric_limits<uint32_t>::max() ||
        chna_payload.size() > std::numeric_limits<uint32_t>::max()) {
        return make_error(ErrorCode::unsupported, "ADM metadata chunk exceeds 4GB", "path=" + path);
    }

    const auto rewritten_path = unique_wav_sidecar_path(path, "layout_tmp");
    if (!rewritten_path.has_value()) {
        return make_error(ErrorCode::io_error, "cannot allocate WAV layout temporary path", "path=" + path);
    }
    TempPathGuard rewritten_guard{*rewritten_path};
    using FilePtr = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    FilePtr input{std::fopen(path.c_str(), "rb"), &std::fclose};
    FilePtr output{std::fopen(rewritten_path->string().c_str(), "wb"), &std::fclose};
    if (!input || !output) {
        return make_error(ErrorCode::io_error, "cannot open WAV layout rewrite streams", "path=" + path);
    }

    std::string_view container_id{"RIFF"};
    if (output_container == RiffContainer::bw64) {
        container_id = "BW64";
    } else if (output_container == RiffContainer::rf64) {
        container_id = "RF64";
    }
    if (!write_fourcc(output.get(), container_id)) {
        return make_error(ErrorCode::io_error, "failed to write WAV container header", "path=" + path);
    }
    write_le32_file(output.get(), use_64_bit_container ? 0xFFFFFFFFU : static_cast<uint32_t>(riff_size));
    if (!write_fourcc(output.get(), "WAVE")) {
        return make_error(ErrorCode::io_error, "failed to write WAV form type", "path=" + path);
    }
    if (use_64_bit_container) {
        if (!write_chunk_header(output.get(), "ds64", 28U)) {
            return make_error(ErrorCode::io_error, "failed to write WAV ds64 header", "path=" + path);
        }
        write_le64_file(output.get(), riff_size);
        write_le64_file(output.get(), source.data.payload_size);
        write_le64_file(output.get(), source.frames);
        write_le32_file(output.get(), 0U);
    }
    if (!write_wave_format(output.get(), source, layout.channel_mask, extensible)) {
        return make_error(ErrorCode::io_error, "failed to write WAV fmt chunk", "path=" + path);
    }
    if (has_chna && !write_payload_chunk(output.get(), "chna", chna_payload.data(), chna_payload.size())) {
        return make_error(ErrorCode::io_error, "failed to write WAV CHNA chunk", "path=" + path);
    }
    if (source.is_float) {
        const uint32_t fact_frames =
            static_cast<uint32_t>(std::min<uint64_t>(source.frames, std::numeric_limits<uint32_t>::max()));
        std::array<uint8_t, 4> fact{static_cast<uint8_t>(fact_frames),
                                    static_cast<uint8_t>(fact_frames >> 8U),
                                    static_cast<uint8_t>(fact_frames >> 16U),
                                    static_cast<uint8_t>(fact_frames >> 24U)};
        if (!write_payload_chunk(output.get(), "fact", fact.data(), fact.size())) {
            return make_error(ErrorCode::io_error, "failed to write WAV fact chunk", "path=" + path);
        }
    }
    if (!write_chunk_header(output.get(),
                            "data",
                            use_64_bit_container ? 0xFFFFFFFFU : static_cast<uint32_t>(source.data.payload_size)) ||
        !file_seek_abs(input.get(), source.data.payload_offset)) {
        return make_error(ErrorCode::io_error, "failed to start WAV sample rewrite", "path=" + path);
    }

    emit_wav_progress(progress, operation, 0.0, 0U, source.frames, "finalizing WAV channel layout");
    const uint64_t bytes_per_sample = source.bits_per_sample / 8U;
    const uint64_t frame_bytes = source.channels * bytes_per_sample;
    constexpr uint64_t k_block_frames = 2048U;
    std::vector<uint8_t> input_buffer(static_cast<std::size_t>(k_block_frames * frame_bytes));
    std::vector<uint8_t> output_buffer(input_buffer.size());
    uint64_t frames_left = source.frames;
    uint64_t frames_done = 0U;
    while (frames_left > 0U) {
        if (cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
        }
        const uint64_t block_frames = std::min(k_block_frames, frames_left);
        const auto block_bytes = static_cast<std::size_t>(block_frames * frame_bytes);
        if (!read_exact(input.get(), input_buffer.data(), block_bytes)) {
            return make_error(ErrorCode::io_error, "short read while finalizing WAV channel layout", "path=" + path);
        }
        for (uint64_t frame = 0; frame < block_frames; ++frame) {
            const auto frame_offset = static_cast<std::size_t>(frame * frame_bytes);
            for (uint32_t output_channel = 0; output_channel < source.channels; ++output_channel) {
                const std::size_t source_offset =
                    frame_offset + (static_cast<std::size_t>(permutation[output_channel]) *
                                    static_cast<std::size_t>(bytes_per_sample));
                const std::size_t destination_offset = frame_offset + (static_cast<std::size_t>(output_channel) *
                                                                       static_cast<std::size_t>(bytes_per_sample));
                std::memcpy(output_buffer.data() + destination_offset,
                            input_buffer.data() + source_offset,
                            static_cast<std::size_t>(bytes_per_sample));
            }
        }
        if (!write_exact(output.get(), output_buffer.data(), block_bytes)) {
            return make_error(ErrorCode::io_error, "short write while finalizing WAV channel layout", "path=" + path);
        }
        frames_left -= block_frames;
        frames_done += block_frames;
        emit_wav_progress(progress,
                          operation,
                          static_cast<double>(frames_done) / static_cast<double>(source.frames),
                          frames_done,
                          source.frames,
                          "finalizing WAV channel layout");
    }
    if ((source.data.payload_size & 1U) != 0U) {
        constexpr uint8_t k_padding = 0;
        if (!write_exact(output.get(), &k_padding, 1U)) {
            return make_error(ErrorCode::io_error, "failed to pad WAV data chunk", "path=" + path);
        }
    }
    if (has_axml && !write_payload_chunk(output.get(), "axml", layout.axml.data(), layout.axml.size())) {
        return make_error(ErrorCode::io_error, "failed to write WAV AXML chunk", "path=" + path);
    }
    if (std::fflush(output.get()) != 0 || std::ferror(output.get()) != 0) {
        return make_error(ErrorCode::io_error, "failed to flush layout-finalized WAV", "path=" + path);
    }
    input.reset();
    output.reset();

    auto replace_res = replace_wav_with_rewrite(path, *rewritten_path, rewritten_guard);
    if (!replace_res) {
        return tl::unexpected{replace_res.error()};
    }
    emit_wav_progress(progress, operation, 1.0, source.frames, source.frames, "WAV channel layout finalized");
    return {};
}

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
