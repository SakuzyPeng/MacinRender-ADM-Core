// DR_WAV_IMPLEMENTATION and DR_FLAC_IMPLEMENTATION must be defined in exactly one TU.
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "adm/audio_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dr_flac.h>
#include <dr_wav.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <opus_multistream.h>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
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

// NOLINTBEGIN(clang-analyzer-unix.Stream)
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
    if (ext == ".flac") {
        auto res = FloatFlacReader::open(path);
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

// ── FloatFlacWriter ───────────────────────────────────────────────────────────
// Converts float32 input to 24-bit signed integers and encodes as FLAC.
// Supports channels 1-8 (FLAC format limit). Compression level 5 (balanced).

constexpr uint32_t k_flac_bits{24};
constexpr float k_flac_scale{8388607.0F}; // 2^23 − 1, maps ±1.0f to ±8388607.

struct FloatFlacWriter::Impl {
    FLAC__StreamEncoder* encoder{nullptr};
    uint32_t channels{};
    std::vector<FLAC__int32> int_buf;
};

Result<FloatFlacWriter> FloatFlacWriter::open(const std::string& path, uint32_t channels, uint32_t sample_rate) {
    if (channels < 1U || channels > 8U) {
        return make_error(
            ErrorCode::unsupported, fmt::format("FLAC supports 1-8 channels, got {}", channels), "path=" + path);
    }

    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (enc == nullptr) {
        return make_error(ErrorCode::io_error, "failed to allocate FLAC encoder", "path=" + path);
    }

    FLAC__stream_encoder_set_channels(enc, channels);
    FLAC__stream_encoder_set_bits_per_sample(enc, k_flac_bits);
    FLAC__stream_encoder_set_sample_rate(enc, sample_rate);
    FLAC__stream_encoder_set_compression_level(enc, 5);

    const FLAC__StreamEncoderInitStatus status = FLAC__stream_encoder_init_file(enc, path.c_str(), nullptr, nullptr);
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(enc);
        return make_error(ErrorCode::io_error,
                          fmt::format("FLAC encoder init failed: {}", FLAC__StreamEncoderInitStatusString[status]),
                          "path=" + path);
    }

    FloatFlacWriter w;
    w.impl_ = std::make_unique<Impl>();
    w.impl_->encoder = enc;
    w.impl_->channels = channels;
    return w;
}

FloatFlacWriter::~FloatFlacWriter() {
    if (impl_ && impl_->encoder != nullptr) {
        FLAC__stream_encoder_finish(impl_->encoder);
        FLAC__stream_encoder_delete(impl_->encoder);
    }
}

FloatFlacWriter::FloatFlacWriter(FloatFlacWriter&&) noexcept = default;
FloatFlacWriter& FloatFlacWriter::operator=(FloatFlacWriter&&) noexcept = default;

uint64_t FloatFlacWriter::write(const float* samples, uint64_t frame_count) {
    const std::size_t total = static_cast<std::size_t>(frame_count) * impl_->channels;
    impl_->int_buf.resize(total);
    for (std::size_t i = 0; i < total; ++i) {
        const float clamped = std::clamp(samples[i], -1.0F, 1.0F);
        impl_->int_buf[i] = static_cast<FLAC__int32>(std::lroundf(clamped * k_flac_scale));
    }
    const FLAC__bool ok = FLAC__stream_encoder_process_interleaved(
        impl_->encoder, impl_->int_buf.data(), static_cast<uint32_t>(frame_count));
    return (ok != 0) ? frame_count : 0U;
}

// ── FloatFlacReader ───────────────────────────────────────────────────────────
// Decodes any integer FLAC file to float32 via dr_flac.

struct FloatFlacReader::Impl {
    drflac* flac{nullptr};
};

Result<FloatFlacReader> FloatFlacReader::open(const std::string& path) {
    // dr_flac owns any internal stream state after a successful open and releases
    // it in drflac_close(); clang's stream analyzer cannot see that handoff.
    // NOLINTNEXTLINE(clang-analyzer-unix.Stream)
    drflac* f = drflac_open_file(path.c_str(), nullptr);
    if (f == nullptr) {
        return make_error(ErrorCode::io_error, "failed to open FLAC file for reading", "path=" + path);
    }
    FloatFlacReader r;
    r.impl_ = std::make_unique<Impl>();
    r.impl_->flac = f;
    return r;
}

FloatFlacReader::~FloatFlacReader() {
    if (impl_ && impl_->flac != nullptr) {
        drflac_close(impl_->flac);
    }
}

FloatFlacReader::FloatFlacReader(FloatFlacReader&&) noexcept = default;
FloatFlacReader& FloatFlacReader::operator=(FloatFlacReader&&) noexcept = default;

uint32_t FloatFlacReader::channels() const {
    return impl_->flac->channels;
}
uint32_t FloatFlacReader::sample_rate() const {
    return impl_->flac->sampleRate;
}
uint64_t FloatFlacReader::frame_count() const {
    return impl_->flac->totalPCMFrameCount;
}
uint64_t FloatFlacReader::read(float* out, uint64_t frames) {
    return drflac_read_pcm_frames_f32(impl_->flac, frames, out);
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
    if (ext == ".flac") {
        auto res = FloatFlacWriter::open(path, channels, sample_rate);
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

// ── FloatOpusMkaWriter ────────────────────────────────────────────────────────
// Minimal hand-written EBML/MKA muxer + libopus encoder.
// Only supports 48000 Hz input (Opus native rate).

// This block is deliberately low-level EBML/libopus C API glue. Keeping the
// Matroska element names close to the spec is more useful than local style churn.
// NOLINTBEGIN(readability-identifier-naming,readability-static-definition-in-anonymous-namespace)
namespace {

// EBML header element IDs
constexpr uint32_t kEbmlId = 0x1A45DFA3U;
constexpr uint32_t kEbmlVersion = 0x4286U;
constexpr uint32_t kEbmlReadVersion = 0x42F7U;
constexpr uint32_t kEbmlMaxIdLen = 0x42F2U;
constexpr uint32_t kEbmlMaxSizeLen = 0x42F3U;
constexpr uint32_t kEbmlDocType = 0x4282U;
constexpr uint32_t kEbmlDocTypeVer = 0x4287U;
constexpr uint32_t kEbmlDocTypeRdVer = 0x4285U;
// Matroska segment-level element IDs
constexpr uint32_t kSegment = 0x18538067U;
constexpr uint32_t kInfo = 0x1549A966U;
constexpr uint32_t kTracks = 0x1654AE6BU;
constexpr uint32_t kCluster = 0x1F43B675U;
constexpr uint32_t kTags = 0x1254C367U;
// Info sub-elements
constexpr uint32_t kTimestampScale = 0x2AD7B1U;
constexpr uint32_t kDuration = 0x4489U;
constexpr uint32_t kMuxingApp = 0x4D80U;
constexpr uint32_t kWritingApp = 0x5741U;
// Track sub-elements
constexpr uint32_t kTrackEntry = 0xAEU;
constexpr uint32_t kTrackNumber = 0xD7U;
constexpr uint32_t kTrackUid = 0x73C5U;
constexpr uint32_t kTrackType = 0x83U;
constexpr uint32_t kFlagEnabled = 0xB9U;
constexpr uint32_t kFlagDefault = 0x88U;
constexpr uint32_t kFlagForced = 0x55AAU;
constexpr uint32_t kCodecId = 0x86U;
constexpr uint32_t kCodecPrivate = 0x63A2U;
constexpr uint32_t kCodecDelay = 0x56AAU;
constexpr uint32_t kSeekPreRoll = 0x56BBU;
constexpr uint32_t kDefaultDuration = 0x23E383U;
constexpr uint32_t kAudio = 0xE1U;
constexpr uint32_t kSamplingFreq = 0xB5U;
constexpr uint32_t kChannels = 0x9FU;
// Cluster sub-elements
constexpr uint32_t kTimestamp = 0xE7U;
constexpr uint32_t kSimpleBlock = 0xA3U;
// Tags sub-elements
constexpr uint32_t kTag = 0x7373U;
constexpr uint32_t kTargets = 0x63C0U;
constexpr uint32_t kSimpleTag = 0x67C8U;
constexpr uint32_t kTagName = 0x45A3U;
constexpr uint32_t kTagString = 0x4487U;

using MkaBuf = std::vector<uint8_t>;

static void mka_write_id(MkaBuf& b, uint32_t id) {
    if (id <= 0xFFU) {
        b.push_back(static_cast<uint8_t>(id));
    } else if (id <= 0xFFFFU) {
        b.push_back(static_cast<uint8_t>(id >> 8U));
        b.push_back(static_cast<uint8_t>(id));
    } else if (id <= 0xFFFFFFU) {
        b.push_back(static_cast<uint8_t>(id >> 16U));
        b.push_back(static_cast<uint8_t>(id >> 8U));
        b.push_back(static_cast<uint8_t>(id));
    } else {
        b.push_back(static_cast<uint8_t>(id >> 24U));
        b.push_back(static_cast<uint8_t>(id >> 16U));
        b.push_back(static_cast<uint8_t>(id >> 8U));
        b.push_back(static_cast<uint8_t>(id));
    }
}

static void mka_write_vint(MkaBuf& b, uint64_t v) {
    if (v < 0x7FULL) {
        b.push_back(static_cast<uint8_t>(v | 0x80U));
    } else if (v < 0x3FFFULL) {
        b.push_back(static_cast<uint8_t>((v >> 8U) | 0x40U));
        b.push_back(static_cast<uint8_t>(v));
    } else if (v < 0x1FFFFFULL) {
        b.push_back(static_cast<uint8_t>((v >> 16U) | 0x20U));
        b.push_back(static_cast<uint8_t>(v >> 8U));
        b.push_back(static_cast<uint8_t>(v));
    } else if (v < 0x0FFFFFFFULL) {
        b.push_back(static_cast<uint8_t>((v >> 24U) | 0x10U));
        b.push_back(static_cast<uint8_t>(v >> 16U));
        b.push_back(static_cast<uint8_t>(v >> 8U));
        b.push_back(static_cast<uint8_t>(v));
    } else if (v < 0x07FFFFFFFFULL) {
        b.push_back(static_cast<uint8_t>((v >> 32U) | 0x08U));
        b.push_back(static_cast<uint8_t>(v >> 24U));
        b.push_back(static_cast<uint8_t>(v >> 16U));
        b.push_back(static_cast<uint8_t>(v >> 8U));
        b.push_back(static_cast<uint8_t>(v));
    } else {
        b.push_back(0x01U);
        for (int i = 7; i >= 0; --i) {
            b.push_back(static_cast<uint8_t>(v >> (static_cast<unsigned>(i) * 8U)));
        }
    }
}

static void mka_write_unknown_size(MkaBuf& b) {
    b.push_back(0x01U);
    for (int i = 0; i < 7; ++i) {
        b.push_back(0xFFU);
    }
}

static void mka_write_uint_be(MkaBuf& b, uint64_t v, int width) {
    for (int i = width - 1; i >= 0; --i) {
        b.push_back(static_cast<uint8_t>(v >> (static_cast<unsigned>(i) * 8U)));
    }
}

static int mka_uint_width(uint64_t v) {
    if (v == 0) {
        return 1;
    }
    int w = 0;
    while (v > 0) {
        v >>= 8U;
        ++w;
    }
    return w;
}

static void mka_write_float64_be(MkaBuf& b, double v) {
    uint64_t bits{};
    std::memcpy(&bits, &v, 8);
    mka_write_uint_be(b, bits, 8);
}

static void mka_write_float32_be(MkaBuf& b, float v) {
    uint32_t bits{};
    std::memcpy(&bits, &v, 4);
    mka_write_uint_be(b, static_cast<uint64_t>(bits), 4);
}

static void mka_elem(MkaBuf& dst, uint32_t id, const MkaBuf& content) {
    mka_write_id(dst, id);
    mka_write_vint(dst, content.size());
    dst.insert(dst.end(), content.begin(), content.end());
}

static void mka_elem_uint(MkaBuf& dst, uint32_t id, uint64_t value) {
    MkaBuf content;
    int w = mka_uint_width(value);
    mka_write_uint_be(content, value, w);
    mka_elem(dst, id, content);
}

static void mka_elem_float64(MkaBuf& dst, uint32_t id, double value) {
    MkaBuf content;
    mka_write_float64_be(content, value);
    mka_elem(dst, id, content);
}

// mka_elem_float32 is defined but only used in build_opus_head indirectly;
// suppress unused warning by wrapping in conditional usage
[[maybe_unused]] static void mka_elem_float32(MkaBuf& dst, uint32_t id, float value) {
    MkaBuf content;
    mka_write_float32_be(content, value);
    mka_elem(dst, id, content);
}

static void mka_elem_str(MkaBuf& dst, uint32_t id, const std::string& s) {
    MkaBuf content(s.begin(), s.end());
    mka_elem(dst, id, content);
}

static bool mka_write_file(FILE* f, const MkaBuf& b) {
    return b.empty() || std::fwrite(b.data(), 1, b.size(), f) == b.size();
}

static std::vector<uint8_t> build_opus_head(uint32_t channels,
                                            int nb_streams,
                                            int nb_coupled,
                                            const uint8_t* mapping,
                                            uint16_t preskip,
                                            uint32_t input_sample_rate,
                                            uint8_t family) {
    std::vector<uint8_t> h;
    const std::string_view magic = "OpusHead";
    h.insert(h.end(), magic.begin(), magic.end());
    h.push_back(1U); // version
    h.push_back(static_cast<uint8_t>(channels));
    h.push_back(static_cast<uint8_t>(preskip & 0xFFU));
    h.push_back(static_cast<uint8_t>(preskip >> 8U));
    h.push_back(static_cast<uint8_t>(input_sample_rate));
    h.push_back(static_cast<uint8_t>(input_sample_rate >> 8U));
    h.push_back(static_cast<uint8_t>(input_sample_rate >> 16U));
    h.push_back(static_cast<uint8_t>(input_sample_rate >> 24U));
    h.push_back(0U);
    h.push_back(0U); // output gain = 0
    h.push_back(family);
    if (family != 0U) {
        h.push_back(static_cast<uint8_t>(nb_streams));
        h.push_back(static_cast<uint8_t>(nb_coupled));
        for (uint32_t i = 0; i < channels; ++i) {
            h.push_back(mapping[i]);
        }
    }
    return h;
}

} // namespace
// NOLINTEND(readability-identifier-naming,readability-static-definition-in-anonymous-namespace)

// NOLINTBEGIN(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-pro-type-vararg,readability-function-size)
struct FloatOpusMkaWriter::Impl {
    FILE* file{nullptr};
    OpusMSEncoder* encoder{nullptr};
    int nb_streams{0};
    int nb_coupled{0};
    uint8_t mapping[255]{};
    uint32_t channels{0};
    int32_t preskip{0};

    long duration_patch_offset{-1};
    uint64_t cluster_ts_ms{0};
    uint64_t total_frames{0};
    bool cluster_open{false};
    bool failed{false};
    std::string path;
    std::string failure_message;

    static constexpr int k_frame_size = 960;
    std::vector<float> ibuf;
    std::vector<uint8_t> pbuf;

    bool open_cluster(uint64_t ts_ms);
    bool encode_and_write(uint64_t frame_ts_ms);
    Result<void> close();
    ~Impl();
};

FloatOpusMkaWriter::Impl::~Impl() {
    (void) close();
}

Result<void> FloatOpusMkaWriter::Impl::close() {
    if (encoder == nullptr && file == nullptr) {
        if (failed) {
            return make_error(ErrorCode::io_error, failure_message, "path=" + path);
        }
        return {};
    }

    if (!ibuf.empty() && file != nullptr) {
        const std::size_t missing = (static_cast<std::size_t>(k_frame_size) * channels) - ibuf.size();
        ibuf.insert(ibuf.end(), missing, 0.0F);
        const uint64_t frame_ms = (total_frames * static_cast<uint64_t>(k_frame_size) * 1000U) / 48000U;
        if (encode_and_write(frame_ms)) {
            ++total_frames;
        }
    }
    if (file != nullptr && duration_patch_offset >= 0) {
        const double duration_ms =
            static_cast<double>(total_frames) * (static_cast<double>(k_frame_size) / 48000.0) * 1000.0;
        MkaBuf tmp;
        mka_write_float64_be(tmp, duration_ms);
        if (std::fseek(file, duration_patch_offset, SEEK_SET) != 0 ||
            std::fwrite(tmp.data(), 1, tmp.size(), file) != tmp.size()) {
            failed = true;
            failure_message = "failed to patch MKA duration";
        }
        if (std::fclose(file) != 0 && !failed) {
            failed = true;
            failure_message = "failed to close MKA file";
        }
        file = nullptr;
    }
    if (encoder != nullptr) {
        opus_multistream_encoder_destroy(encoder);
        encoder = nullptr;
    }
    if (failed) {
        return make_error(ErrorCode::io_error, failure_message, "path=" + path);
    }
    return {};
}

bool FloatOpusMkaWriter::Impl::open_cluster(uint64_t ts_ms) {
    cluster_ts_ms = ts_ms;
    cluster_open = true;
    MkaBuf hdr;
    mka_write_id(hdr, kCluster);
    mka_write_unknown_size(hdr);
    if (!mka_write_file(file, hdr)) {
        failed = true;
        failure_message = "failed to write MKA cluster header";
        return false;
    }
    MkaBuf tb;
    mka_elem_uint(tb, kTimestamp, ts_ms);
    if (!mka_write_file(file, tb)) {
        failed = true;
        failure_message = "failed to write MKA cluster timestamp";
        return false;
    }
    return true;
}

bool FloatOpusMkaWriter::Impl::encode_and_write(uint64_t frame_ts_ms) {
    if (failed) {
        return false;
    }
    if (!cluster_open || frame_ts_ms - cluster_ts_ms > 5000U) {
        if (!open_cluster(frame_ts_ms)) {
            return false;
        }
    }
    const int ret = opus_multistream_encode_float(
        encoder, ibuf.data(), k_frame_size, pbuf.data(), static_cast<opus_int32>(pbuf.size()));
    if (ret <= 0) {
        failed = true;
        failure_message = fmt::format("opus_multistream_encode_float failed: {}", opus_strerror(ret));
        return false;
    }

    const auto rel_ms = static_cast<int16_t>(static_cast<int64_t>(frame_ts_ms) - static_cast<int64_t>(cluster_ts_ms));

    MkaBuf sb;
    sb.push_back(0x81U); // vint(1) — track number 1
    sb.push_back(static_cast<uint8_t>(static_cast<uint16_t>(rel_ms) >> 8U));
    sb.push_back(static_cast<uint8_t>(static_cast<uint16_t>(rel_ms)));
    sb.push_back(0x80U); // keyframe flag
    sb.insert(sb.end(), pbuf.data(), pbuf.data() + ret);

    MkaBuf block_elem;
    mka_elem(block_elem, kSimpleBlock, sb);
    if (!mka_write_file(file, block_elem)) {
        failed = true;
        failure_message = "failed to write MKA SimpleBlock";
        return false;
    }

    ibuf.clear();
    return true;
}

Result<FloatOpusMkaWriter> FloatOpusMkaWriter::open(const std::string& path,
                                                    uint32_t channels,
                                                    uint32_t sample_rate,
                                                    uint32_t bitrate_per_ch_kbps) {
    if (sample_rate != 48000U) {
        return make_error(ErrorCode::unsupported,
                          fmt::format("Opus MKA requires 48000 Hz input, got {} Hz", sample_rate),
                          "path=" + path);
    }
    if (channels < 1U || channels > 255U) {
        return make_error(
            ErrorCode::unsupported, fmt::format("Opus supports 1-255 channels, got {}", channels), "path=" + path);
    }

    auto impl = std::make_unique<Impl>();
    impl->channels = channels;
    impl->path = path;

    int family = 255;
    if (channels <= 2U) {
        family = 0;
    } else if (channels <= 8U) {
        family = 1;
    }

    int error{};
    impl->encoder = opus_multistream_surround_encoder_create(48000,
                                                             static_cast<int>(channels),
                                                             family,
                                                             &impl->nb_streams,
                                                             &impl->nb_coupled,
                                                             impl->mapping,
                                                             OPUS_APPLICATION_AUDIO,
                                                             &error);
    if (impl->encoder == nullptr || error != OPUS_OK) {
        return make_error(ErrorCode::io_error,
                          fmt::format("opus_multistream_surround_encoder_create failed: {}", opus_strerror(error)),
                          "path=" + path);
    }

    if (bitrate_per_ch_kbps > 0U && (bitrate_per_ch_kbps < 6U || bitrate_per_ch_kbps > 320U)) {
        return make_error(
            ErrorCode::invalid_argument,
            fmt::format("opus_bitrate_per_ch_kbps must be 0 (auto) or 6-320, got {}", bitrate_per_ch_kbps),
            "path=" + path);
    }
    uint64_t total_bps = static_cast<uint64_t>(channels) * 64000U;
    if (bitrate_per_ch_kbps > 0U) {
        total_bps = static_cast<uint64_t>(bitrate_per_ch_kbps) * channels * 1000U;
    } else if (channels <= 2U) {
        total_bps = 128000U;
    }
    const auto bitrate = static_cast<int32_t>(total_bps);
    const auto opus_ctl_error = [&](int ret, std::string_view op) -> Result<void> {
        if (ret == OPUS_OK) {
            return {};
        }
        return make_error(ErrorCode::io_error, fmt::format("{} failed: {}", op, opus_strerror(ret)), "path=" + path);
    };
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#endif
    if (auto ret = opus_ctl_error(opus_multistream_encoder_ctl(impl->encoder, OPUS_SET_VBR(1)), "OPUS_SET_VBR"); !ret) {
        return tl::unexpected{ret.error()};
    }
    if (auto ret = opus_ctl_error(opus_multistream_encoder_ctl(impl->encoder, OPUS_SET_VBR_CONSTRAINT(0)),
                                  "OPUS_SET_VBR_CONSTRAINT");
        !ret) {
        return tl::unexpected{ret.error()};
    }
    if (auto ret =
            opus_ctl_error(opus_multistream_encoder_ctl(impl->encoder, OPUS_SET_BITRATE(bitrate)), "OPUS_SET_BITRATE");
        !ret) {
        return tl::unexpected{ret.error()};
    }

    int32_t preskip{};
    if (auto ret = opus_ctl_error(opus_multistream_encoder_ctl(impl->encoder, OPUS_GET_LOOKAHEAD(&preskip)),
                                  "OPUS_GET_LOOKAHEAD");
        !ret) {
        return tl::unexpected{ret.error()};
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    impl->preskip = preskip;

    impl->pbuf.resize(static_cast<std::size_t>(impl->nb_streams) * 1275);

    const auto opus_head = build_opus_head(channels,
                                           impl->nb_streams,
                                           impl->nb_coupled,
                                           impl->mapping,
                                           static_cast<uint16_t>(preskip),
                                           48000U,
                                           static_cast<uint8_t>(family));

    std::random_device rd;
    std::mt19937_64 rng(rd());
    const uint64_t track_uid = rng();

    // ── Build EBML header ─────────────────────────────────────────────────────
    MkaBuf ebml;
    {
        MkaBuf hdr;
        mka_elem_uint(hdr, kEbmlVersion, 1U);
        mka_elem_uint(hdr, kEbmlReadVersion, 1U);
        mka_elem_uint(hdr, kEbmlMaxIdLen, 4U);
        mka_elem_uint(hdr, kEbmlMaxSizeLen, 8U);
        mka_elem_str(hdr, kEbmlDocType, "matroska");
        mka_elem_uint(hdr, kEbmlDocTypeVer, 4U);
        mka_elem_uint(hdr, kEbmlDocTypeRdVer, 2U);
        mka_elem(ebml, kEbmlId, hdr);
    }

    // ── Segment (unknown size) ────────────────────────────────────────────────
    {
        MkaBuf seg_hdr;
        mka_write_id(seg_hdr, kSegment);
        mka_write_unknown_size(seg_hdr);
        ebml.insert(ebml.end(), seg_hdr.begin(), seg_hdr.end());
    }

    // ── Info ─────────────────────────────────────────────────────────────────
    {
        // Build Info content first to compute sizes for duration patch offset.
        MkaBuf info;

        // TimestampScale: ID(kTimestampScale=0x2AD7B1, 3 bytes) + vint(3) + value(3) = 7 bytes
        mka_elem_uint(info, kTimestampScale, 1000000U);

        // Duration element: write directly so we know exact byte layout.
        // ID(kDuration=0x4489, 2 bytes) + vint(8, 1 byte) + value(8 bytes) = 11 bytes
        MkaBuf dur_elem;
        mka_write_id(dur_elem, kDuration);
        mka_write_vint(dur_elem, 8U);
        mka_write_float64_be(dur_elem, 0.0);
        info.insert(info.end(), dur_elem.begin(), dur_elem.end());

        mka_elem_str(info, kMuxingApp, "MacinRender ADM Core");
        mka_elem_str(info, kWritingApp, "MacinRender ADM Core");

        // Compute patch offset:
        // ebml.size() = start of Info element in buffer
        // Info element header = ID(kInfo=0x1549A966, 4 bytes) + vint(info.size())
        const std::size_t info_elem_start = ebml.size();
        constexpr std::size_t k_info_id_bytes = 4U; // kInfo is 4-byte ID
        const std::size_t info_content_size = info.size();
        std::size_t sz_bytes = 4U;
        if (info_content_size < 0x7FULL) {
            sz_bytes = 1U;
        } else if (info_content_size < 0x3FFFULL) {
            sz_bytes = 2U;
        } else if (info_content_size < 0x1FFFFFULL) {
            sz_bytes = 3U;
        }

        // TimestampScale element is 7 bytes (3+1+3), Duration header is 3 bytes (2+1),
        // Duration value starts 7+3=10 bytes into info content.
        // Patch offset = info_elem_start + ID(4) + vint_size(sz_bytes) + 10
        impl->duration_patch_offset = static_cast<long>(info_elem_start + k_info_id_bytes + sz_bytes + 10U);

        mka_elem(ebml, kInfo, info);
    }

    // ── Tracks ────────────────────────────────────────────────────────────────
    {
        MkaBuf entry;
        mka_elem_uint(entry, kTrackNumber, 1U);
        mka_elem_uint(entry, kTrackUid, track_uid);
        mka_elem_uint(entry, kTrackType, 2U); // audio
        mka_elem_uint(entry, kFlagEnabled, 1U);
        mka_elem_uint(entry, kFlagDefault, 1U);
        mka_elem_uint(entry, kFlagForced, 0U);
        mka_elem_str(entry, kCodecId, "A_OPUS");
        {
            MkaBuf cp(opus_head.begin(), opus_head.end());
            mka_elem(entry, kCodecPrivate, cp);
        }
        const uint64_t codec_delay_ns = (static_cast<uint64_t>(preskip) * 1000000000ULL) / 48000ULL;
        mka_elem_uint(entry, kCodecDelay, codec_delay_ns);
        mka_elem_uint(entry, kSeekPreRoll, 80000000ULL);
        mka_elem_uint(entry, kDefaultDuration, 20000000ULL);
        {
            MkaBuf audio;
            mka_elem_float64(audio, kSamplingFreq, 48000.0);
            mka_elem_uint(audio, kChannels, static_cast<uint64_t>(channels));
            mka_elem(entry, kAudio, audio);
        }
        MkaBuf tracks;
        mka_elem(tracks, kTrackEntry, entry);
        mka_elem(ebml, kTracks, tracks);
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return make_error(ErrorCode::io_error, "failed to open MKA file for writing", "path=" + path);
    }
    if (!mka_write_file(f, ebml)) {
        std::fclose(f);
        return make_error(ErrorCode::io_error, "failed to write MKA header", "path=" + path);
    }

    impl->file = f;
    if (!impl->open_cluster(0U)) {
        auto err = impl->close();
        if (!err) {
            return tl::unexpected{err.error()};
        }
        return make_error(ErrorCode::io_error, "failed to write initial MKA cluster", "path=" + path);
    }

    FloatOpusMkaWriter w;
    w.impl_ = std::move(impl);
    return w;
}

uint64_t FloatOpusMkaWriter::write(const float* samples, uint64_t frame_count) {
    if (!impl_ || impl_->failed) {
        return 0;
    }
    uint64_t frames_consumed = 0;
    const std::size_t ch = impl_->channels;
    while (frames_consumed < frame_count) {
        const std::size_t needed = (static_cast<std::size_t>(Impl::k_frame_size) * ch) - impl_->ibuf.size();
        const std::size_t avail = static_cast<std::size_t>(frame_count - frames_consumed) * ch;
        const std::size_t take = std::min(needed, avail);
        impl_->ibuf.insert(impl_->ibuf.end(), samples, samples + take);
        samples += take;
        frames_consumed += take / ch;

        if (impl_->ibuf.size() == static_cast<std::size_t>(Impl::k_frame_size) * ch) {
            const uint64_t frame_ms =
                (impl_->total_frames * static_cast<uint64_t>(Impl::k_frame_size) * 1000U) / 48000U;
            if (!impl_->encode_and_write(frame_ms)) {
                return frames_consumed;
            }
            ++impl_->total_frames;
        }
    }
    return frame_count;
}

FloatOpusMkaWriter::~FloatOpusMkaWriter() = default;
FloatOpusMkaWriter::FloatOpusMkaWriter(FloatOpusMkaWriter&&) noexcept = default;
FloatOpusMkaWriter& FloatOpusMkaWriter::operator=(FloatOpusMkaWriter&&) noexcept = default;

Result<void> FloatOpusMkaWriter::close() {
    if (!impl_) {
        return {};
    }
    return impl_->close();
}

Result<void> convert_to_opus_mka(const std::string& src_path,
                                 const std::string& mka_path,
                                 const std::string& layout_id,
                                 uint32_t bitrate_per_ch_kbps) {
    auto reader_res = FloatWavReader::open(src_path);
    if (!reader_res) {
        return tl::unexpected{reader_res.error()};
    }
    auto& reader = *reader_res;

    if (reader.sample_rate() != 48000U) {
        return make_error(ErrorCode::unsupported,
                          fmt::format("Opus MKA requires 48000 Hz source, got {} Hz", reader.sample_rate()),
                          "src=" + src_path);
    }

    auto writer_res = FloatOpusMkaWriter::open(mka_path, reader.channels(), reader.sample_rate(), bitrate_per_ch_kbps);
    if (!writer_res) {
        return tl::unexpected{writer_res.error()};
    }
    auto& writer = *writer_res;

    constexpr uint64_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(reader.channels()) * k_block);
    uint64_t left = reader.frame_count();
    while (left > 0) {
        const uint64_t n = std::min(k_block, left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        if (writer.write(buf.data(), got) != got) {
            return make_error(ErrorCode::io_error, "short write in convert_to_opus_mka", "path=" + mka_path);
        }
        left -= got;
    }
    if (left != 0) {
        return make_error(ErrorCode::io_error, "short read in convert_to_opus_mka", "src=" + src_path);
    }
    (void) layout_id;
    return writer.close();
}
// NOLINTEND(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-pro-type-vararg,readability-function-size)

// ── write_file_metadata ───────────────────────────────────────────────────────

// NOLINTBEGIN(readability-static-definition-in-anonymous-namespace)
namespace {

std::optional<std::size_t> find_mka_unknown_size_cluster(const std::vector<uint8_t>& data) {
    MkaBuf needle;
    mka_write_id(needle, kCluster);
    mka_write_unknown_size(needle);
    // NOLINTNEXTLINE(modernize-use-ranges)
    const auto it = std::search(data.begin(), data.end(), needle.begin(), needle.end());
    if (it == data.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(data.begin(), it));
}

// write_mka_metadata: insert Tags before the first Cluster. Appending after an
// unknown-size Cluster is commonly ignored by Matroska parsers.
Result<void> write_mka_metadata(const std::string& path, const MetadataFields& meta) {
    const auto make_simple_tag = [](const std::string& name, const std::string& value) {
        MkaBuf st;
        mka_elem_str(st, kTagName, name);
        mka_elem_str(st, kTagString, value);
        MkaBuf elem;
        mka_elem(elem, kSimpleTag, st);
        return elem;
    };

    MkaBuf tag_content;
    MkaBuf targets;
    // NOLINTNEXTLINE(readability-suspicious-call-argument)
    mka_elem(tag_content, kTargets, targets);

    const auto append_simple = [&](const std::string& name, const std::string& value) {
        auto st = make_simple_tag(name, value);
        tag_content.insert(tag_content.end(), st.begin(), st.end());
    };

    if (!meta.encoder.empty()) {
        append_simple("ENCODER", meta.encoder);
    }
    if (!meta.date_utc.empty()) {
        append_simple("DATE_RECORDED", meta.date_utc);
    }
    if (!meta.renderer.empty()) {
        append_simple("RENDERER", meta.renderer);
    }
    if (!meta.output_layout.empty()) {
        append_simple("OUTPUT_LAYOUT", meta.output_layout);
    }
    if (meta.lufs) {
        append_simple("INTEGRATED_LOUDNESS", fmt::format("{:.1f}", *meta.lufs));
    }
    if (meta.peak_dbtp) {
        append_simple("TRUE_PEAK", fmt::format("{:.2f}", *meta.peak_dbtp));
    }

    MkaBuf tag_elem;
    mka_elem(tag_elem, kTag, tag_content);

    MkaBuf tags;
    mka_elem(tags, kTags, tag_elem);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return make_error(ErrorCode::io_error, "cannot open MKA for metadata read", "path=" + path);
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return make_error(ErrorCode::io_error, "failed to read MKA for metadata update", "path=" + path);
    }

    const auto cluster_pos = find_mka_unknown_size_cluster(data);
    if (!cluster_pos) {
        return make_error(ErrorCode::io_error, "cannot locate MKA Cluster for metadata insertion", "path=" + path);
    }

    std::vector<uint8_t> out;
    out.reserve(data.size() + tags.size());
    out.insert(out.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(*cluster_pos));
    out.insert(out.end(), tags.begin(), tags.end());
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(*cluster_pos), data.end());

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return make_error(ErrorCode::io_error, "cannot open MKA for metadata rewrite", "path=" + path);
    }
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) {
        return make_error(ErrorCode::io_error, "failed to rewrite MKA metadata", "path=" + path);
    }
    return {};
}

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

// Insert Vorbis Comment block into an existing FLAC file using the libFLAC
// metadata chain API. Recognised output layouts get a WAVEFORMATEXTENSIBLE_CHANNEL_MASK
// tag so that standard players (ffmpeg, foobar2000) can identify channel positions.
// NOLINTNEXTLINE(readability-function-size)
Result<void> write_flac_metadata(const std::string& path, const MetadataFields& meta) {
    FLAC__Metadata_Chain* chain = FLAC__metadata_chain_new();
    if (chain == nullptr) {
        return make_error(ErrorCode::io_error, "FLAC metadata chain alloc failed", "path=" + path);
    }

    if (FLAC__metadata_chain_read(chain, path.c_str()) == 0) {
        FLAC__metadata_chain_delete(chain);
        return make_error(ErrorCode::io_error, "FLAC metadata chain read failed", "path=" + path);
    }

    FLAC__StreamMetadata* vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    if (vc == nullptr) {
        FLAC__metadata_chain_delete(chain);
        return make_error(ErrorCode::io_error, "FLAC Vorbis Comment alloc failed", "path=" + path);
    }

    // append_tag returns false on OOM (FLAC__bool is int).
    const auto append_tag = [&](const std::string& tag) -> bool {
        FLAC__StreamMetadata_VorbisComment_Entry entry{};
        entry.length = static_cast<FLAC__uint32>(tag.size());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        entry.entry = reinterpret_cast<FLAC__byte*>(const_cast<char*>(tag.c_str()));
        return FLAC__metadata_object_vorbiscomment_append_comment(vc, entry, /*copy=*/true) != 0;
    };

    bool tags_ok = true;
    if (!meta.encoder.empty()) {
        tags_ok &= append_tag("ENCODER=" + meta.encoder);
    }
    if (!meta.date_utc.empty()) {
        tags_ok &= append_tag("DATE=" + meta.date_utc);
    }
    if (!meta.renderer.empty() || !meta.output_layout.empty()) {
        std::string comment = "COMMENT=renderer=" + meta.renderer + " layout=" + meta.output_layout;
        if (meta.lufs) {
            comment += fmt::format(" loudness={:.1f}LUFS", *meta.lufs);
        }
        if (meta.peak_dbtp) {
            comment += fmt::format(" peak={:.2f}dBTP", *meta.peak_dbtp);
        }
        tags_ok &= append_tag(comment);
    }

    // WAVEFORMATEXTENSIBLE_CHANNEL_MASK — ffmpeg convention for multi-channel FLAC.
    // Masks for layouts where FLAC channel count ≤ 8 applies.
    constexpr std::array<std::pair<std::string_view, uint32_t>, 3> k_masks{{
        {"0+2+0", 0x0003U}, // FL FR
        {"0+5+0", 0x003FU}, // FL FR FC LFE BL BR
        {"wav71", 0x063FU}, // FL FR FC LFE BL BR SL SR (WAVE_7_1)
    }};
    for (const auto& [id, mask] : k_masks) {
        if (meta.output_layout == id) {
            tags_ok &= append_tag(fmt::format("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x{:08X}", mask));
            break;
        }
    }

    if (!tags_ok) {
        FLAC__metadata_object_delete(vc);
        FLAC__metadata_chain_delete(chain);
        return make_error(ErrorCode::io_error, "FLAC Vorbis Comment entry append failed (OOM?)", "path=" + path);
    }

    FLAC__Metadata_Iterator* it = FLAC__metadata_iterator_new();
    if (it == nullptr) {
        FLAC__metadata_object_delete(vc);
        FLAC__metadata_chain_delete(chain);
        return make_error(ErrorCode::io_error, "FLAC metadata iterator alloc failed", "path=" + path);
    }
    FLAC__metadata_iterator_init(it, chain);

    // Replace the existing Vorbis Comment block (encoder writes an empty one by
    // default) to avoid duplicate VC blocks, which violates the FLAC spec.
    // On failure, vc is NOT owned by the chain — caller must delete it.
    bool replaced = false;
    bool keep_scanning = true;
    while (keep_scanning) {
        if (FLAC__metadata_iterator_get_block_type(it) == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            if (FLAC__metadata_iterator_set_block(it, vc) == 0) {
                FLAC__metadata_object_delete(vc);
                FLAC__metadata_iterator_delete(it);
                FLAC__metadata_chain_delete(chain);
                return make_error(ErrorCode::io_error, "FLAC VC block replace failed", "path=" + path);
            }
            replaced = true;
            break;
        }
        keep_scanning = FLAC__metadata_iterator_next(it) != 0;
    }

    if (!replaced) {
        // No existing VC block — insert after STREAMINFO (first block).
        FLAC__metadata_iterator_init(it, chain);
        if (FLAC__metadata_iterator_insert_block_after(it, vc) == 0) {
            FLAC__metadata_object_delete(vc);
            FLAC__metadata_iterator_delete(it);
            FLAC__metadata_chain_delete(chain);
            return make_error(ErrorCode::io_error, "FLAC VC block insert failed", "path=" + path);
        }
    }
    FLAC__metadata_iterator_delete(it);

    FLAC__metadata_chain_sort_padding(chain);
    const FLAC__bool ok = FLAC__metadata_chain_write(chain, /*use_padding=*/1, /*preserve_file_stats=*/0);
    FLAC__metadata_chain_delete(chain);

    if (ok == 0) {
        return make_error(ErrorCode::io_error, "FLAC metadata chain write failed", "path=" + path);
    }
    return {};
}

} // anonymous namespace
// NOLINTEND(readability-static-definition-in-anonymous-namespace)

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
    if (ext == ".flac") {
        return write_flac_metadata(path, meta);
    }
    if (ext == ".mka") {
        return write_mka_metadata(path, meta);
    }
    return {}; // unsupported extension — silently skip
}

// Encode a fully post-processed float32 WAV to FLAC (24-bit, compression level 5).
// All loudness/peak adjustments must be applied to src_path before calling this so
// that quantisation happens exactly once on the final sample values.
Result<void> convert_to_flac(const std::string& src_path, const std::string& flac_path) {
    auto reader_res = FloatWavReader::open(src_path);
    if (!reader_res) {
        return tl::unexpected{reader_res.error()};
    }
    auto& reader = *reader_res;

    auto writer_res = FloatFlacWriter::open(flac_path, reader.channels(), reader.sample_rate());
    if (!writer_res) {
        return tl::unexpected{writer_res.error()};
    }
    auto& writer = *writer_res;

    constexpr uint64_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(reader.channels()) * k_block);
    uint64_t left = reader.frame_count();

    while (left > 0) {
        const uint64_t n = std::min(k_block, left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        if (writer.write(buf.data(), got) != got) {
            return make_error(ErrorCode::io_error, "short write in convert_to_flac", "path=" + flac_path);
        }
        left -= got;
    }
    if (left != 0) {
        return make_error(ErrorCode::io_error, "short read in convert_to_flac", "path=" + src_path);
    }
    return {};
}

} // namespace mradm::audio
