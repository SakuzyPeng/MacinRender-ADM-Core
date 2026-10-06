#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "audio_io_internal.h"
#include "wav_backend.h"

namespace mradm::audio {

namespace {

class TempPathGuard {
  public:
    explicit TempPathGuard(std::filesystem::path path, bool active = true) : path_(std::move(path)), active_(active) {}
    TempPathGuard(const TempPathGuard&) = delete;
    TempPathGuard& operator=(const TempPathGuard&) = delete;
    TempPathGuard(TempPathGuard&&) = delete;
    TempPathGuard& operator=(TempPathGuard&&) = delete;
    ~TempPathGuard() {
        if (active_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    void dismiss() noexcept { active_ = false; }
    void arm() noexcept { active_ = true; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

[[nodiscard]] std::optional<std::filesystem::path> unique_wav_sidecar_path(const std::filesystem::path& original,
                                                                           std::string_view purpose);
[[nodiscard]] Result<void> replace_wav_with_rewrite(const std::filesystem::path& original,
                                                    const std::filesystem::path& rewritten,
                                                    TempPathGuard& rewritten_guard);

void emit_wav_progress(ProgressSink* progress,
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

} // namespace

// ── FloatWavWriter ────────────────────────────────────────────────────────────

struct FloatWavWriter::Impl {
    std::unique_ptr<RustWavWriter> writer;
    std::optional<Result<void>> finish_result;
};

Result<FloatWavWriter> FloatWavWriter::open(const std::string& path, uint32_t channels, uint32_t sample_rate) {
    auto writer = RustWavWriter::create(path, channels, sample_rate, 32U, true, false);
    if (!writer) {
        return tl::unexpected{writer.error()};
    }
    FloatWavWriter w;
    w.impl_ = std::make_unique<Impl>();
    w.impl_->writer = std::move(*writer);
    return w;
}

FloatWavWriter::~FloatWavWriter() {
    if (impl_ && !impl_->finish_result.has_value()) {
        // Best effort for callers that rely on RAII; finish() reports the same failure explicitly.
        static_cast<void>(impl_->writer->finish());
    }
}

FloatWavWriter::FloatWavWriter(FloatWavWriter&&) noexcept = default;
FloatWavWriter& FloatWavWriter::operator=(FloatWavWriter&&) noexcept = default;

uint64_t FloatWavWriter::write(const float* samples, uint64_t frame_count) {
    if (impl_->finish_result.has_value() || !impl_->writer->write(samples, frame_count)) {
        return 0;
    }
    return frame_count;
}

Result<void> FloatWavWriter::finish() {
    if (!impl_->finish_result.has_value()) {
        // Rust consumes the writer even on failure; repeated calls must preserve that result.
        impl_->finish_result.emplace(impl_->writer->finish());
    }
    return *impl_->finish_result;
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

// ── downconvert_to_int ────────────────────────────────────────────────────────

Result<void> downconvert_to_int(const std::string& path,
                                uint16_t bit_depth,
                                const std::stop_token& cancel_token,
                                ProgressSink* progress,
                                RenderOperation operation) {
    try {
        if (cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
        }
        if (bit_depth != 16U && bit_depth != 24U && bit_depth != 32U) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("integer PCM bit depth must be 16, 24, or 32; got {}", bit_depth),
                              "path=" + path);
        }

        const auto tmp_path = unique_wav_sidecar_path(path, "bitdepth_tmp");
        if (!tmp_path) {
            return make_error(ErrorCode::io_error, "cannot allocate integer WAVE temporary path", "path=" + path);
        }
        TempPathGuard tmp_guard{*tmp_path, false};
        uint64_t total_frames = 0;
        {
            // reader 与 writer 都置于此块内，块结束即关闭句柄；之后才能在 Windows 上
            // rename 顶替原文件（POSIX 可 rename 顶替正打开的文件，Windows 会 Access denied）。
            auto reader_res = FloatWavReader::open(path);
            if (!reader_res) {
                return tl::unexpected{reader_res.error()};
            }
            auto& reader = *reader_res;

            const uint32_t channels = reader.channels();
            const uint32_t sample_rate = reader.sample_rate();
            total_frames = reader.frame_count();
            emit_wav_progress(progress, operation, 0.0, 0, total_frames, "converting bit depth");

            auto writer_res = IntegerWavWriter::create(tmp_path->string(), channels, sample_rate, bit_depth);
            if (!writer_res) {
                return tl::unexpected{writer_res.error()};
            }
            auto& writer = *writer_res;
            tmp_guard.arm(); // create_new established ownership; never remove a competing file.

            constexpr uint64_t k_block = 4096;
            std::vector<float> buf(static_cast<std::size_t>(channels) * k_block);
            uint64_t left = total_frames;
            uint64_t done = 0;

            while (left > 0) {
                if (cancel_token.stop_requested()) {
                    return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
                }
                const uint64_t n = std::min(k_block, left);
                const uint64_t got = reader.read(buf.data(), n);
                if (got != n) {
                    return make_error(ErrorCode::io_error, "short input read while converting PCM", "path=" + path);
                }
                if (auto result = writer->write(buf.data(), got); !result) {
                    return result;
                }
                left -= got;
                done += got;
                emit_wav_progress(progress,
                                  operation,
                                  static_cast<double>(done) / static_cast<double>(std::max<uint64_t>(1, total_frames)),
                                  done,
                                  total_frames,
                                  "converting bit depth");
            }
            if (auto result = writer->finish(); !result) {
                return result;
            }
        }

        if (cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
        }
        if (auto result = replace_wav_with_rewrite(path, *tmp_path, tmp_guard); !result) {
            return result;
        }
        emit_wav_progress(progress, operation, 1.0, total_frames, total_frames, "bit depth converted");
        return {};

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("bit depth conversion failed: ") + e.what(), "path=" + path);
    }
}


namespace {

[[nodiscard]] const uint8_t* wav_bytes(std::string_view text) {
    return reinterpret_cast<const uint8_t*>(text.data());
}

[[nodiscard]] std::optional<std::filesystem::path> unique_wav_sidecar_path(const std::filesystem::path& original,
                                                                           std::string_view purpose) {
    const auto parent = original.parent_path();
    const auto stem = original.stem().string();
    const auto extension = original.extension().string();
    for (uint32_t index = 0; index < 1024U; ++index) {
        auto candidate = parent / fmt::format("{}.{}.{:04}{}", stem, purpose, index, extension);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] Result<void> replace_wav_with_rewrite(const std::filesystem::path& original,
                                                    const std::filesystem::path& rewritten,
                                                    TempPathGuard& rewritten_guard) {
    const auto backup = unique_wav_sidecar_path(original, "layout_backup");
    if (!backup.has_value()) {
        return make_error(ErrorCode::io_error, "cannot allocate WAV layout backup path", "path=" + original.string());
    }
    TempPathGuard backup_guard{*backup};
    std::error_code ec;
    std::filesystem::rename(original, *backup, ec);
    if (ec) {
        return make_error(ErrorCode::io_error,
                          "cannot move original WAV before layout replacement: " + ec.message(),
                          "path=" + original.string());
    }
    std::filesystem::rename(rewritten, original, ec);
    if (ec) {
        std::error_code restore_ec;
        std::filesystem::rename(*backup, original, restore_ec);
        if (restore_ec) {
            backup_guard.dismiss();
            return make_error(ErrorCode::io_error,
                              "cannot install layout-finalized WAV and could not restore original: " + ec.message() +
                                  "; original preserved at " + backup->string(),
                              "path=" + original.string());
        }
        return make_error(
            ErrorCode::io_error, "cannot install layout-finalized WAV: " + ec.message(), "path=" + original.string());
    }
    rewritten_guard.dismiss();
    return {};
}

} // namespace

Result<void> finalize_wav_layout(const std::string& path,
                                 const WavLayoutFinalization& layout,
                                 const std::stop_token& cancel_token,
                                 ProgressSink* progress,
                                 RenderOperation operation) {
    if (cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
    }
    const auto rewritten_path = unique_wav_sidecar_path(path, "layout_tmp");
    if (!rewritten_path.has_value()) {
        return make_error(ErrorCode::io_error, "cannot allocate WAV layout temporary path", "path=" + path);
    }
    TempPathGuard rewritten_guard{*rewritten_path};

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
    const auto source = wav_path_utf8(path);
    const auto target = wav_path_utf8(rewritten_path->string());
    std::array<uint8_t, 512> message{};
    MradmWavLayout* handle = nullptr;
    uint64_t frames = 0;
    auto code = mradm_wav_layout_begin(wav_bytes(source),
                                       source.size(),
                                       wav_bytes(target),
                                       target.size(),
                                       &options,
                                       &handle,
                                       &frames,
                                       message.data(),
                                       message.size());
    if (code != 0) {
        return tl::unexpected{wav_error(code, message, path)};
    }
    std::unique_ptr<MradmWavLayout, decltype(&mradm_wav_layout_destroy)> rewrite{handle, mradm_wav_layout_destroy};

    emit_wav_progress(progress, operation, 0.0, 0U, frames, "finalizing WAV channel layout");
    constexpr uint64_t k_block_frames = 2048U;
    uint64_t frames_done = 0U;
    while (frames_done < frames) {
        if (cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "path=" + path);
        }
        uint64_t copied = 0U;
        code = mradm_wav_layout_step(rewrite.get(), k_block_frames, &copied, message.data(), message.size());
        if (code != 0) {
            return tl::unexpected{wav_error(code, message, path)};
        }
        if (copied == 0U) {
            return make_error(ErrorCode::io_error, "WAV layout rewrite stopped before the last frame", "path=" + path);
        }
        frames_done += copied;
        emit_wav_progress(progress,
                          operation,
                          static_cast<double>(frames_done) / static_cast<double>(frames),
                          frames_done,
                          frames,
                          "finalizing WAV channel layout");
    }
    code = mradm_wav_layout_finish(rewrite.get(), message.data(), message.size());
    rewrite.reset();
    if (code != 0) {
        return tl::unexpected{wav_error(code, message, path)};
    }

    auto replace_res = replace_wav_with_rewrite(path, *rewritten_path, rewritten_guard);
    if (!replace_res) {
        return tl::unexpected{replace_res.error()};
    }
    emit_wav_progress(progress, operation, 1.0, frames, frames, "WAV channel layout finalized");
    return {};
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
    auto reader = RustWavReader::open(path);
    if (!reader) {
        return tl::unexpected{reader.error()};
    }
    WavAdmChunks chunks;
    auto axml = (*reader)->chunk("axml");
    if (!axml) {
        return tl::unexpected{axml.error()};
    }
    chunks.axml = std::move(axml->value_or(std::string{}));
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
    const auto code = mradm_wav_replace_axml(wav_bytes(utf8_source),
                                             utf8_source.size(),
                                             wav_bytes(utf8_target),
                                             utf8_target.size(),
                                             wav_bytes(axml),
                                             axml.size(),
                                             message.data(),
                                             message.size());
    if (code != 0) {
        return tl::unexpected{wav_error(code, message, source + " -> " + target)};
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
