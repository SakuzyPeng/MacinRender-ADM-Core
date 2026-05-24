#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <opus_multistream.h>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "audio_io_internal.h"

namespace mradm::audio {

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

[[nodiscard]] bool is_square_channel_count(uint32_t channels) {
    const auto root = static_cast<uint32_t>(std::sqrt(static_cast<double>(channels)));
    return root * root == channels;
}

[[nodiscard]] bool is_hoa_layout(std::string_view layout_id, uint32_t channels) {
    return layout_id.starts_with("hoa") && is_square_channel_count(channels);
}

[[nodiscard]] bool is_standard_opus_surround_layout(std::string_view layout_id, uint32_t channels) {
    if ((layout_id == "0+5+0" || layout_id == "5.1") && channels == 6U) {
        return true;
    }
    if ((layout_id == "wav71" || layout_id == "7.1") && channels == 8U) {
        return true;
    }
    return false;
}

[[nodiscard]] int opus_mapping_family(uint32_t channels, std::string_view layout_id) {
    if (is_hoa_layout(layout_id, channels)) {
        return 2;
    }
    if (channels <= 2U) {
        return 0;
    }
    if (is_standard_opus_surround_layout(layout_id, channels)) {
        return 1;
    }
    if (layout_id.empty() && channels <= 8U) {
        return 1;
    }
    return 255;
}

[[nodiscard]] std::vector<uint32_t> opus_vorbis_source_order(std::string_view layout_id, uint32_t channels) {
    if ((layout_id == "0+5+0" || layout_id == "5.1") && channels == 6U) {
        // Internal/WAVE order: L R C LFE Ls Rs
        // Opus family 1/Vorbis order: L C R Ls Rs LFE
        return {0U, 2U, 1U, 4U, 5U, 3U};
    }
    if ((layout_id == "wav71" || layout_id == "7.1") && channels == 8U) {
        // Internal WAVE_7_1 order: L R C LFE Rls Rrs Ls Rs
        // Opus family 1/Vorbis order: L C R Ls Rs Rls Rrs LFE
        return {0U, 2U, 1U, 6U, 7U, 4U, 5U, 3U};
    }
    return {};
}

[[nodiscard]] std::string fixed_decimal(double value, int decimals) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
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
        failure_message = std::string{"opus_multistream_encode_float failed: "} + opus_strerror(ret);
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
                                                    uint32_t bitrate_per_ch_kbps,
                                                    const std::string& layout_id) {
    if (sample_rate != 48000U) {
        return make_error(ErrorCode::unsupported,
                          "Opus MKA requires 48000 Hz input, got " + std::to_string(sample_rate) + " Hz",
                          "path=" + path);
    }
    if (channels < 1U || channels > 255U) {
        return make_error(
            ErrorCode::unsupported, "Opus supports 1-255 channels, got " + std::to_string(channels), "path=" + path);
    }

    auto impl = std::make_unique<Impl>();
    impl->channels = channels;
    impl->path = path;

    const int family = opus_mapping_family(channels, layout_id);

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
                          std::string{"opus_multistream_surround_encoder_create failed: "} + opus_strerror(error),
                          "path=" + path);
    }

    if (bitrate_per_ch_kbps > 0U && (bitrate_per_ch_kbps < 6U || bitrate_per_ch_kbps > 320U)) {
        return make_error(ErrorCode::invalid_argument,
                          "opus_bitrate_per_ch_kbps must be 0 (auto) or 6-320, got " +
                              std::to_string(bitrate_per_ch_kbps),
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
        return make_error(
            ErrorCode::io_error, std::string{op.data(), op.size()} + " failed: " + opus_strerror(ret), "path=" + path);
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
                          "Opus MKA requires 48000 Hz source, got " + std::to_string(reader.sample_rate()) + " Hz",
                          "src=" + src_path);
    }

    auto writer_res =
        FloatOpusMkaWriter::open(mka_path, reader.channels(), reader.sample_rate(), bitrate_per_ch_kbps, layout_id);
    if (!writer_res) {
        return tl::unexpected{writer_res.error()};
    }
    auto& writer = *writer_res;

    constexpr uint64_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(reader.channels()) * k_block);
    const auto source_order = opus_vorbis_source_order(layout_id, reader.channels());
    std::vector<float> reordered_buf(source_order.empty() ? 0U : buf.size());
    uint64_t left = reader.frame_count();
    while (left > 0) {
        const uint64_t n = std::min(k_block, left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        const float* write_ptr = buf.data();
        if (!source_order.empty()) {
            const auto channels = static_cast<std::size_t>(reader.channels());
            for (uint64_t frame = 0; frame < got; ++frame) {
                const auto base = static_cast<std::size_t>(frame) * channels;
                for (std::size_t dst_ch = 0; dst_ch < channels; ++dst_ch) {
                    reordered_buf[base + dst_ch] = buf[base + source_order[dst_ch]];
                }
            }
            write_ptr = reordered_buf.data();
        }
        if (writer.write(write_ptr, got) != got) {
            return make_error(ErrorCode::io_error, "short write in convert_to_opus_mka", "path=" + mka_path);
        }
        left -= got;
    }
    if (left != 0) {
        return make_error(ErrorCode::io_error, "short read in convert_to_opus_mka", "src=" + src_path);
    }
    return writer.close();
}
// NOLINTEND(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-pro-type-vararg,readability-function-size)

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

} // namespace

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
        append_simple("INTEGRATED_LOUDNESS", fixed_decimal(*meta.lufs, 1));
    }
    if (meta.peak_dbtp) {
        append_simple("TRUE_PEAK", fixed_decimal(*meta.peak_dbtp, 2));
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

} // namespace mradm::audio
