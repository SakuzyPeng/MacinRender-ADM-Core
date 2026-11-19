#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "audio_io_internal.h"
#include "wav_backend.h"

namespace mradm::audio {

// ── FloatWavWriter / FloatCafWriter ───────────────────────────────────────────
// Both wrap the Rust final-PCM writer, which also numbers and writes the writer/pass* checkpoints.

struct FloatWavWriter::Impl {
    std::unique_ptr<PcmOutputWriter> writer;
};

Result<FloatWavWriter> FloatWavWriter::open(const std::string& path, uint32_t channels, uint32_t sample_rate) {
    auto writer = PcmOutputWriter::create(path, channels, sample_rate, std::nullopt);
    if (!writer) {
        return mradm::unexpected{writer.error()};
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
    std::unique_ptr<PcmOutputWriter> writer;
};

Result<FloatCafWriter>
FloatCafWriter::open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id) {
    auto writer = PcmOutputWriter::create(path, channels, sample_rate, layout_id);
    if (!writer) {
        return mradm::unexpected{writer.error()};
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
        return mradm::unexpected{wav_error(code, message, path)};
    }
    return {};
}

// ── FloatWavReader ────────────────────────────────────────────────────────────

struct FloatWavReader::Impl {
    std::unique_ptr<WavFileReader> reader;
};

Result<FloatWavReader> FloatWavReader::open(const std::string& path) {
    auto reader = WavFileReader::open(path);
    if (!reader) {
        return mradm::unexpected{reader.error()};
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

namespace {

[[nodiscard]] const uint8_t* wav_bytes(std::string_view text) {
    return reinterpret_cast<const uint8_t*>(text.data());
}

} // namespace

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
        return mradm::unexpected{wav_error(code, message, path)};
    }
    return {};
}

} // namespace mradm::audio
