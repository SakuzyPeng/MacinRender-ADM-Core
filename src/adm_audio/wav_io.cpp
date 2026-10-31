#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "audio_io_internal.h"
#include "wav_backend.h"

namespace mradm::audio {

// ── FloatWavWriter / FloatCafWriter ───────────────────────────────────────────
// Both wrap the Rust final-PCM writer, which also numbers and writes the writer/pass* checkpoints.

struct FloatWavWriter::Impl {
    std::unique_ptr<RustOutputWriter> writer;
};

Result<FloatWavWriter> FloatWavWriter::open(const std::string& path, uint32_t channels, uint32_t sample_rate) {
    auto writer = RustOutputWriter::create(path, channels, sample_rate, std::nullopt);
    if (!writer) {
        return tl::unexpected{writer.error()};
    }
    FloatWavWriter w;
    w.impl_ = std::make_unique<Impl>(Impl{std::move(*writer)});
    return w;
}

// Destroying the Rust writer finishes it on a best-effort basis; finish() reports the same failure.
FloatWavWriter::~FloatWavWriter() = default;
FloatWavWriter::FloatWavWriter(FloatWavWriter&&) noexcept = default;
FloatWavWriter& FloatWavWriter::operator=(FloatWavWriter&&) noexcept = default;

uint64_t FloatWavWriter::write(const float* samples, uint64_t frame_count) {
    return impl_->writer->write(samples, frame_count) ? frame_count : 0U;
}

Result<void> FloatWavWriter::finish() {
    return impl_->writer->finish();
}

struct FloatCafWriter::Impl {
    std::unique_ptr<RustOutputWriter> writer;
};

Result<FloatCafWriter>
FloatCafWriter::open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id) {
    auto writer = RustOutputWriter::create(path, channels, sample_rate, layout_id);
    if (!writer) {
        return tl::unexpected{writer.error()};
    }
    FloatCafWriter w;
    w.impl_ = std::make_unique<Impl>(Impl{std::move(*writer)});
    return w;
}

FloatCafWriter::~FloatCafWriter() = default;
FloatCafWriter::FloatCafWriter(FloatCafWriter&&) noexcept = default;
FloatCafWriter& FloatCafWriter::operator=(FloatCafWriter&&) noexcept = default;

uint64_t FloatCafWriter::write(const float* samples, uint64_t frame_count) {
    return impl_->writer->write(samples, frame_count) ? frame_count : 0U;
}

Result<void> FloatCafWriter::finish() {
    return impl_->writer->finish();
}

// ── FloatCafReader ────────────────────────────────────────────────────────────

struct FloatCafReader::Impl {
    std::unique_ptr<RustCafReader> reader;
};

Result<FloatCafReader> FloatCafReader::open(const std::string& path) {
    auto reader = RustCafReader::open(path);
    if (!reader) {
        return tl::unexpected{reader.error()};
    }
    FloatCafReader r;
    r.impl_ = std::make_unique<Impl>(Impl{std::move(*reader)});
    return r;
}

FloatCafReader::~FloatCafReader() = default;
FloatCafReader::FloatCafReader(FloatCafReader&&) noexcept = default;
FloatCafReader& FloatCafReader::operator=(FloatCafReader&&) noexcept = default;

uint32_t FloatCafReader::channels() const {
    return impl_->reader->info().channels;
}
uint32_t FloatCafReader::sample_rate() const {
    return impl_->reader->info().rate;
}
uint64_t FloatCafReader::frame_count() const {
    return impl_->reader->info().frames;
}
uint64_t FloatCafReader::read(float* out, uint64_t frames) {
    return impl_->reader->read(out, frames);
}

// Appends a CAF info chunk with encodingapplication / date / comments.
Result<void> write_caf_metadata(const std::string& path, const MetadataFields& meta) {
    const auto utf8_path = wav_path_utf8(path);
    const auto comments = compact_metadata_comment(meta);
    std::array<uint8_t, 512> message{};
    const auto bytes = [](std::string_view text) { return reinterpret_cast<const uint8_t*>(text.data()); };
    const auto code = mradm_caf_append_info(bytes(utf8_path),
                                            utf8_path.size(),
                                            bytes(meta.encoder),
                                            meta.encoder.size(),
                                            bytes(meta.date_utc),
                                            meta.date_utc.size(),
                                            bytes(comments),
                                            comments.size(),
                                            message.data(),
                                            message.size());
    if (code != 0) {
        return tl::unexpected{wav_error(code, message, path)};
    }
    return {};
}

// ── FloatWavReader ────────────────────────────────────────────────────────────

struct FloatWavReader::Impl {
    std::unique_ptr<RustWavReader> reader;
};

Result<FloatWavReader> FloatWavReader::open(const std::string& path) {
    auto reader = RustWavReader::open(path);
    if (!reader) {
        return tl::unexpected{reader.error()};
    }
    FloatWavReader r;
    r.impl_ = std::make_unique<Impl>();
    r.impl_->reader = std::move(*reader);
    return r;
}

FloatWavReader::~FloatWavReader() = default;
FloatWavReader::FloatWavReader(FloatWavReader&&) noexcept = default;
FloatWavReader& FloatWavReader::operator=(FloatWavReader&&) noexcept = default;

uint32_t FloatWavReader::channels() const {
    return impl_->reader->info().channels;
}
uint32_t FloatWavReader::sample_rate() const {
    return impl_->reader->info().rate;
}
uint64_t FloatWavReader::frame_count() const {
    return impl_->reader->info().frames;
}
uint32_t FloatWavReader::channel_mask() const {
    return impl_->reader->info().channel_mask;
}
uint16_t FloatWavReader::bits_per_sample() const {
    return impl_->reader->info().bits;
}
bool FloatWavReader::is_linear_pcm() const {
    return impl_->reader->info().format == 1U;
}
bool FloatWavReader::is_ieee_float() const {
    return impl_->reader->info().format == 3U;
}

uint64_t FloatWavReader::read(float* out, uint64_t frames) {
    auto read = impl_->reader->read(out, frames);
    return read ? *read : 0U;
}

bool FloatWavReader::seek(uint64_t frame) {
    return impl_->reader->seek_frame(frame).has_value();
}

// ── downconvert_to_int / finalize_wav_layout ─────────────────────────────────
// Rust converts or rewrites into a sidecar and installs it with a backup (PCM consistency 4.8).

namespace {

[[nodiscard]] const uint8_t* wav_bytes(std::string_view text) {
    return reinterpret_cast<const uint8_t*>(text.data());
}

} // namespace

Result<void> downconvert_to_int(const std::string& path,
                                uint16_t bit_depth,
                                const std::stop_token& cancel_token,
                                ProgressSink* progress,
                                RenderOperation operation) {
    if (cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
    }
    const auto utf8_path = wav_path_utf8(path);
    return run_output_pass(path, cancel_token, progress, operation, [&](auto... out) {
        return mradm_output_bitdepth_begin(wav_bytes(utf8_path), utf8_path.size(), bit_depth, out...);
    });
}

Result<void> finalize_wav_layout(const std::string& path,
                                 const WavLayoutFinalization& layout,
                                 const std::stop_token& cancel_token,
                                 ProgressSink* progress,
                                 RenderOperation operation) {
    if (cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
    }
    std::vector<MradmWavChnaTrack> chna(layout.chna.size());
    std::ranges::transform(layout.chna, chna.begin(), [](const WavChnaEntry& entry) {
        return MradmWavChnaTrack{wav_bytes(entry.track_uid),
                                 entry.track_uid.size(),
                                 wav_bytes(entry.track_format),
                                 entry.track_format.size(),
                                 wav_bytes(entry.pack_format),
                                 entry.pack_format.size(),
                                 entry.track_index};
    });
    const MradmWavLayoutOptions options{layout.output_channel_sources.data(),
                                        layout.output_channel_sources.size(),
                                        chna.data(),
                                        chna.size(),
                                        wav_bytes(layout.axml),
                                        layout.axml.size(),
                                        layout.channel_mask,
                                        static_cast<uint8_t>(layout.force_extensible ? 1U : 0U),
                                        static_cast<uint8_t>(layout.prefer_riff ? 1U : 0U),
                                        static_cast<uint8_t>(layout.include_pcm_fact ? 1U : 0U)};
    const auto utf8_path = wav_path_utf8(path);
    return run_output_pass(path, cancel_token, progress, operation, [&](auto... out) {
        return mradm_output_finalize_begin(wav_bytes(utf8_path), utf8_path.size(), nullptr, 0, 0, &options, 0, out...);
    });
}

Result<void> finalize_wav_output(const std::string& path,
                                 const std::string& output_layout,
                                 bool speaker_rerender,
                                 const std::stop_token& cancel_token,
                                 ProgressSink* progress) {
    const auto utf8_path = wav_path_utf8(path);
    return run_output_pass(path, cancel_token, progress, RenderOperation::write_metadata, [&](auto... out) {
        return mradm_output_finalize_begin(wav_bytes(utf8_path),
                                           utf8_path.size(),
                                           wav_bytes(output_layout),
                                           output_layout.size(),
                                           speaker_rerender ? 1U : 0U,
                                           nullptr,
                                           cancel_token.stop_requested() ? 1U : 0U,
                                           out...);
    });
}

// Append a BWF v2 bext chunk (EBU Tech 3285, 602-byte payload) to an existing RIFF/RF64/BW64 WAVE file;
// HOA3 output also carries the AmbiX ambi marker. mradm-wav updates the container size.
Result<void> write_wav_metadata(const std::string& path, const MetadataFields& meta) {
    const std::string description =
        meta.renderer.empty() ? "" : "renderer=" + meta.renderer + " layout=" + meta.output_layout;
    const MradmWavBext fields{wav_bytes(description),
                              description.size(),
                              wav_bytes(meta.encoder),
                              meta.encoder.size(),
                              wav_bytes(meta.output_layout),
                              meta.output_layout.size(),
                              wav_bytes(meta.date_utc),
                              meta.date_utc.size(),
                              meta.lufs.value_or(0.0),
                              meta.peak_dbtp.value_or(0.0),
                              static_cast<uint8_t>(meta.lufs.has_value() ? 1U : 0U),
                              static_cast<uint8_t>(meta.peak_dbtp.has_value() ? 1U : 0U),
                              static_cast<uint8_t>(meta.output_layout == "hoa3" ? 1U : 0U)};
    const auto utf8_path = wav_path_utf8(path);
    std::array<uint8_t, 512> message{};
    const auto code =
        mradm_wav_append_bext(wav_bytes(utf8_path), utf8_path.size(), &fields, message.data(), message.size());
    if (code != 0) {
        return tl::unexpected{wav_error(code, message, path)};
    }
    return {};
}

Result<WavAdmChunks> read_wav_adm_chunks(const std::string& path) {
    auto reader = RustWavChunks::open(path);
    if (!reader) {
        return tl::unexpected{reader.error()};
    }
    WavAdmChunks chunks;
    auto axml = (*reader)->chunk("axml");
    if (!axml) {
        return tl::unexpected{axml.error()};
    }
    chunks.axml = std::move(*axml).value_or(std::string{});
    auto chna = (*reader)->chna();
    if (!chna) {
        return tl::unexpected{chna.error()};
    }
    chunks.chna = std::move(*chna);
    return chunks;
}

Result<void> replace_wav_axml(const std::string& source, const std::string& target, std::string_view axml) {
    const auto utf8_source = wav_path_utf8(source);
    const auto utf8_target = wav_path_utf8(target);
    std::array<uint8_t, 512> message{};
    std::array<uint8_t, 1024> context{};
    const auto code = mradm_wav_replace_axml(wav_bytes(utf8_source),
                                             utf8_source.size(),
                                             wav_bytes(utf8_target),
                                             utf8_target.size(),
                                             wav_bytes(axml),
                                             axml.size(),
                                             message.data(),
                                             message.size(),
                                             context.data(),
                                             context.size());
    if (code != 0) {
        return tl::unexpected{post_error(code, message, context)};
    }
    return {};
}

Result<bool> wav_has_chunk(const std::string& path, std::string_view id) {
    if (id.size() != 4U) {
        return make_error(ErrorCode::invalid_argument, "WAVE chunk id must have four characters", "path=" + path);
    }
    const auto utf8_path = wav_path_utf8(path);
    std::array<uint8_t, 512> message{};
    uint8_t present = 0;
    const auto code = mradm_wav_has_chunk(
        wav_bytes(utf8_path), utf8_path.size(), wav_bytes(id), &present, message.data(), message.size());
    if (code != 0) {
        return tl::unexpected{wav_error(code, message, path)};
    }
    return present != 0U;
}

} // namespace mradm::audio
