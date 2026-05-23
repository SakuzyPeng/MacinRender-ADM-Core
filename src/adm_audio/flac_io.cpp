// DR_FLAC_IMPLEMENTATION must be defined in exactly one TU.
#define DR_FLAC_IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <dr_flac.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "audio_io_internal.h"

namespace mradm::audio {

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
        tags_ok &= append_tag("COMMENT=" + compact_metadata_comment(meta));
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
