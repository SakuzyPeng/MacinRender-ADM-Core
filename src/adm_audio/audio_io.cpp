// DR_WAV_IMPLEMENTATION must be defined in exactly one translation unit.
#define DR_WAV_IMPLEMENTATION
#include "adm/audio_io.h"

#include <algorithm>
#include <array>
#include <cctype>
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
#include <numeric>
#include <random>
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

// ── FloatCafReader ────────────────────────────────────────────────────────────
// Reads CAF files produced by FloatCafWriter: float32 LE lpcm, known chunk layout.
// Scans all chunks to locate desc + data, tolerates any ordering (e.g. trailing info).

namespace {

[[nodiscard]] std::filesystem::path unique_sidecar_path(const std::filesystem::path& original_path,
                                                        std::string_view purpose) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;

    const auto parent = original_path.parent_path();
    const auto stem = original_path.stem().string();
    const auto ext = original_path.extension().string();

    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto token = dist(rng);
        auto candidate = parent / fmt::format("{}.{}.{:016x}{}", stem, purpose, token, ext);
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return parent / fmt::format("{}.{}.{:016x}{}", stem, purpose, dist(rng), ext);
}

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

[[nodiscard]] uint32_t read_le32(const char* p) {
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8U) | (static_cast<uint32_t>(b[2]) << 16U) |
           (static_cast<uint32_t>(b[3]) << 24U);
}

} // anonymous namespace

struct FloatCafReader::Impl {
    std::FILE* file{nullptr};
    uint32_t channels{0};
    uint32_t sample_rate{0};
    uint64_t frame_count{0};
    long data_start{0}; // file offset of first sample (after edit_count)
    uint64_t frames_read{0};
};

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
    uint16_t header_word = 0;
    if (!caf_read_u16(f, header_word) || !caf_read_u16(f, header_word)) {
        std::fclose(f);
        return make_error(ErrorCode::io_error, "truncated CAF file header", "path=" + path);
    }

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

// ── ReaderHandle ──────────────────────────────────────────────────────────────

Result<ReaderHandle> ReaderHandle::open(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    if (ext == ".caf") {
        auto res = FloatCafReader::open(path);
        if (!res) {
            return tl::unexpected{res.error()};
        }
        return ReaderHandle{std::move(*res)};
    }
    auto res = FloatWavReader::open(path);
    if (!res) {
        return tl::unexpected{res.error()};
    }
    return ReaderHandle{std::move(*res)};
}

uint32_t ReaderHandle::channels() const {
    return std::visit([](const auto& r) { return r.channels(); }, impl_);
}
uint32_t ReaderHandle::sample_rate() const {
    return std::visit([](const auto& r) { return r.sample_rate(); }, impl_);
}
uint64_t ReaderHandle::frame_count() const {
    return std::visit([](const auto& r) { return r.frame_count(); }, impl_);
}
uint64_t ReaderHandle::read(float* out, uint64_t frames) {
    return std::visit([&](auto& r) { return r.read(out, frames); }, impl_);
}

// ── apply_gain_to_file ────────────────────────────────────────────────────────

Result<void> apply_gain_to_file(const std::string& path, float gain, const std::string& layout_id) {
    if (std::abs(gain - 1.0F) < 1e-6F) {
        return {};
    }

    const std::filesystem::path original_path{path};
    const auto tmp_path = unique_sidecar_path(original_path, "gain_tmp");

    {
        auto reader_res = ReaderHandle::open(path);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        auto& reader = *reader_res;

        const uint32_t num_ch = reader.channels();
        const uint32_t sr = reader.sample_rate();
        const uint64_t total_frames = reader.frame_count();

        auto writer_res = WriterHandle::open(tmp_path.string(), num_ch, sr, layout_id);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        constexpr uint64_t k_block = 4096;
        std::vector<float> buf(static_cast<std::size_t>(num_ch) * k_block);
        uint64_t left = total_frames;

        while (left > 0) {
            const uint64_t n = std::min(k_block, left);
            const uint64_t got = reader.read(buf.data(), n);
            if (got == 0) {
                break;
            }
            const std::size_t samples = static_cast<std::size_t>(num_ch) * static_cast<std::size_t>(got);
            for (std::size_t i = 0; i < samples; ++i) {
                buf[i] *= gain;
            }
            if (writer.write(buf.data(), got) != got) {
                return make_error(
                    ErrorCode::io_error, "short write in apply_gain_to_file", "path=" + tmp_path.string());
            }
            left -= got;
        }
        if (left != 0) {
            return make_error(ErrorCode::io_error, "short read in apply_gain_to_file", "path=" + path);
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, original_path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(original_path, remove_ec);
        ec.clear();
        std::filesystem::rename(tmp_path, original_path, ec);
        if (ec) {
            return make_error(ErrorCode::io_error,
                              "failed to replace output after apply_gain_to_file: " + ec.message(),
                              "path=" + path);
        }
    }
    return {};
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

// ── write_file_metadata ───────────────────────────────────────────────────────

namespace {

// Append a BWF v2 bext chunk to an existing RIFF/WAVE file and update the RIFF
// size.  EBU Tech 3285 supplement 5 field layout (602-byte fixed payload).
Result<void> write_wav_metadata(const std::string& path, const MetadataFields& meta) {
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) {
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

    // Write bext chunk at EOF.
    std::fseek(f, 0, SEEK_END);

    // Null-padded fixed-width string helper.
    const auto write_str_field = [&](const std::string& s, std::size_t width) {
        std::vector<char> buf(width, '\0');
        std::memcpy(buf.data(), s.c_str(), std::min(s.size(), width - 1));
        std::fwrite(buf.data(), 1, width, f);
    };
    const auto write_le16 = [&](int16_t v) {
        const auto u = static_cast<uint16_t>(v);
        const std::array<uint8_t, 2> bytes{static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8U)};
        std::fwrite(bytes.data(), 1, bytes.size(), f);
    };
    const auto write_le32 = [&](uint32_t v) {
        const std::array<uint8_t, 4> bytes{static_cast<uint8_t>(v),
                                           static_cast<uint8_t>(v >> 8U),
                                           static_cast<uint8_t>(v >> 16U),
                                           static_cast<uint8_t>(v >> 24U)};
        std::fwrite(bytes.data(), 1, bytes.size(), f);
    };

    // Chunk header (LE chunk size).
    std::fwrite("bext", 1, 4, f);
    write_le32(k_payload);

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
    write_le32(0);
    write_le32(0);

    // Version = 2 (BWF v2 for loudness fields).
    write_le16(2);

    // UMID[64] — zero.
    std::array<uint8_t, 64> umid{};
    std::fwrite(umid.data(), 1, umid.size(), f);

    // Loudness fields (int16_t LE, unit = 0.01; 0x7FFF = not-indicated).
    constexpr int16_t k_ni = 0x7FFF;
    const int16_t loudness_val = meta.lufs ? static_cast<int16_t>(std::lround(*meta.lufs * 100.0)) : k_ni;
    const int16_t peak_val = meta.peak_dbtp ? static_cast<int16_t>(std::lround(*meta.peak_dbtp * 100.0)) : k_ni;
    write_le16(loudness_val); // LoudnessValue
    write_le16(k_ni);         // LoudnessRange — not measured
    write_le16(peak_val);     // MaxTruePeakLevel
    write_le16(k_ni);         // MaxMomentaryLoudness — not measured
    write_le16(k_ni);         // MaxShortTermLoudness — not measured

    // Reserved[180] — zero.
    std::array<uint8_t, 180> reserved{};
    std::fwrite(reserved.data(), 1, reserved.size(), f);
    // Byte count: 256+32+32+10+8+4+4+2+64+2+2+2+2+2+180 = 602 ✓

    // Update RIFF size (bytes 4-7, LE).
    const uint32_t new_riff_size = riff_size + k_chunk_total;
    std::fseek(f, 4, SEEK_SET);
    write_le32(new_riff_size);

    std::fclose(f);
    return {};
}

// Append a CAF info chunk to an existing CAF file.
// Chunk payload: num_entries (u32 BE) + [key\0value\0]* pairs.
Result<void> write_caf_metadata(const std::string& path, const MetadataFields& meta) {
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) {
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
    std::string comments;
    const auto append = [&](std::string_view kv) {
        if (!comments.empty()) {
            comments += ' ';
        }
        comments.append(kv);
    };
    if (!meta.renderer.empty()) {
        append("renderer=" + meta.renderer);
    }
    if (!meta.output_layout.empty()) {
        append("layout=" + meta.output_layout);
    }
    if (meta.lufs) {
        append(fmt::format("loudness={:.1f}LUFS", *meta.lufs));
    }
    if (meta.peak_dbtp) {
        append(fmt::format("peak={:.2f}dBTP", *meta.peak_dbtp));
    }
    if (!comments.empty()) {
        pairs.push_back({"comments", comments});
    }

    // Serialise payload: num_entries (u32 BE) + [key\0value\0]*.
    std::vector<uint8_t> payload;
    const uint32_t n = static_cast<uint32_t>(pairs.size());
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
    const uint64_t csize = static_cast<uint64_t>(payload.size());
    caf_write_i64(f, static_cast<int64_t>(csize));
    std::fwrite(payload.data(), 1, payload.size(), f);

    std::fclose(f);
    return {};
}

} // anonymous namespace

Result<void> write_file_metadata(const std::string& path, const MetadataFields& meta) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (ext == ".caf") {
        return write_caf_metadata(path, meta);
    }
    if (ext == ".wav") {
        return write_wav_metadata(path, meta);
    }
    return {}; // unsupported extension — silently skip
}

} // namespace mradm::audio
