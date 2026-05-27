#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <opus.h>
#include <string>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"
#include "adm/errors.h"

// IAMF raw OBU stream writer (.iamf standalone, no ISO BMFF container).
// Spec: Alliance for Open Media IAMF v1.1.0.
// Channel-based layouts only; Opus codec; independent encoder per substream.

namespace mradm::audio {

namespace {

// OBU type constants
constexpr uint8_t k_obu_codec_config = 0;
constexpr uint8_t k_obu_audio_element = 1;
constexpr uint8_t k_obu_mix_presentation = 2;
constexpr uint8_t k_obu_audio_frame_base = 6; // implicit stream ID: type = 6 + sid (IDs 0..17)
constexpr uint8_t k_obu_ia_sequence_header = 31;

// LoudspeakerLayout enum (IAMF spec Table 4.5)
constexpr uint8_t k_ll_stereo = 1;
constexpr uint8_t k_ll_5_1 = 2;
constexpr uint8_t k_ll_5_1_2 = 3;
constexpr uint8_t k_ll_5_1_4 = 4;
constexpr uint8_t k_ll_7_1 = 5;
constexpr uint8_t k_ll_7_1_4 = 7;
constexpr uint8_t k_ll_expanded = 15;

// ExpandedLoudspeakerLayout enum (IAMF spec Table 4.6)
constexpr uint8_t k_exp_9_1_6 = 8;

// SoundSystem enum (IAMF spec Table 4.28 / Annex A)
constexpr uint8_t k_ss_stereo = 0; // A: 0.2.0
constexpr uint8_t k_ss_5_1 = 1;    // B: 0.5.0
constexpr uint8_t k_ss_5_1_2 = 2;  // C: 2.5.0
constexpr uint8_t k_ss_5_1_4 = 3;  // D: 4.5.0
constexpr uint8_t k_ss_7_1 = 8;    // I: 0.7.0
constexpr uint8_t k_ss_7_1_4 = 9;  // J: 4.7.0
constexpr uint8_t k_ss_9_1_6 = 13; // Extension 13: 6.9.0

constexpr int k_frame_size = 960;
constexpr int16_t k_audio_roll_distance = -4;
constexpr uint16_t k_preskip = 312;
constexpr uint32_t k_sample_rate = 48000;

// ---- Layout table ---------------------------------------------------------
// iamf_from_project[enc_position] = project channel index.
// Encoding order: nb_coupled stereo pairs first (2 floats each),
// then (nb_streams - nb_coupled) mono channels (1 float each).
//
// Project WAV channel orders:
//   0+2+0 : FL FR
//   0+5+0 : FL FR C LFE Ls Rs
//   2+5+0 : FL FR C LFE Ls Rs TpFL TpFR
//   4+5+0 : FL FR C LFE Ls Rs TpFL TpFR TpBL TpBR
//   wav71  : FL FR C LFE Rls Rrs Ls Rs          (WAVE_7_1)
//   4+7+0  : FL FR C LFE Ls Rs Rls Rrs TpFL TpFR TpBL TpBR
//   9.1.6  : FL FR C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr
//   9+10+3 : FLc FRc FC LFE BL BR FL FR BC LFE2 SiL SiR TpFL TpFR TpFC TpC TpBL TpBR TpSiL TpSiR TpBC BtFC BtFL BtFR

struct IamfLayoutInfo {
    std::string_view layout_id;
    uint32_t channels;
    uint8_t loudspeaker_layout;
    bool expanded;
    uint8_t expanded_layout; // valid when expanded == true
    uint8_t sound_system;
    uint8_t nb_streams;
    uint8_t nb_coupled;
    std::array<uint8_t, 24> iamf_from_project;
};

// clang-format off
constexpr std::array<IamfLayoutInfo, 7> k_layouts{{
    // Stereo: 1 coupled pair
    {"0+2+0", 2, k_ll_stereo, false, 0, k_ss_stereo, 1, 1,
     {0,1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    // 5.1: (FL,FR) (Ls,Rs) | C LFE
    {"0+5+0", 6, k_ll_5_1, false, 0, k_ss_5_1, 4, 2,
     {0,1, 4,5, 2,3, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    // 5.1.2: (FL,FR) (Ls,Rs) (TpFL,TpFR) | C LFE
    {"2+5+0", 8, k_ll_5_1_2, false, 0, k_ss_5_1_2, 5, 3,
     {0,1, 4,5, 6,7, 2,3, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    // 5.1.4: (FL,FR) (Ls,Rs) (TpFL,TpFR) (TpBL,TpBR) | C LFE
    {"4+5+0", 10, k_ll_5_1_4, false, 0, k_ss_5_1_4, 6, 4,
     {0,1, 4,5, 6,7, 8,9, 2,3, 0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    // 7.1 (wav71): proj is WAVE_7_1 = FL FR C LFE Rls Rrs Ls Rs
    //   IAMF pairs: (FL,FR) (Ls,Rs)=(proj6,7) (Rls,Rrs)=(proj4,5) | C LFE
    {"wav71", 8, k_ll_7_1, false, 0, k_ss_7_1, 5, 3,
     {0,1, 6,7, 4,5, 2,3, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    // 7.1.4: (FL,FR) (Ls,Rs) (Rls,Rrs) (TpFL,TpFR) (TpBL,TpBR) | C LFE
    {"4+7+0", 12, k_ll_7_1_4, false, 0, k_ss_7_1_4, 7, 5,
     {0,1, 4,5, 6,7, 8,9, 10,11, 2,3, 0,0,0,0,0,0,0,0,0,0,0,0}},
    // 9.1.6 expanded: (FL,FR) (Rls,Rrs) (Lw,Rw) (Ls,Rs) (Vhl,Vhr) (Ltr,Rtr) (Ltm,Rtm) | C LFE
    //   proj: [0]FL [1]FR [2]C [3]LFE [4]Ls [5]Rs [6]Rls [7]Rrs [8]Lw [9]Rw
    //         [10]Vhl [11]Vhr [12]Ltm [13]Rtm [14]Ltr [15]Rtr
    {"9.1.6", 16, k_ll_expanded, true, k_exp_9_1_6, k_ss_9_1_6, 9, 7,
     {0,1, 6,7, 8,9, 4,5, 10,11, 14,15, 12,13, 2,3, 0,0,0,0,0,0,0,0}},
}};
// clang-format on

const IamfLayoutInfo* find_layout(std::string_view layout_id, uint32_t channels) {
    const auto it = std::ranges::find_if(
        k_layouts, [&](const auto& li) { return li.layout_id == layout_id && li.channels == channels; });
    return it != k_layouts.end() ? &*it : nullptr;
}

// ---- Low-level OBU serialization ------------------------------------------

using Buf = std::vector<uint8_t>;

void push_u8(Buf& b, uint8_t v) {
    b.push_back(v);
}

void push_i16_be(Buf& b, int16_t v) {
    auto u = static_cast<uint16_t>(v);
    b.push_back(static_cast<uint8_t>(u >> 8u));
    b.push_back(static_cast<uint8_t>(u & 0xffu));
}

void push_uleb128(Buf& b, uint64_t v) {
    do {
        uint8_t byte = static_cast<uint8_t>(v & 0x7fu);
        v >>= 7u;
        if (v != 0u) {
            byte |= 0x80u;
        }
        b.push_back(byte);
    } while (v != 0u);
}

void push_str_nul(Buf& b, const char* s) {
    while (*s != '\0') {
        b.push_back(static_cast<uint8_t>(*s++));
    }
    b.push_back(0u);
}

bool write_obu(FILE* f, uint8_t obu_type, const Buf& payload) {
    // OBU header byte: obu_type f(5) | obu_redundant_copy f(1) | obu_trimming_status_flag f(1) | obu_extension_flag
    // f(1)
    uint8_t hdr = static_cast<uint8_t>(obu_type << 3u);
    if (fwrite(&hdr, 1, 1, f) != 1) {
        return false;
    }
    Buf size_buf;
    push_uleb128(size_buf, payload.size());
    if (fwrite(size_buf.data(), 1, size_buf.size(), f) != size_buf.size()) {
        return false;
    }
    if (!payload.empty()) {
        if (fwrite(payload.data(), 1, payload.size(), f) != payload.size()) {
            return false;
        }
    }
    return true;
}

// ---- Descriptor OBUs (written once at file open) --------------------------

bool write_ia_sequence_header(FILE* f) {
    Buf payload;
    payload.push_back('i');
    payload.push_back('a');
    payload.push_back('m');
    payload.push_back('f');
    push_u8(payload, 0); // primary_profile = Simple Profile
    push_u8(payload, 0); // additional_profile = Simple Profile
    return write_obu(f, k_obu_ia_sequence_header, payload);
}

bool write_codec_config(FILE* f) {
    // Single shared Codec Config (id=0). The IAMF decoder derives per-substream
    // channel count (1 or 2) from the Audio Element's coupled_substream_count,
    // so output_channel_count=2 here is just a reference value.
    Buf payload;
    push_uleb128(payload, 0); // codec_config_id = 0
    // codec_config {
    payload.push_back('O');
    payload.push_back('p');
    payload.push_back('u');
    payload.push_back('s');                      // codec_id = 'Opus'
    push_uleb128(payload, k_frame_size);         // num_samples_per_frame = 960
    push_i16_be(payload, k_audio_roll_distance); // audio_roll_distance = -4
    // decoder_config_opus: all f(N) fields are big-endian per IAMF spec
    push_u8(payload, 1);                                   // version
    push_u8(payload, 2);                                   // output_channel_count (reference; coupling overrides)
    push_i16_be(payload, static_cast<int16_t>(k_preskip)); // pre_skip f(16)
    push_u8(payload, static_cast<uint8_t>((k_sample_rate >> 24u) & 0xffu)); // input_sample_rate f(32)
    push_u8(payload, static_cast<uint8_t>((k_sample_rate >> 16u) & 0xffu));
    push_u8(payload, static_cast<uint8_t>((k_sample_rate >> 8u) & 0xffu));
    push_u8(payload, static_cast<uint8_t>(k_sample_rate & 0xffu));
    push_i16_be(payload, 0); // output_gain f(16)
    push_u8(payload, 0);     // channel_mapping_family
    // }
    return write_obu(f, k_obu_codec_config, payload);
}

bool write_audio_element(FILE* f, const IamfLayoutInfo& li) {
    Buf payload;
    push_uleb128(payload, 0);             // audio_element_id = 0
    push_u8(payload, 0x00);               // audio_element_type f(3)=0 (channel-based) | reserved f(5)=0
    push_uleb128(payload, 0);             // codec_config_id = 0
    push_uleb128(payload, li.nb_streams); // num_substreams
    for (uint8_t sid = 0; sid < li.nb_streams; ++sid) {
        push_uleb128(payload, sid); // audio_substream_id
    }
    push_uleb128(payload, 0); // num_parameters = 0

    // scalable_channel_layout_config:
    // num_layers f(3)=1, reserved f(5)=0 → (1 << 5) = 0x20
    push_u8(payload, 0x20u);

    // channel_audio_layer_config for layer 0:
    // loudspeaker_layout f(4) | output_gain_is_present f(1)=0 | recon_gain_is_present f(1)=0 | reserved_a f(2)=0
    push_u8(payload, static_cast<uint8_t>(li.loudspeaker_layout << 4u));
    push_u8(payload, li.nb_streams); // substream_count f(8)
    push_u8(payload, li.nb_coupled); // coupled_substream_count f(8)
    if (li.expanded) {
        push_u8(payload, li.expanded_layout); // expanded_loudspeaker_layout f(8)
    }

    return write_obu(f, k_obu_audio_element, payload);
}

bool write_mix_presentation(FILE* f,
                            const IamfLayoutInfo& li,
                            std::optional<double> loudness_lufs,
                            std::optional<double> peak_dbtp) {
    Buf payload;
    push_uleb128(payload, 0);     // mix_presentation_id = 0
    push_uleb128(payload, 1);     // count_label = 1
    push_str_nul(payload, "und"); // language_label (BCP-47 null-terminated)
    push_str_nul(payload, "");    // mix_presentation_friendly_label

    push_uleb128(payload, 1); // num_sub_mixes = 1
    // sub_mix[0]:
    push_uleb128(payload, 1);  // num_audio_elements = 1
    push_uleb128(payload, 0);  // audio_element_id = 0
    push_str_nul(payload, ""); // audio_element_friendly_label (for label "und")

    // RenderingConfig: headphones_rendering_mode f(2)=0 | element_gain_offset_flag f(1)=0
    //                  binaural_filter_profile f(2)=0 | reserved f(3)=0  → byte 0x00
    //                  rendering_config_extension_size Leb128() = 0 → byte 0x00
    push_u8(payload, 0x00u); // packed bit fields
    push_u8(payload, 0x00u); // rendering_config_extension_size = 0

    // element_mix_gain (MixGainParamDefinition):
    push_uleb128(payload, 1);             // parameter_id = 1
    push_uleb128(payload, k_sample_rate); // parameter_rate = 48000
    push_u8(payload, 0x80u);              // param_definition_mode f(1)=1 | reserved f(7)=0
    push_i16_be(payload, 0);              // default_mix_gain = 0 dB (Q7.8)

    // output_mix_gain (MixGainParamDefinition):
    push_uleb128(payload, 2); // parameter_id = 2
    push_uleb128(payload, k_sample_rate);
    push_u8(payload, 0x80u);
    push_i16_be(payload, 0); // default_mix_gain = 0 dB (Q7.8)

    // IAMF §3.7: every sub-mix MUST contain a stereo (Sound System A) layout.
    const bool is_stereo_target = (li.sound_system == k_ss_stereo);
    push_uleb128(payload, is_stereo_target ? 1u : 2u); // num_layouts

    const bool has_peak = peak_dbtp.has_value();
    const uint8_t info_type = has_peak ? 0x01u : 0x00u; // bit0 = true_peak
    const int16_t int_lufs =
        loudness_lufs.has_value() ? static_cast<int16_t>(*loudness_lufs * 256.0) : static_cast<int16_t>(0x7FFF);
    const int16_t dpeak =
        peak_dbtp.has_value() ? static_cast<int16_t>(*peak_dbtp * 256.0) : static_cast<int16_t>(0x7FFF);

    // layout[0]: target layout
    // layout_type f(2)=2 (loudspeaker) | sound_system f(4) | reserved f(2)=0 → 1 byte
    push_u8(payload, static_cast<uint8_t>((2u << 6u) | (static_cast<uint32_t>(li.sound_system) << 2u)));
    push_u8(payload, info_type);
    push_i16_be(payload, int_lufs);
    push_i16_be(payload, dpeak);
    if (has_peak) {
        push_i16_be(payload, dpeak); // true_peak = digital_peak
    }

    if (!is_stereo_target) {
        // layout[1]: mandatory stereo (Sound System A, sound_system=0)
        push_u8(payload, static_cast<uint8_t>(2u << 6u)); // layout_type=2 | sound_system=0 | reserved=0
        push_u8(payload, info_type);
        push_i16_be(payload, int_lufs);
        push_i16_be(payload, dpeak);
        if (has_peak) {
            push_i16_be(payload, dpeak);
        }
    }

    return write_obu(f, k_obu_mix_presentation, payload);
}

} // namespace

// ---- FloatIamfWriter -------------------------------------------------------

struct FloatIamfWriter::Impl {
    FILE* file{nullptr};
    std::vector<OpusEncoder*> encoders;
    std::vector<std::vector<float>> enc_bufs; // per-encoder pending samples
    std::vector<uint8_t> pkt_buf;
    std::vector<float> reorder_buf;
    const IamfLayoutInfo* layout_info{nullptr};
    uint32_t channels{0};
    bool failed{false};
    std::string path;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        for (auto* enc : encoders) {
            if (enc != nullptr) {
                opus_encoder_destroy(enc);
            }
        }
        if (file != nullptr) {
            fclose(file);
        }
    }
};

FloatIamfWriter::~FloatIamfWriter() = default;
FloatIamfWriter::FloatIamfWriter(FloatIamfWriter&&) noexcept = default;
FloatIamfWriter& FloatIamfWriter::operator=(FloatIamfWriter&&) noexcept = default;

Result<FloatIamfWriter> FloatIamfWriter::open(const std::string& path,
                                              uint32_t channels,
                                              uint32_t sample_rate,
                                              uint32_t bitrate_per_ch_kbps,
                                              const std::string& layout_id,
                                              std::optional<double> loudness_lufs,
                                              std::optional<double> peak_dbtp) {
    if (sample_rate != k_sample_rate) {
        return make_error(ErrorCode::invalid_argument,
                          "IAMF output requires 48000 Hz; got " + std::to_string(sample_rate));
    }

    if (layout_id == "9+10+3" || layout_id == "22.2") {
        return make_error(ErrorCode::unsupported,
                          "IAMF 22.2 / 10.2.9.3 is not supported: public IAMF v1.1 does not define a usable profile "
                          "for this layout");
    }

    const IamfLayoutInfo* li = find_layout(layout_id, channels);
    if (li == nullptr) {
        return make_error(ErrorCode::unsupported,
                          "IAMF layout not supported: " + layout_id + " (" + std::to_string(channels) + "ch)");
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return make_error(ErrorCode::io_error, "cannot open for writing: " + path);
    }

    std::vector<OpusEncoder*> encoders;
    encoders.reserve(li->nb_streams);

    for (uint8_t sid = 0; sid < li->nb_streams; ++sid) {
        const int enc_ch = (sid < li->nb_coupled) ? 2 : 1;
        int err = OPUS_OK;
        OpusEncoder* enc =
            opus_encoder_create(static_cast<opus_int32>(k_sample_rate), enc_ch, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK || enc == nullptr) {
            for (auto* e : encoders) {
                opus_encoder_destroy(e);
            }
            fclose(f);
            return make_error(ErrorCode::io_error, "opus_encoder_create failed: " + std::string(opus_strerror(err)));
        }

        // Bitrate: 64 kbps/ch default, minimum 128 kbps for stereo pairs.
        const uint32_t bps_kbps = (bitrate_per_ch_kbps > 0) ? bitrate_per_ch_kbps : 64u;
        const auto bps = static_cast<int>(bps_kbps * 1000u * static_cast<uint32_t>(enc_ch));
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(bps));

        encoders.push_back(enc);
    }

    // Write descriptor OBUs before any audio data.
    bool ok = write_ia_sequence_header(f);
    ok = ok && write_codec_config(f);
    ok = ok && write_audio_element(f, *li);
    ok = ok && write_mix_presentation(f, *li, loudness_lufs, peak_dbtp);
    if (!ok) {
        for (auto* e : encoders) {
            opus_encoder_destroy(e);
        }
        fclose(f);
        return make_error(ErrorCode::io_error, "IAMF header write failed: " + path);
    }

    auto impl = std::make_unique<Impl>();
    impl->file = f;
    impl->encoders = std::move(encoders);
    impl->enc_bufs.resize(li->nb_streams);
    impl->pkt_buf.resize(4096);
    impl->reorder_buf.resize(channels);
    impl->layout_info = li;
    impl->channels = channels;
    impl->path = path;

    FloatIamfWriter writer;
    writer.impl_ = std::move(impl);
    return writer;
}

uint64_t FloatIamfWriter::write(const float* samples, uint64_t frame_count) {
    auto& im = *impl_;
    if (im.failed) {
        return 0;
    }
    const IamfLayoutInfo& li = *im.layout_info;
    // enc_ch for stream 0: 2 if any coupled, else 1
    const int enc_ch_0 = (li.nb_coupled > 0) ? 2 : 1;
    const size_t flush_threshold = static_cast<size_t>(enc_ch_0 * k_frame_size);

    for (uint64_t f = 0; f < frame_count; ++f) {
        // Distribute one sample frame (all channels) to per-encoder buffers
        // after reordering from project order to IAMF encoding order.
        const float* src = samples + f * im.channels;
        uint32_t ch_idx = 0;
        for (uint8_t sid = 0; sid < li.nb_streams; ++sid) {
            const int enc_ch = (sid < li.nb_coupled) ? 2 : 1;
            for (int c = 0; c < enc_ch; ++c) {
                im.enc_bufs[sid].push_back(src[li.iamf_from_project[ch_idx++]]);
            }
        }

        // All encoder buffers fill at the same rate (same number of audio frames).
        // Flush when enc_bufs[0] accumulates a complete Opus frame.
        if (im.enc_bufs[0].size() == flush_threshold) {
            for (uint8_t sid = 0; sid < li.nb_streams; ++sid) {
                const int pkt_size = opus_encode_float(im.encoders[sid],
                                                       im.enc_bufs[sid].data(),
                                                       k_frame_size,
                                                       im.pkt_buf.data(),
                                                       static_cast<opus_int32>(im.pkt_buf.size()));
                if (pkt_size < 0) {
                    im.failed = true;
                    return f;
                }
                Buf frame_payload(im.pkt_buf.begin(), im.pkt_buf.begin() + pkt_size);
                if (!write_obu(im.file, static_cast<uint8_t>(k_obu_audio_frame_base + sid), frame_payload)) {
                    im.failed = true;
                    return f;
                }
                im.enc_bufs[sid].clear();
            }
        }
    }
    return frame_count;
}

Result<void> FloatIamfWriter::close() {
    if (!impl_) {
        return {};
    }
    auto& im = *impl_;
    if (im.failed) {
        return make_error(ErrorCode::io_error, "IAMF encoding error: " + im.path);
    }

    const IamfLayoutInfo& li = *im.layout_info;

    // Flush partial final frame with zero-padding if needed.
    if (!im.enc_bufs[0].empty()) {
        for (uint8_t sid = 0; sid < li.nb_streams; ++sid) {
            const int enc_ch = (sid < li.nb_coupled) ? 2 : 1;
            const size_t target = static_cast<size_t>(enc_ch * k_frame_size);
            im.enc_bufs[sid].resize(target, 0.0f);

            const int pkt_size = opus_encode_float(im.encoders[sid],
                                                   im.enc_bufs[sid].data(),
                                                   k_frame_size,
                                                   im.pkt_buf.data(),
                                                   static_cast<opus_int32>(im.pkt_buf.size()));
            if (pkt_size > 0) {
                Buf frame_payload(im.pkt_buf.begin(), im.pkt_buf.begin() + pkt_size);
                write_obu(im.file, static_cast<uint8_t>(k_obu_audio_frame_base + sid), frame_payload);
            }
        }
    }

    if (fflush(im.file) != 0) {
        return make_error(ErrorCode::io_error, "IAMF flush failed: " + im.path);
    }
    fclose(im.file);
    im.file = nullptr;
    return {};
}

// ---- convert_to_iamf -------------------------------------------------------

Result<void> convert_to_iamf(const std::string& src_path,
                             const std::string& iamf_path,
                             const std::string& layout_id,
                             uint32_t bitrate_per_ch_kbps,
                             std::optional<double> loudness_lufs,
                             std::optional<double> peak_dbtp) {
    auto reader_res = FloatWavReader::open(src_path);
    if (!reader_res) {
        return tl::unexpected{reader_res.error()};
    }
    auto reader = std::move(*reader_res);

    auto writer_res = FloatIamfWriter::open(
        iamf_path, reader.channels(), reader.sample_rate(), bitrate_per_ch_kbps, layout_id, loudness_lufs, peak_dbtp);
    if (!writer_res) {
        return tl::unexpected{writer_res.error()};
    }
    auto writer = std::move(*writer_res);

    constexpr uint64_t k_buf_frames = 4096;
    std::vector<float> buf(k_buf_frames * reader.channels());

    while (true) {
        const uint64_t n = reader.read(buf.data(), k_buf_frames);
        if (n == 0) {
            break;
        }
        writer.write(buf.data(), n);
    }

    return writer.close();
}

} // namespace mradm::audio
