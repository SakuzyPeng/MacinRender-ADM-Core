// DR_FLAC_IMPLEMENTATION must be defined in exactly one TU.
#define DR_FLAC_IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dr_flac.h>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <FLAC/callback.h>
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

namespace {

void emit_flac_progress(ProgressSink* progress,
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

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()), wide.data(), size);
    return wide;
}
#endif

FILE* open_binary_write(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = utf8_to_wide(path);
    if (wide.empty()) {
        return nullptr;
    }
    return _wfopen(wide.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

FILE* open_binary_read(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = utf8_to_wide(path);
    if (wide.empty()) {
        return nullptr;
    }
    return _wfopen(wide.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

int seek_binary_file(FILE* file, FLAC__uint64 absolute_byte_offset) {
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(absolute_byte_offset), SEEK_SET);
#else
    return fseeko(file, static_cast<off_t>(absolute_byte_offset), SEEK_SET);
#endif
}

int seek_binary_file(FILE* file, FLAC__int64 offset, int whence) {
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(offset), whence);
#else
    return fseeko(file, static_cast<off_t>(offset), whence);
#endif
}

bool tell_binary_file(FILE* file, FLAC__uint64* absolute_byte_offset) {
#ifdef _WIN32
    const __int64 pos = _ftelli64(file);
    if (pos < 0) {
        return false;
    }
    *absolute_byte_offset = static_cast<FLAC__uint64>(pos);
    return true;
#else
    const off_t pos = ftello(file);
    if (pos < 0) {
        return false;
    }
    *absolute_byte_offset = static_cast<FLAC__uint64>(pos);
    return true;
#endif
}

FLAC__int64 tell_binary_file(FILE* file) {
#ifdef _WIN32
    return static_cast<FLAC__int64>(_ftelli64(file));
#else
    return static_cast<FLAC__int64>(ftello(file));
#endif
}

bool replace_file(const std::string& src_path, const std::string& dst_path) {
#ifdef _WIN32
    const std::wstring src = utf8_to_wide(src_path);
    const std::wstring dst = utf8_to_wide(dst_path);
    if (src.empty() || dst.empty()) {
        return false;
    }
    return MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(src_path, dst_path, ec);
    return !ec;
#endif
}

FLAC__StreamEncoderWriteStatus flac_write_callback(const FLAC__StreamEncoder* /*encoder*/,
                                                   const FLAC__byte buffer[],
                                                   size_t bytes,
                                                   uint32_t /*samples*/,
                                                   uint32_t /*current_frame*/,
                                                   void* client_data) {
    auto* file = static_cast<FILE*>(client_data);
    if (file == nullptr || std::fwrite(buffer, 1U, bytes, file) != bytes) {
        return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

FLAC__StreamEncoderSeekStatus
flac_seek_callback(const FLAC__StreamEncoder* /*encoder*/, FLAC__uint64 absolute_byte_offset, void* client_data) {
    auto* file = static_cast<FILE*>(client_data);
    if (file == nullptr) {
        return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    }
    return seek_binary_file(file, absolute_byte_offset) == 0 ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK
                                                             : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
}

FLAC__StreamEncoderTellStatus
flac_tell_callback(const FLAC__StreamEncoder* /*encoder*/, FLAC__uint64* absolute_byte_offset, void* client_data) {
    auto* file = static_cast<FILE*>(client_data);
    if (file == nullptr || absolute_byte_offset == nullptr || !tell_binary_file(file, absolute_byte_offset)) {
        return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
    }
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

size_t flac_io_read_callback(void* ptr, size_t size, size_t nmemb, FLAC__IOHandle handle) {
    return std::fread(ptr, size, nmemb, static_cast<FILE*>(handle));
}

size_t flac_io_write_callback(const void* ptr, size_t size, size_t nmemb, FLAC__IOHandle handle) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(handle));
}

int flac_io_seek_callback(FLAC__IOHandle handle, FLAC__int64 offset, int whence) {
    return seek_binary_file(static_cast<FILE*>(handle), offset, whence);
}

FLAC__int64 flac_io_tell_callback(FLAC__IOHandle handle) {
    return tell_binary_file(static_cast<FILE*>(handle));
}

int flac_io_eof_callback(FLAC__IOHandle handle) {
    return std::feof(static_cast<FILE*>(handle));
}

struct FileCloser {
    void operator()(FILE* file) const {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

using UniqueFile = std::unique_ptr<FILE, FileCloser>;

FLAC__IOCallbacks flac_metadata_source_callbacks() {
    FLAC__IOCallbacks callbacks{};
    callbacks.read = flac_io_read_callback;
    callbacks.seek = flac_io_seek_callback;
    callbacks.tell = flac_io_tell_callback;
    callbacks.eof = flac_io_eof_callback;
    return callbacks;
}

FLAC__IOCallbacks flac_metadata_temp_callbacks() {
    FLAC__IOCallbacks callbacks{};
    callbacks.write = flac_io_write_callback;
    return callbacks;
}

} // namespace

struct FloatFlacWriter::Impl {
    FLAC__StreamEncoder* encoder{nullptr};
    FILE* file{nullptr};
    uint32_t channels{};
    std::vector<FLAC__int32> int_buf;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() {
        if (encoder != nullptr) {
            FLAC__stream_encoder_finish(encoder);
            FLAC__stream_encoder_delete(encoder);
        }
        if (file != nullptr) {
            std::fclose(file);
        }
    }
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

    FILE* file = open_binary_write(path);
    if (file == nullptr) {
        FLAC__stream_encoder_delete(enc);
        return make_error(ErrorCode::io_error, "failed to open FLAC file for writing", "path=" + path);
    }

    const FLAC__StreamEncoderInitStatus status = FLAC__stream_encoder_init_stream(
        enc, flac_write_callback, flac_seek_callback, flac_tell_callback, nullptr, file);
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(enc);
        std::fclose(file);
        return make_error(ErrorCode::io_error,
                          fmt::format("FLAC encoder init failed: {}", FLAC__StreamEncoderInitStatusString[status]),
                          "path=" + path);
    }

    FloatFlacWriter w;
    w.impl_ = std::make_unique<Impl>();
    w.impl_->encoder = enc;
    w.impl_->file = file;
    w.impl_->channels = channels;
    return w;
}

FloatFlacWriter::~FloatFlacWriter() = default;

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

    UniqueFile source(open_binary_read(path));
    if (source == nullptr) {
        FLAC__metadata_chain_delete(chain);
        return make_error(ErrorCode::io_error, "failed to open FLAC file for metadata", "path=" + path);
    }

    const FLAC__IOCallbacks source_callbacks = flac_metadata_source_callbacks();
    if (FLAC__metadata_chain_read_with_callbacks(chain, source.get(), source_callbacks) == 0) {
        const auto status = FLAC__metadata_chain_status(chain);
        FLAC__metadata_chain_delete(chain);
        return make_error(ErrorCode::io_error,
                          fmt::format("FLAC metadata chain read failed: {}", FLAC__Metadata_ChainStatusString[status]),
                          "path=" + path);
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
    const std::string temp_path = path + ".metadata_tmp";
    UniqueFile temp(open_binary_write(temp_path));
    if (temp == nullptr) {
        FLAC__metadata_chain_delete(chain);
        return make_error(ErrorCode::io_error, "failed to open FLAC metadata temp file", "path=" + temp_path);
    }

    const FLAC__IOCallbacks temp_callbacks = flac_metadata_temp_callbacks();
    const FLAC__bool ok = FLAC__metadata_chain_write_with_callbacks_and_tempfile(
        chain, /*use_padding=*/1, source.get(), source_callbacks, temp.get(), temp_callbacks);
    const auto status = FLAC__metadata_chain_status(chain);
    FLAC__metadata_chain_delete(chain);

    if (ok == 0) {
        std::filesystem::remove(temp_path);
        return make_error(ErrorCode::io_error,
                          fmt::format("FLAC metadata chain write failed: {}", FLAC__Metadata_ChainStatusString[status]),
                          "path=" + path);
    }

    source.reset();
    temp.reset();
    if (!replace_file(temp_path, path)) {
        std::filesystem::remove(temp_path);
        return make_error(ErrorCode::io_error, "failed to replace FLAC file after metadata write", "path=" + path);
    }
    return {};
}

// Encode a fully post-processed float32 WAV to FLAC (24-bit, compression level 5).
// All loudness/peak adjustments must be applied to src_path before calling this so
// that quantisation happens exactly once on the final sample values.
Result<void> convert_to_flac(const std::string& src_path,
                             const std::string& flac_path,
                             const std::stop_token& cancel_token,
                             ProgressSink* progress,
                             RenderOperation operation) {
    if (cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + flac_path);
    }
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
    const uint64_t total_frames = left;
    uint64_t done = 0;
    emit_flac_progress(progress, operation, 0.0, 0, total_frames, "encoding FLAC");

    while (left > 0) {
        if (cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "path=" + flac_path);
        }
        const uint64_t n = std::min(k_block, left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        if (writer.write(buf.data(), got) != got) {
            return make_error(ErrorCode::io_error, "short write in convert_to_flac", "path=" + flac_path);
        }
        left -= got;
        done += got;
        emit_flac_progress(progress,
                           operation,
                           static_cast<double>(done) / static_cast<double>(std::max<uint64_t>(1, total_frames)),
                           done,
                           total_frames,
                           "encoding FLAC");
    }
    if (left != 0) {
        return make_error(ErrorCode::io_error, "short read in convert_to_flac", "path=" + src_path);
    }
    emit_flac_progress(progress, operation, 1.0, total_frames, total_frames, "FLAC encoded");
    return {};
}

} // namespace mradm::audio
