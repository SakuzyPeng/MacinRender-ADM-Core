#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "audio_io_internal.h"

namespace mradm::audio {

// ── FloatCafReader ────────────────────────────────────────────────────────────
// Reads CAF files produced by FloatCafWriter: float32 LE lpcm, known chunk layout.
// Scans all chunks to locate desc + data, tolerates any ordering (e.g. trailing info).

// NOLINTBEGIN(clang-analyzer-unix.Stream)
namespace {

[[nodiscard]] bool caf_read_u16(std::FILE* f, uint16_t& out) {
    std::array<uint8_t, 2> b{};
    if (std::fread(b.data(), 1, b.size(), f) != b.size()) {
        return false;
    }
    out = static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8U) | b[1]);
    return true;
}
[[nodiscard]] bool caf_read_u32(std::FILE* f, uint32_t& out) {
    std::array<uint8_t, 4> b{};
    if (std::fread(b.data(), 1, b.size(), f) != b.size()) {
        return false;
    }
    out = (static_cast<uint32_t>(b[0]) << 24U) | (static_cast<uint32_t>(b[1]) << 16U) |
          (static_cast<uint32_t>(b[2]) << 8U) | b[3];
    return true;
}
[[nodiscard]] bool caf_read_i64(std::FILE* f, int64_t& out) {
    std::array<uint8_t, 8> b{};
    if (std::fread(b.data(), 1, b.size(), f) != b.size()) {
        return false;
    }
    const uint64_t u =
        std::accumulate(b.begin(), b.end(), uint64_t{0}, [](uint64_t acc, uint8_t byte) { return (acc << 8U) | byte; });
    out = static_cast<int64_t>(u);
    return true;
}
[[nodiscard]] bool caf_read_f64(std::FILE* f, double& out) {
    std::array<uint8_t, 8> b{};
    if (std::fread(b.data(), 1, b.size(), f) != b.size()) {
        return false;
    }
    const uint64_t u =
        std::accumulate(b.begin(), b.end(), uint64_t{0}, [](uint64_t acc, uint8_t byte) { return (acc << 8U) | byte; });
    std::memcpy(&out, &u, 8);
    return true;
}


} // namespace
// NOLINTEND(clang-analyzer-unix.Stream)

struct FloatCafReader::Impl {
    std::FILE* file{nullptr};
    uint32_t channels{0};
    uint32_t sample_rate{0};
    uint64_t frame_count{0};
    long data_start{0}; // file offset of first sample (after edit_count)
    uint64_t frames_read{0};
};

// NOLINTBEGIN(clang-analyzer-unix.Stream,readability-function-size)
Result<FloatCafReader> FloatCafReader::open(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return make_error(ErrorCode::io_error, "cannot open CAF file for reading", "path=" + path);
    }

    // File header: "caff" (4) + version u16 (2) + flags u16 (2).
    std::array<char, 4> magic{};
    if (std::fread(magic.data(), 1, magic.size(), f) != magic.size()) {
        std::fclose(f);
        return make_error(ErrorCode::io_error, "truncated CAF file header", "path=" + path);
    }
    if (std::string_view(magic.data(), 4) != "caff") {
        std::fclose(f);
        return make_error(ErrorCode::io_error, "not a CAF file (bad magic)", "path=" + path);
    }
    uint16_t caf_version = 0;
    uint16_t caf_flags = 0;
    if (!caf_read_u16(f, caf_version) || !caf_read_u16(f, caf_flags)) {
        std::fclose(f);
        return make_error(ErrorCode::io_error, "truncated CAF file header", "path=" + path);
    }
    (void) caf_version;
    (void) caf_flags;

    // Scan chunks to find desc and data.
    uint32_t caf_channels = 0;
    uint32_t caf_sample_rate = 0;
    long data_start = -1;
    uint64_t data_bytes = 0;
    bool desc_ok = false;

    while (true) {
        std::array<char, 4> ctype{};
        if (std::fread(ctype.data(), 1, 4, f) != 4) {
            break; // EOF
        }
        int64_t csize = 0;
        if (!caf_read_i64(f, csize)) {
            std::fclose(f);
            return make_error(ErrorCode::io_error, "truncated CAF chunk header", "path=" + path);
        }
        const std::string_view ct(ctype.data(), 4);
        const long payload_start = std::ftell(f);

        if (ct == "desc") {
            // 32-byte payload: f64 sample_rate, FourCC format_id, u32 flags,
            // u32 bytes_per_packet, u32 frames_per_packet, u32 channels, u32 bits.
            double sr = 0.0;
            std::array<char, 4> fmt_id{};
            uint32_t flags = 0;
            uint32_t bytes_per_packet = 0;
            uint32_t fpp = 0;
            uint32_t ch = 0;
            uint32_t bpc = 0;
            if (!caf_read_f64(f, sr) || std::fread(fmt_id.data(), 1, fmt_id.size(), f) != fmt_id.size() ||
                !caf_read_u32(f, flags) || !caf_read_u32(f, bytes_per_packet) || !caf_read_u32(f, fpp) ||
                !caf_read_u32(f, ch) || !caf_read_u32(f, bpc)) {
                std::fclose(f);
                return make_error(ErrorCode::io_error, "truncated CAF desc chunk", "path=" + path);
            }

            constexpr uint32_t k_is_float = 1U;
            constexpr uint32_t k_is_le = 2U;
            const bool valid = std::string_view(fmt_id.data(), 4) == "lpcm" &&
                               (flags & (k_is_float | k_is_le)) == (k_is_float | k_is_le) && fpp == 1U && bpc == 32U;
            if (!valid) {
                std::fclose(f);
                return make_error(
                    ErrorCode::unsupported, "CAF file is not float32 LE lpcm — cannot read back", "path=" + path);
            }
            caf_channels = ch;
            caf_sample_rate = static_cast<uint32_t>(sr);
            desc_ok = true;
            // Seek past any remaining desc payload (in case future versions extend it).
            std::fseek(f, payload_start + static_cast<long>(csize), SEEK_SET);

        } else if (ct == "data") {
            // First 4 bytes of payload are edit_count; samples follow.
            std::fseek(f, 4, SEEK_CUR); // skip edit_count
            data_start = std::ftell(f);

            if (csize == -1) {
                // Unknown size: data extends to EOF.
                std::fseek(f, 0, SEEK_END);
                data_bytes = static_cast<uint64_t>(std::ftell(f) - data_start);
            } else {
                data_bytes = static_cast<uint64_t>(csize) - 4U;
                std::fseek(f, payload_start + static_cast<long>(csize), SEEK_SET);
            }

        } else {
            // Unknown or unneeded chunk (chan, info, …): skip.
            if (csize >= 0) {
                std::fseek(f, payload_start + static_cast<long>(csize), SEEK_SET);
            } else {
                break; // malformed unknown chunk with unknown size — stop
            }
        }
    }

    if (!desc_ok || data_start < 0 || caf_channels == 0) {
        std::fclose(f);
        return make_error(ErrorCode::io_error, "CAF file missing valid desc or data chunk", "path=" + path);
    }

    const uint64_t caf_frame_count = data_bytes / (static_cast<uint64_t>(caf_channels) * sizeof(float));
    std::fseek(f, data_start, SEEK_SET);

    FloatCafReader r;
    r.impl_ = std::make_unique<Impl>();
    r.impl_->file = f;
    r.impl_->channels = caf_channels;
    r.impl_->sample_rate = caf_sample_rate;
    r.impl_->frame_count = caf_frame_count;
    r.impl_->data_start = data_start;
    return r;
}
// NOLINTEND(clang-analyzer-unix.Stream,readability-function-size)

FloatCafReader::~FloatCafReader() {
    if (impl_ && impl_->file != nullptr) {
        std::fclose(impl_->file);
    }
}

FloatCafReader::FloatCafReader(FloatCafReader&&) noexcept = default;
FloatCafReader& FloatCafReader::operator=(FloatCafReader&&) noexcept = default;

uint32_t FloatCafReader::channels() const {
    return impl_->channels;
}
uint32_t FloatCafReader::sample_rate() const {
    return impl_->sample_rate;
}
uint64_t FloatCafReader::frame_count() const {
    return impl_->frame_count;
}

uint64_t FloatCafReader::read(float* out, uint64_t frames) {
    const uint64_t remaining = impl_->frame_count - impl_->frames_read;
    const uint64_t to_read = std::min(frames, remaining);
    if (to_read == 0) {
        return 0;
    }
    const std::size_t samples = static_cast<std::size_t>(to_read) * impl_->channels;
    // Samples are float32 LE — on any LE host (macOS) fread gives correct values directly.
    const std::size_t got = std::fread(out, sizeof(float), samples, impl_->file);
    const uint64_t frames_got = static_cast<uint64_t>(got) / impl_->channels;
    impl_->frames_read += frames_got;
    return frames_got;
}


// ── FloatCafWriter ────────────────────────────────────────────────────────────
// CAF format reference: Apple "Core Audio Format Specification 1.0"
// All chunk headers and metadata are big-endian; PCM samples are little-endian
// float32 (kCAFLinearPCMFormatFlagIsFloat | kCAFLinearPCMFormatFlagIsLittleEndian).

namespace {

// Apple AudioChannelLayoutTag values for our supported output layouts.
// Tag encoding: (tag_constant << 16) | channel_count — public integers, no
// AudioToolbox dependency.  Speaker channel ordering follows BS.2051 / libear
// conventions which align exactly with the Apple tag slot ordering.
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
constexpr std::array<CafLayoutEntry, 8> k_caf_tags = {{
    {"0+2+0",   (101U << 16) | 2U,  2U}, // kAudioChannelLayoutTag_MPEG_2_0  / CICP_2  (L R)
    {"binaural", (106U << 16) | 2U,  2U}, // kAudioChannelLayoutTag_Binaural (BinauralLeft BinauralRight)
    {"0+5+0",  (121U << 16) | 6U,  6U},  // kAudioChannelLayoutTag_MPEG_5_1_A / CICP_6 (L R C LFE Ls Rs)
    {"wav71",  (189U << 16) | 8U,  8U},  // kAudioChannelLayoutTag_WAVE_7_1 (L R C LFE Rls Rrs Ls Rs)
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


// Append a CAF info chunk to an existing CAF file.
// Chunk payload: num_entries (u32 BE) + [key\0value\0]* pairs.
Result<void> write_caf_metadata(const std::string& path, const MetadataFields& meta) {
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (f == nullptr) {
        return make_error(ErrorCode::io_error, "cannot open CAF for metadata write", "path=" + path);
    }

    std::array<char, 4> magic{};
    if (std::fread(magic.data(), 1, magic.size(), f) != magic.size() || std::string_view(magic.data(), 4) != "caff") {
        std::fclose(f);
        return make_error(ErrorCode::io_error, "not a CAF file", "path=" + path);
    }

    // Build key-value pairs.
    struct KV {
        std::string key;
        std::string value;
    };
    std::vector<KV> pairs;

    if (!meta.encoder.empty()) {
        pairs.push_back({"encodingapplication", meta.encoder});
    }
    if (!meta.date_utc.empty()) {
        pairs.push_back({"date", meta.date_utc});
    }

    // Compact comments: renderer, layout, loudness, peak.
    const std::string comments = compact_metadata_comment(meta);
    if (!comments.empty()) {
        pairs.push_back({"comments", comments});
    }

    // Serialise payload: num_entries (u32 BE) + [key\0value\0]*.
    std::vector<uint8_t> payload;
    const auto n = static_cast<uint32_t>(pairs.size());
    payload.push_back((n >> 24) & 0xFF);
    payload.push_back((n >> 16) & 0xFF);
    payload.push_back((n >> 8) & 0xFF);
    payload.push_back(n & 0xFF);
    for (const auto& kv : pairs) {
        std::ranges::transform(kv.key, std::back_inserter(payload), [](char c) { return static_cast<uint8_t>(c); });
        payload.push_back(0);
        std::ranges::transform(kv.value, std::back_inserter(payload), [](char c) { return static_cast<uint8_t>(c); });
        payload.push_back(0);
    }

    // Append info chunk (FourCC + int64 BE size + payload).
    std::fseek(f, 0, SEEK_END);
    std::fwrite("info", 1, 4, f);
    const auto csize = static_cast<uint64_t>(payload.size());
    caf_write_i64(f, static_cast<int64_t>(csize));
    std::fwrite(payload.data(), 1, payload.size(), f);

    std::fclose(f);
    return {};
}

} // namespace mradm::audio
