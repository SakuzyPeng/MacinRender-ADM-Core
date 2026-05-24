#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <FLAC/metadata.h>
#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm/audio_io.h"
#include "adm/render.h"

namespace {

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() { std::filesystem::remove(path_); }

  private:
    std::filesystem::path path_;
};

bool check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return condition;
}

constexpr std::size_t sample_count(uint32_t channels, uint32_t frames) {
    return static_cast<std::size_t>(channels) * static_cast<std::size_t>(frames);
}

std::filesystem::path temp_path(std::string_view stem, std::string_view ext) {
    static std::atomic<int> s_seq{0};
    const auto name = std::string(stem) + "_" + std::to_string(static_cast<int>(::getpid())) + "_" +
                      std::to_string(s_seq.fetch_add(1)) + std::string(ext);
    return std::filesystem::temp_directory_path() / name;
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_render_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"FlacCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"FlacPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"FlacSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"FlacTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"FlacRenderObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"FlacRenderContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"FlacRenderProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::filesystem::path write_render_fixture() {
    constexpr uint32_t k_ch = 1U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 512U;

    const auto [doc, uid_str] = make_render_doc();
    auto path = temp_path("mr_flac_engine_input", ".wav");

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    auto writer = bw64::writeFile(path.string(), k_ch, k_sr, 24U, chna, axml);
    std::vector<float> samples(k_frames, 0.25F);
    writer->write(samples.data(), k_frames);
    return path;
}

bool has_render_temp_sidecar(const std::filesystem::path& final_path) {
    const auto parent = final_path.parent_path();
    const auto prefix = final_path.stem().string() + ".render_tmp.";
    const auto legacy_fixed_name = final_path.stem().string() + ".render_tmp.wav";
    std::error_code ec;
    const std::filesystem::directory_iterator begin(parent, ec);
    if (ec) {
        return true;
    }
    return std::ranges::any_of(begin, std::filesystem::directory_iterator{}, [&](const auto& entry) {
        const auto name = entry.path().filename().string();
        return name != legacy_fixed_name && name.starts_with(prefix) && entry.path().extension() == ".wav";
    });
}

bool flac_has_tag(const std::string& path, std::string_view prefix) {
    FLAC__Metadata_Chain* chain = FLAC__metadata_chain_new();
    if (chain == nullptr) {
        return false;
    }
    if (FLAC__metadata_chain_read(chain, path.c_str()) == 0) {
        FLAC__metadata_chain_delete(chain);
        return false;
    }

    FLAC__Metadata_Iterator* it = FLAC__metadata_iterator_new();
    if (it == nullptr) {
        FLAC__metadata_chain_delete(chain);
        return false;
    }
    FLAC__metadata_iterator_init(it, chain);

    bool found = false;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while): libFLAC iterator is advanced after inspecting current block.
    do {
        if (FLAC__metadata_iterator_get_block_type(it) != FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            continue;
        }
        const FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(it);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): libFLAC exposes metadata payloads as a C union.
        const auto& vc = block->data.vorbis_comment;
        for (uint32_t i = 0; i < vc.num_comments; ++i) {
            const std::string_view entry(reinterpret_cast<const char*>(vc.comments[i].entry),
                                         static_cast<std::size_t>(vc.comments[i].length));
            found = found || entry.starts_with(prefix);
        }
    } while (!found && FLAC__metadata_iterator_next(it) != 0);

    FLAC__metadata_iterator_delete(it);
    FLAC__metadata_chain_delete(chain);
    return found;
}

// ── round-trip ────────────────────────────────────────────────────────────────
// Write float32 samples → FloatFlacWriter → FloatFlacReader → compare.
// Tolerance is 1e-6, far above the 24-bit quantisation error (~6e-8).

bool verify_flac_roundtrip() {
    constexpr uint32_t k_ch = 2U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 1024U;
    constexpr float k_tol = 1e-6F;

    const std::string path = "/tmp/mr_flac_roundtrip_test.flac";
    FileGuard guard(path);

    std::vector<float> orig(sample_count(k_ch, k_frames));
    for (std::size_t i = 0; i < orig.size(); ++i) {
        orig[i] = 0.9F * std::sin(static_cast<float>(i) * 0.1F);
    }

    {
        auto wr = mradm::audio::FloatFlacWriter::open(path, k_ch, k_sr);
        if (!check(wr.has_value(), "FloatFlacWriter::open failed")) {
            return false;
        }
        wr->write(orig.data(), k_frames);
    }

    auto rd = mradm::audio::FloatFlacReader::open(path);
    if (!check(rd.has_value(), "FloatFlacReader::open failed")) {
        return false;
    }
    if (!check(rd->channels() == k_ch, "channels mismatch")) {
        return false;
    }
    if (!check(rd->sample_rate() == k_sr, "sample_rate mismatch")) {
        return false;
    }
    if (!check(rd->frame_count() == k_frames, "frame_count mismatch")) {
        return false;
    }

    std::vector<float> back(sample_count(k_ch, k_frames));
    rd->read(back.data(), k_frames);

    for (std::size_t i = 0; i < orig.size(); ++i) {
        if (std::abs(orig[i] - back[i]) > k_tol) {
            std::cerr << "FAIL: sample " << i << ": orig=" << orig[i] << " back=" << back[i] << "\n";
            return false;
        }
    }
    return true;
}

// ── apply_gain_to_file on FLAC ────────────────────────────────────────────────
// Constant-amplitude file → gain 0.5 → verify amplitude halved within tolerance.

bool verify_flac_gain() {
    constexpr uint32_t k_ch = 1U;
    constexpr uint32_t k_sr = 44100U;
    constexpr uint32_t k_frames = 512U;
    constexpr float k_amp = 0.5F;
    constexpr float k_gain = 0.5F;
    constexpr float k_tol = 2e-6F;

    const std::string path = "/tmp/mr_flac_gain_test.flac";
    FileGuard guard(path);

    {
        std::vector<float> samples(sample_count(k_ch, k_frames), k_amp);
        auto wr = mradm::audio::FloatFlacWriter::open(path, k_ch, k_sr);
        if (!check(wr.has_value(), "FloatFlacWriter::open (gain) failed")) {
            return false;
        }
        wr->write(samples.data(), k_frames);
    }

    auto res = mradm::audio::apply_gain_to_file(path, k_gain);
    if (!check(res.has_value(), "apply_gain_to_file on FLAC failed")) {
        return false;
    }

    auto rd = mradm::audio::FloatFlacReader::open(path);
    if (!check(rd.has_value(), "FloatFlacReader::open after gain failed")) {
        return false;
    }
    if (!check(rd->frame_count() == k_frames, "frame_count changed after gain")) {
        return false;
    }

    std::vector<float> back(sample_count(k_ch, k_frames));
    rd->read(back.data(), k_frames);

    const float expected = k_amp * k_gain;
    const auto bad =
        std::ranges::find_if(back, [expected](float sample) { return std::abs(sample - expected) > k_tol; });
    if (bad != back.end()) {
        const auto i = static_cast<std::size_t>(std::distance(back.begin(), bad));
        std::cerr << "FAIL: gain sample " << i << ": got=" << *bad << " expected=" << expected << "\n";
        return false;
    }
    return true;
}

// ── Vorbis Comment: single VC block, required tags present ───────────────────

bool verify_flac_vorbis_comment() {
    constexpr uint32_t k_ch = 2U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 256U;

    const std::string path = "/tmp/mr_flac_meta_test.flac";
    FileGuard guard(path);

    {
        std::vector<float> silence(sample_count(k_ch, k_frames), 0.0F);
        auto wr = mradm::audio::FloatFlacWriter::open(path, k_ch, k_sr);
        if (!check(wr.has_value(), "FloatFlacWriter::open (meta) failed")) {
            return false;
        }
        wr->write(silence.data(), k_frames);
    }

    mradm::audio::MetadataFields meta;
    meta.encoder = "TestEncoder";
    meta.date_utc = "2026-01-01T00:00:00Z";
    meta.renderer = "test-backend";
    meta.output_layout = "0+2+0";
    meta.lufs = -23.0;
    meta.peak_dbtp = -1.0;

    auto mres = mradm::audio::write_file_metadata(path, meta);
    if (!check(mres.has_value(), "write_file_metadata on FLAC failed")) {
        return false;
    }

    FLAC__Metadata_Chain* chain = FLAC__metadata_chain_new();
    if (!check(chain != nullptr, "chain alloc")) {
        return false;
    }
    if (!check(FLAC__metadata_chain_read(chain, path.c_str()) != 0, "chain read")) {
        FLAC__metadata_chain_delete(chain);
        return false;
    }

    FLAC__Metadata_Iterator* it = FLAC__metadata_iterator_new();
    if (!check(it != nullptr, "iterator alloc")) {
        FLAC__metadata_chain_delete(chain);
        return false;
    }
    FLAC__metadata_iterator_init(it, chain);

    int vc_count = 0;
    bool has_encoder = false;
    bool has_date = false;
    bool has_comment = false;
    bool has_channel_mask = false;

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while): libFLAC iterator is advanced after inspecting current block.
    do {
        if (FLAC__metadata_iterator_get_block_type(it) != FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            continue;
        }
        ++vc_count;
        const FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(it);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): libFLAC exposes metadata payloads as a C union.
        const auto& vc = block->data.vorbis_comment;
        for (uint32_t i = 0; i < vc.num_comments; ++i) {
            const std::string_view entry(reinterpret_cast<const char*>(vc.comments[i].entry),
                                         static_cast<std::size_t>(vc.comments[i].length));
            if (entry.starts_with("ENCODER=")) {
                has_encoder = true;
            }
            if (entry.starts_with("DATE=")) {
                has_date = true;
            }
            if (entry.starts_with("COMMENT=")) {
                has_comment = true;
            }
            if (entry.starts_with("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=")) {
                has_channel_mask = true;
            }
        }
    } while (FLAC__metadata_iterator_next(it) != 0);

    FLAC__metadata_iterator_delete(it);
    FLAC__metadata_chain_delete(chain);

    bool ok = true;
    ok &= check(vc_count == 1, "exactly one Vorbis Comment block");
    ok &= check(has_encoder, "ENCODER tag present");
    ok &= check(has_date, "DATE tag present");
    ok &= check(has_comment, "COMMENT tag present");
    ok &= check(has_channel_mask, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK tag present for stereo");
    return ok;
}

// ── WAVEFORMATEXTENSIBLE_CHANNEL_MASK exact value for stereo ─────────────────

bool verify_flac_channel_mask_value() {
    constexpr uint32_t k_ch = 2U;
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 128U;

    const std::string path = "/tmp/mr_flac_mask_test.flac";
    FileGuard guard(path);

    {
        std::vector<float> silence(sample_count(k_ch, k_frames), 0.0F);
        auto wr = mradm::audio::FloatFlacWriter::open(path, k_ch, k_sr);
        if (!check(wr.has_value(), "FloatFlacWriter::open (mask) failed")) {
            return false;
        }
        wr->write(silence.data(), k_frames);
    }

    mradm::audio::MetadataFields meta;
    meta.encoder = "TestEncoder";
    meta.date_utc = "2026-01-01T00:00:00Z";
    meta.renderer = "test";
    meta.output_layout = "0+2+0";

    auto mres = mradm::audio::write_file_metadata(path, meta);
    if (!check(mres.has_value(), "write_file_metadata (mask) failed")) {
        return false;
    }

    FLAC__Metadata_Chain* chain = FLAC__metadata_chain_new();
    if (!check(chain != nullptr, "chain alloc (mask)")) {
        return false;
    }
    if (!check(FLAC__metadata_chain_read(chain, path.c_str()) != 0, "chain read (mask)")) {
        FLAC__metadata_chain_delete(chain);
        return false;
    }

    FLAC__Metadata_Iterator* it = FLAC__metadata_iterator_new();
    FLAC__metadata_iterator_init(it, chain);

    std::string mask_value;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while): libFLAC iterator is advanced after inspecting current block.
    do {
        if (FLAC__metadata_iterator_get_block_type(it) != FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            continue;
        }
        const FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(it);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): libFLAC exposes metadata payloads as a C union.
        const auto& vc = block->data.vorbis_comment;
        constexpr std::string_view k_prefix = "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=";
        for (uint32_t i = 0; i < vc.num_comments; ++i) {
            const std::string_view entry(reinterpret_cast<const char*>(vc.comments[i].entry),
                                         static_cast<std::size_t>(vc.comments[i].length));
            if (entry.starts_with(k_prefix)) {
                mask_value = std::string(entry.substr(k_prefix.size()));
            }
        }
        break;
    } while (FLAC__metadata_iterator_next(it) != 0);

    FLAC__metadata_iterator_delete(it);
    FLAC__metadata_chain_delete(chain);

    if (mask_value != "0x00000003") {
        std::cerr << "FAIL: channel mask: expected '0x00000003' got '" << mask_value << "'\n";
        return false;
    }
    return true;
}

// ── RenderService final .flac pipeline ───────────────────────────────────────
// Render to a unique float32 temp WAV, encode final FLAC, clean temp, write final metadata.

bool verify_render_service_flac_final_pipeline() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);

    const auto out_path = temp_path("mr_flac_engine_out", ".flac");
    const FileGuard out_guard(out_path);

    const auto fixed_old_tmp = out_path.parent_path() / (out_path.stem().string() + std::string(".render_tmp.wav"));
    {
        std::ofstream stale(fixed_old_tmp, std::ios::binary);
        stale << "stale";
    }
    const FileGuard stale_guard(fixed_old_tmp);

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::saf;
    req.options.output_layout = "0+2+0";
    req.options.peak_limit = false;
    req.options.measure_loudness = false;
    req.options.internal_allow_speaker_stereo = true;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!check(res.success(), "RenderService .flac render failed")) {
        return false;
    }
    if (!check(std::filesystem::exists(out_path), "final FLAC output missing")) {
        return false;
    }
    if (!check(std::filesystem::exists(fixed_old_tmp), "legacy fixed temp path was touched")) {
        return false;
    }
    {
        std::ifstream stale(fixed_old_tmp, std::ios::binary);
        std::string content;
        stale >> content;
        if (!check(content == "stale", "legacy fixed temp path content changed")) {
            return false;
        }
    }
    if (!check(!has_render_temp_sidecar(out_path), "unique render temp sidecar was not cleaned")) {
        return false;
    }

    auto rd = mradm::audio::FloatFlacReader::open(out_path.string());
    if (!check(rd.has_value(), "RenderService final FLAC not readable")) {
        return false;
    }
    if (!check(rd->channels() == 2U, "RenderService final FLAC channel count")) {
        return false;
    }
    if (!check(flac_has_tag(out_path.string(), "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x00000003"),
               "RenderService final FLAC channel mask metadata")) {
        return false;
    }
    return true;
}

bool verify_render_service_flac_rejects_more_than_8_channels() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);

    const auto out_path = temp_path("mr_flac_engine_too_many_channels", ".flac");
    const FileGuard out_guard(out_path);

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::saf;
    req.options.output_layout = "7.1.4";
    req.options.peak_limit = false;
    req.options.measure_loudness = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!check(!res.success(), "RenderService accepted >8ch FLAC layout")) {
        return false;
    }
    if (!check(res.error.code == mradm::ErrorCode::unsupported, "RenderService >8ch FLAC error code")) {
        return false;
    }
    if (!check(res.error.message.find("non-height layouts") != std::string::npos,
               "RenderService >8ch FLAC error message")) {
        return false;
    }
    if (!check(!std::filesystem::exists(out_path), "RenderService created unsupported FLAC output")) {
        return false;
    }
    if (!check(!has_render_temp_sidecar(out_path), "RenderService created render temp for unsupported FLAC layout")) {
        return false;
    }
    return true;
}

bool verify_render_service_flac_rejects_height_layouts() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);

    const auto out_path = temp_path("mr_flac_engine_height_layout", ".flac");
    const FileGuard out_guard(out_path);

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.output_layout = "5.1.2";
    req.options.peak_limit = false;
    req.options.measure_loudness = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!check(!res.success(), "RenderService accepted height FLAC layout")) {
        return false;
    }
    if (!check(res.error.code == mradm::ErrorCode::unsupported, "RenderService height FLAC error code")) {
        return false;
    }
    if (!check(res.error.message.find("non-height layouts") != std::string::npos,
               "RenderService height FLAC error message")) {
        return false;
    }
    if (!check(!std::filesystem::exists(out_path), "RenderService created unsupported height FLAC output")) {
        return false;
    }
    if (!check(!has_render_temp_sidecar(out_path), "RenderService created temp for unsupported height FLAC layout")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_flac_roundtrip();
    ok &= verify_flac_gain();
    ok &= verify_flac_vorbis_comment();
    ok &= verify_flac_channel_mask_value();
    ok &= verify_render_service_flac_final_pipeline();
    ok &= verify_render_service_flac_rejects_more_than_8_channels();
    ok &= verify_render_service_flac_rejects_height_layouts();
    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "FLAC I/O smoke tests passed (6/6)\n";
    return EXIT_SUCCESS;
}
