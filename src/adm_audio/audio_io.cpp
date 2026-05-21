// DR_WAV_IMPLEMENTATION must be defined in exactly one translation unit.
#define DR_WAV_IMPLEMENTATION
#include "adm/audio_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dr_wav.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/errors.h"

namespace mradm::audio {

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

Result<void> downconvert_to_int(const std::string& path, uint16_t bit_depth) {
    try {
        auto reader_res = FloatWavReader::open(path);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        auto& reader = *reader_res;

        const uint32_t channels = reader.channels();
        const uint32_t sample_rate = reader.sample_rate();
        const uint64_t total_frames = reader.frame_count();

        // libbw64 0.10.0 writeFile takes uint16_t sampleRate.
        if (sample_rate > std::numeric_limits<uint16_t>::max()) {
            return make_error(
                ErrorCode::unsupported,
                fmt::format("sample rate {} Hz exceeds integer PCM writer limit (65535 Hz); use --output-bit-depth f32",
                            sample_rate),
                "path=" + path);
        }

        const auto tmp_path = path + ".bitdepth_tmp";
        {
            auto writer = bw64::writeFile(
                tmp_path, static_cast<uint16_t>(channels), static_cast<uint16_t>(sample_rate), bit_depth);

            constexpr uint64_t k_block = 4096;
            std::vector<float> buf(static_cast<std::size_t>(channels) * k_block);
            uint64_t left = total_frames;

            while (left > 0) {
                const uint64_t n = std::min(k_block, left);
                const uint64_t got = reader.read(buf.data(), n);
                if (got == 0) {
                    break;
                }
                writer->write(buf.data(), got);
                left -= got;
            }
        }

        std::filesystem::rename(tmp_path, path);
        return {};

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("bit depth conversion failed: ") + e.what(), "path=" + path);
    }
}

// ── FloatCafWriter ────────────────────────────────────────────────────────────
// CAF format reference: Apple "Core Audio Format Specification 1.0"
// All chunk headers and metadata are big-endian; PCM samples are little-endian
// float32 (kCAFLinearPCMFormatFlagIsFloat | kCAFLinearPCMFormatFlagIsLittleEndian).

namespace {

// Apple AudioChannelLayoutTag values for our supported BS.2051 output layouts.
// Tag encoding: (tag_constant << 16) | channel_count — public integers, no
// AudioToolbox dependency.  Channel ordering follows BS.2051 / libear conventions
// which align exactly with the Apple tag slot ordering for all seven layouts.
//
// NB: BS.2051 uses "LFE1/LFE2" labels while CICP_13 names the same slots
// "LFE2/LFE3".  This is a naming-context difference only; the spatial positions
// and channel slots are identical.
struct CafLayoutEntry {
    const char* layout_id;
    uint32_t tag;
    uint32_t channel_count;
};
// clang-format off
constexpr std::array<CafLayoutEntry, 7> k_caf_tags = {{
    {"0+2+0",  (101U << 16) | 2U,  2U},  // kAudioChannelLayoutTag_MPEG_2_0  / CICP_2  (L R)
    {"0+5+0",  (121U << 16) | 6U,  6U},  // kAudioChannelLayoutTag_MPEG_5_1_A / CICP_6 (L R C LFE Ls Rs)
    {"0+7+0",  (128U << 16) | 8U,  8U},  // kAudioChannelLayoutTag_MPEG_7_1_C / CICP_12
    {"4+5+0",  (195U << 16) | 10U, 10U}, // kAudioChannelLayoutTag_Atmos_5_1_4
    {"4+7+0",  (192U << 16) | 12U, 12U}, // kAudioChannelLayoutTag_Atmos_7_1_4
    {"9.1.6",  (193U << 16) | 16U, 16U}, // kAudioChannelLayoutTag_Atmos_9_1_6
    {"9+10+3", (204U << 16) | 24U, 24U}, // kAudioChannelLayoutTag_CICP_13 (22.2)
}};
// clang-format on

[[nodiscard]] const CafLayoutEntry* caf_layout_entry(std::string_view layout_id) {
    const auto* const it =
        std::ranges::find_if(k_caf_tags, [layout_id](const CafLayoutEntry& e) { return layout_id == e.layout_id; });
    if (it == k_caf_tags.end()) {
        return nullptr;
    }
    return it;
}

template <std::size_t N> void caf_write_bytes(std::FILE* f, const std::array<uint8_t, N>& bytes) {
    std::fwrite(bytes.data(), 1, bytes.size(), f);
}

// Big-endian write helpers (no-dependency, portable).
void caf_write_u16(std::FILE* f, uint16_t v) {
    const std::array<uint8_t, 2> bytes{static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    caf_write_bytes(f, bytes);
}
void caf_write_u32(std::FILE* f, uint32_t v) {
    const std::array<uint8_t, 4> bytes{static_cast<uint8_t>(v >> 24),
                                       static_cast<uint8_t>(v >> 16),
                                       static_cast<uint8_t>(v >> 8),
                                       static_cast<uint8_t>(v)};
    caf_write_bytes(f, bytes);
}
void caf_write_i64(std::FILE* f, int64_t v) {
    auto u = static_cast<uint64_t>(v);
    const std::array<uint8_t, 8> bytes{static_cast<uint8_t>(u >> 56),
                                       static_cast<uint8_t>(u >> 48),
                                       static_cast<uint8_t>(u >> 40),
                                       static_cast<uint8_t>(u >> 32),
                                       static_cast<uint8_t>(u >> 24),
                                       static_cast<uint8_t>(u >> 16),
                                       static_cast<uint8_t>(u >> 8),
                                       static_cast<uint8_t>(u)};
    caf_write_bytes(f, bytes);
}
void caf_write_f64(std::FILE* f, double v) {
    uint64_t u{};
    std::memcpy(&u, &v, 8);
    caf_write_i64(f, static_cast<int64_t>(u));
}
void caf_write_fourcc(std::FILE* f, std::string_view s) {
    std::fwrite(s.data(), 1, 4, f);
}

} // anonymous namespace

struct FloatCafWriter::Impl {
    std::FILE* file{nullptr};
    uint32_t channels{0};
    long data_size_offset{0}; // ftell position of the data chunk's int64 size field
    uint64_t pcm_bytes_written{0};
};

Result<FloatCafWriter>
FloatCafWriter::open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id) {
    const auto* layout = caf_layout_entry(layout_id);
    if (layout == nullptr) {
        return make_error(ErrorCode::unsupported,
                          fmt::format("CAF output: no channel layout tag for '{}'", layout_id),
                          "path=" + path);
    }
    if (channels != layout->channel_count) {
        return make_error(
            ErrorCode::invalid_argument,
            fmt::format(
                "CAF output layout '{}' expects {} channels, got {}", layout_id, layout->channel_count, channels),
            "path=" + path);
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return make_error(ErrorCode::io_error, "failed to open CAF file for writing", "path=" + path);
    }

    // ── 'caff' file header (8 bytes) ─────────────────────────────────────────
    caf_write_fourcc(f, "caff");
    caf_write_u16(f, 1U); // version
    caf_write_u16(f, 0U); // flags

    // ── 'desc' chunk (32-byte payload) ───────────────────────────────────────
    caf_write_fourcc(f, "desc");
    caf_write_i64(f, 32);
    caf_write_f64(f, static_cast<double>(sample_rate));
    caf_write_fourcc(f, "lpcm");
    caf_write_u32(f, 0x3U);          // IsFloat(1) | IsLittleEndian(2)
    caf_write_u32(f, channels * 4U); // bytes per packet
    caf_write_u32(f, 1U);            // frames per packet
    caf_write_u32(f, channels);      // channels per frame
    caf_write_u32(f, 32U);           // bits per channel

    // ── 'chan' chunk (12-byte payload) ───────────────────────────────────────
    caf_write_fourcc(f, "chan");
    caf_write_i64(f, 12);
    caf_write_u32(f, layout->tag); // mChannelLayoutTag
    caf_write_u32(f, 0U);          // mChannelBitmap (unused when tag != UseChannelBitmap)
    caf_write_u32(f, 0U);          // mNumberChannelDescriptions = 0

    // ── 'data' chunk header ───────────────────────────────────────────────────
    // Offset so far: 8 + (4+8+32) + (4+8+12) + 4 = 80
    caf_write_fourcc(f, "data");
    const long size_offset = std::ftell(f); // position of the int64 size field
    caf_write_i64(f, -1);                   // size = unknown; patched on close
    caf_write_u32(f, 0U);                   // edit_count

    FloatCafWriter w;
    w.impl_ = std::make_unique<Impl>();
    w.impl_->file = f;
    w.impl_->channels = channels;
    w.impl_->data_size_offset = size_offset;
    return w;
}

FloatCafWriter::~FloatCafWriter() {
    if (!impl_ || impl_->file == nullptr) {
        return;
    }
    // Patch data chunk size: edit_count field (4 bytes) + PCM bytes.
    const int64_t data_size = 4 + static_cast<int64_t>(impl_->pcm_bytes_written);
    std::fseek(impl_->file, impl_->data_size_offset, SEEK_SET);
    caf_write_i64(impl_->file, data_size);
    std::fclose(impl_->file);
}

FloatCafWriter::FloatCafWriter(FloatCafWriter&&) noexcept = default;
FloatCafWriter& FloatCafWriter::operator=(FloatCafWriter&&) noexcept = default;

uint64_t FloatCafWriter::write(const float* samples, uint64_t frame_count) {
    const uint64_t sample_count = frame_count * impl_->channels;
    const std::size_t written =
        std::fwrite(samples, sizeof(float), static_cast<std::size_t>(sample_count), impl_->file);
    impl_->pcm_bytes_written += written * sizeof(float);
    return written / impl_->channels;
}

// ── WriterHandle ──────────────────────────────────────────────────────────────

Result<WriterHandle>
WriterHandle::open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    if (ext == ".caf") {
        auto res = FloatCafWriter::open(path, channels, sample_rate, layout_id);
        if (!res) {
            return tl::unexpected{res.error()};
        }
        return WriterHandle{std::move(*res)};
    }
    auto res = FloatWavWriter::open(path, channels, sample_rate);
    if (!res) {
        return tl::unexpected{res.error()};
    }
    return WriterHandle{std::move(*res)};
}

uint64_t WriterHandle::write(const float* samples, uint64_t frame_count) {
    return std::visit([&](auto& w) { return w.write(samples, frame_count); }, impl_);
}

} // namespace mradm::audio
