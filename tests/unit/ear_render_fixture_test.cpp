#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// libadm – construct the fixture ADM document
#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>

// libbw64 – write BW64 input fixtures (integer PCM)
#include <bw64/bw64.hpp>

// Our engine
#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/render.h"
#include "adm/render_ear.h"

namespace {

// Records warnings logged by module "ear" for assertion in tests.
class CapturingLogSink final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel level, std::string_view /*module*/, std::string_view message) override {
        if (level == mradm::LogLevel::warning) {
            warnings_.emplace_back(message);
        }
    }
    [[nodiscard]] bool has_warning_containing(std::string_view substr) const {
        return std::ranges::any_of(warnings_,
                                   [&](const std::string& w) { return w.find(substr) != std::string::npos; });
    }

  private:
    std::vector<std::string> warnings_;
};

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

// Build a complete Objects-based ADM document:
//   Programme → Content → Object → UID → PackFormat(OBJECTS) → ChannelFormat(OBJECTS)
//   ChannelFormat has one AudioBlockFormatObjects at center front (az=0, el=0).
// Returns the document and the UID string for CHNA.
std::pair<std::shared_ptr<adm::Document>, std::string> make_objects_doc() {
    auto doc = adm::Document::create();

    // AudioChannelFormat (OBJECTS) with a single block at center position
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"TestCF"}, adm::TypeDefinition::OBJECTS);
    {
        // AudioBlockFormatObjects requires a position in its constructor.
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        cf->add(block);
    }
    doc->add(cf);

    // AudioPackFormat (OBJECTS)
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"TestPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    // AudioStreamFormat and AudioTrackFormat (required for complete ADM)
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"TestSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TestTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    // AudioTrackUid linking the physical track to pack/track formats
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    // AudioObject → Content → Programme
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"TestObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"TestContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"TestProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);

    const std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());
    return {doc, uid_str};
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_two_block_jump_objects_doc() {
    auto doc = adm::Document::create();

    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"EarTwoBlockCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects right_block{adm::SphericalPosition{adm::Azimuth{-30.0F}, adm::Elevation{0.0F}}};
        right_block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{0}}});
        right_block.set(adm::Duration{adm::Time{std::chrono::milliseconds{500}}});
        right_block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(right_block);

        adm::AudioBlockFormatObjects left_block{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}}};
        left_block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{500}}});
        left_block.set(adm::Duration{adm::Time{std::chrono::milliseconds{500}}});
        left_block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(left_block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"EarTwoBlockPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"EarTwoBlockSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"EarTwoBlockTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"EarTwoBlockObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"EarTwoBlockContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"EarTwoBlockProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

struct DirectSpeakersDoc {
    std::shared_ptr<adm::Document> doc;
    std::array<std::string, 2> uids;
};

struct SingleDirectSpeakerDoc {
    std::shared_ptr<adm::Document> doc;
    std::string uid;
};

struct SpeakerSetup {
    const char* label;
    float azimuth;
};

DirectSpeakersDoc make_direct_speakers_doc() {
    auto doc = adm::Document::create();
    std::vector<std::shared_ptr<adm::AudioTrackUid>> uids;

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"DirectSpeakersObject"});

    constexpr std::array<SpeakerSetup, 2> speakers{{{"M+030", 30.0F}, {"M-030", -30.0F}}};

    for (const auto& speaker : speakers) {
        const auto suffix = std::to_string(uids.size() + 1U);
        auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"DsCF" + suffix},
                                                  adm::TypeDefinition::DIRECT_SPEAKERS);
        {
            adm::AudioBlockFormatDirectSpeakers block{adm::SphericalSpeakerPosition{
                adm::Azimuth{speaker.azimuth}, adm::Elevation{0.0F}, adm::Distance{1.0F}}};
            block.add(adm::SpeakerLabel{speaker.label});
            cf->add(block);
        }
        doc->add(cf);

        auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"DsPF" + suffix},
                                               adm::TypeDefinition::DIRECT_SPEAKERS);
        pf->addReference(cf);
        doc->add(pf);

        auto sf =
            adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"DsSF" + suffix}, adm::FormatDefinition::PCM);
        sf->setReference(cf);
        doc->add(sf);

        auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"DsTF" + suffix}, adm::FormatDefinition::PCM);
        tf->setReference(sf);
        sf->addReference(tf);
        doc->add(tf);

        auto uid = adm::AudioTrackUid::create();
        uid->setReference(tf);
        uid->setReference(pf);
        doc->add(uid);
        obj->addReference(uid);
        uids.push_back(uid);
    }

    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"DirectSpeakersContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"DirectSpeakersProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);

    return {doc,
            {adm::formatId(uids.at(0)->get<adm::AudioTrackUidId>()),
             adm::formatId(uids.at(1)->get<adm::AudioTrackUidId>())}};
}

SingleDirectSpeakerDoc make_single_direct_speaker_doc(const char* label, float azimuth, float elevation) {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"SingleDsCF"},
                                              adm::TypeDefinition::DIRECT_SPEAKERS);
    {
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{azimuth}, adm::Elevation{elevation}, adm::Distance{1.0F}}};
        block.add(adm::SpeakerLabel{label});
        cf->add(block);
    }
    doc->add(cf);

    auto pf =
        adm::AudioPackFormat::create(adm::AudioPackFormatName{"SingleDsPF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"SingleDsSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"SingleDsTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"SingleDirectSpeakersObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"SingleDirectSpeakersContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"SingleDirectSpeakersProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::filesystem::path write_input_fixture(const std::string& uid_str, const std::shared_ptr<adm::Document>& doc) {
    auto path = std::filesystem::temp_directory_path() / "mr_ear_fixture_in.wav";

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    const std::string xml_str = xml_buf.str();

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_str);

    // 1 channel, 48 kHz, 1 000 frames: 0.5f DC offset for easy energy checking
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    constexpr uint32_t k_frames = 1000U;
    std::vector<float> samples(k_frames, 0.5F);
    writer->write(samples.data(), k_frames);

    return path;
}

std::filesystem::path write_input_fixture(const std::string& uid_str,
                                          const std::shared_ptr<adm::Document>& doc,
                                          std::string_view stem,
                                          uint16_t sample_rate,
                                          uint32_t frames) {
    auto path = std::filesystem::temp_directory_path() / std::string{stem};

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    const std::string xml_str = xml_buf.str();

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_str);
    auto writer = bw64::writeFile(path.string(), 1U, sample_rate, 24U, chna, axml);
    std::vector<float> samples(frames, 0.5F);
    writer->write(samples.data(), frames);

    return path;
}

std::filesystem::path write_direct_speakers_input_fixture(const DirectSpeakersDoc& fixture) {
    auto path = std::filesystem::temp_directory_path() / "mr_ear_ds_fixture_in.wav";

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, fixture.doc);
    const std::string xml_str = xml_buf.str();

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{
        bw64::AudioId(1U, fixture.uids[0], "", ""), bw64::AudioId(2U, fixture.uids[1], "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_str);

    auto writer = bw64::writeFile(path.string(), 2U, 48000U, 24U, chna, axml);
    constexpr uint32_t k_frames = 1000U;
    std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U);
    for (uint32_t frame = 0; frame < k_frames; ++frame) {
        const auto base = static_cast<std::size_t>(frame) * 2U;
        samples[base] = 0.5F;
        samples[base + 1U] = 0.25F;
    }
    writer->write(samples.data(), k_frames);

    return path;
}

std::vector<double> read_channel_sums(const std::filesystem::path& path, uint32_t channels) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res || reader_res->channels() != channels) {
        return {};
    }
    auto& reader = *reader_res;
    const auto frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(frames * channels);
    reader.read(samples.data(), reader.frame_count());

    std::vector<double> sums(channels, 0.0);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t ch = 0; ch < channels; ++ch) {
            sums[ch] += std::fabs(static_cast<double>(samples[(frame * channels) + ch]));
        }
    }
    return sums;
}

bool verify_objects_render_fixture(const mradm::RenderService& service,
                                   mradm::ProgressSink& progress,
                                   mradm::LogSink& logs) {
    auto [doc, uid_str] = make_objects_doc();
    const auto in_path = write_input_fixture(uid_str, doc);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_fixture_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+2+0";
    request.options.internal_allow_speaker_stereo = true;
    request.options.renderer = mradm::RendererSelection::ear;

    const mradm::RenderResult result = service.render(request, progress, logs);
    bool ok = true;

    if (!result.success()) {
        std::cerr << "FAIL: render failed: " << result.error.message << "\n";
        return false;
    }

    auto out_reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!out_reader_res) {
        std::cerr << "FAIL: cannot open output: " << out_reader_res.error().message << "\n";
        return false;
    }
    auto& out_reader = *out_reader_res;

    ok &= check(out_reader.channels() == 2U, "output has 2 channels");
    ok &= check(out_reader.sample_rate() == 48000U, "output sample rate == 48000");
    ok &= check(out_reader.frame_count() == 1000U, "output frame count == 1000");

    if (ok) {
        const auto n_frames = static_cast<std::size_t>(out_reader.frame_count());
        std::vector<float> out_samples(n_frames * 2U);
        out_reader.read(out_samples.data(), out_reader.frame_count());

        double sum_l = 0.0;
        double sum_r = 0.0;
        for (std::size_t f = 0; f < n_frames; f++) {
            sum_l += std::fabs(static_cast<double>(out_samples[2U * f]));
            sum_r += std::fabs(static_cast<double>(out_samples[(2U * f) + 1U]));
        }
        ok &= check(sum_l > 0.0, "left channel is not silent");
        ok &= check(sum_r > 0.0, "right channel is not silent");

        const double ratio = (sum_l > 0.0) ? (sum_r / sum_l) : 0.0;
        ok &= check(ratio > 0.95 && ratio < 1.05, "L≈R energy for center object");
    }

    return ok;
}

bool verify_direct_speakers_render_fixture(const mradm::RenderService& service,
                                           mradm::ProgressSink& progress,
                                           mradm::LogSink& logs) {
    const auto ds_fixture = make_direct_speakers_doc();
    const auto ds_in_path = write_direct_speakers_input_fixture(ds_fixture);
    FileGuard ds_in_guard{ds_in_path};

    const auto ds_out_path = std::filesystem::temp_directory_path() / "mr_ear_ds_fixture_out.wav";
    FileGuard ds_out_guard{ds_out_path};

    mradm::RenderRequest ds_request;
    ds_request.input_path = ds_in_path;
    ds_request.output_path = ds_out_path;
    ds_request.options.output_layout = "0+2+0";
    ds_request.options.internal_allow_speaker_stereo = true;
    ds_request.options.renderer = mradm::RendererSelection::ear;

    const mradm::RenderResult ds_result = service.render(ds_request, progress, logs);
    bool ok = true;

    if (!ds_result.success()) {
        std::cerr << "FAIL: DirectSpeakers render failed: " << ds_result.error.message << "\n";
        return false;
    }

    auto ds_reader_res = mradm::audio::FloatWavReader::open(ds_out_path.string());
    if (!ds_reader_res) {
        std::cerr << "FAIL: cannot open DirectSpeakers output: " << ds_reader_res.error().message << "\n";
        return false;
    }
    auto& ds_reader = *ds_reader_res;

    ok &= check(ds_reader.channels() == 2U, "DS output has 2 channels");
    ok &= check(ds_reader.sample_rate() == 48000U, "DS output sample rate == 48000");
    ok &= check(ds_reader.frame_count() == 1000U, "DS output frame count == 1000");

    if (ok) {
        const auto n_frames = static_cast<std::size_t>(ds_reader.frame_count());
        std::vector<float> ds_samples(n_frames * 2U);
        ds_reader.read(ds_samples.data(), ds_reader.frame_count());

        double sum_l = 0.0;
        double sum_r = 0.0;
        for (std::size_t f = 0; f < n_frames; f++) {
            sum_l += std::fabs(static_cast<double>(ds_samples[2U * f]));
            sum_r += std::fabs(static_cast<double>(ds_samples[(2U * f) + 1U]));
        }
        ok &= check(sum_l > 0.0, "DS left channel is not silent");
        ok &= check(sum_r > 0.0, "DS right channel is not silent");
    }

    return ok;
}

bool verify_ear_custom_916_objects(const mradm::RenderService& service,
                                   mradm::ProgressSink& progress,
                                   mradm::LogSink& logs) {
    auto [doc, uid_str] = make_objects_doc();
    const auto in_path = write_input_fixture(uid_str, doc, "mr_ear_916_obj_in.wav", 48000U, 1000U);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_916_obj_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "9.1.6";
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: EAR 9.1.6 object render failed: " << res.error.message << "\n";
        return false;
    }

    const auto sums = read_channel_sums(out_path, 16U);
    bool ok = true;
    ok &= check(sums.size() == 16U, "EAR 9.1.6 object: output is 16-channel");
    if (sums.size() == 16U) {
        ok &= check(sums[2U] > 0.0, "EAR 9.1.6 object: center channel has energy");
        ok &= check(sums[3U] < 1.0e-9, "EAR 9.1.6 object: LFE is silent");
    }
    return ok;
}

bool verify_ear_custom_916_direct_speakers(const mradm::RenderService& service,
                                           mradm::ProgressSink& progress,
                                           mradm::LogSink& logs) {
    auto fixture = make_single_direct_speaker_doc("U+110", 110.0F, 45.0F);
    const auto in_path = write_input_fixture(fixture.uid, fixture.doc, "mr_ear_916_ds_in.wav", 48000U, 1000U);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_916_ds_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "9.1.6";
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: EAR 9.1.6 DirectSpeakers render failed: " << res.error.message << "\n";
        return false;
    }

    constexpr std::size_t k_ltm_ch = 12U;
    const auto sums = read_channel_sums(out_path, 16U);
    bool ok = true;
    ok &= check(sums.size() == 16U, "EAR 9.1.6 DirectSpeakers: output is 16-channel");
    if (sums.size() == 16U) {
        ok &= check(sums[k_ltm_ch] > 0.0, "EAR 9.1.6 DirectSpeakers: U+110 routes to Ltm");
        ok &= check(sums[3U] < 1.0e-9, "EAR 9.1.6 DirectSpeakers: LFE is silent");
    }
    return ok;
}

bool verify_ear_direct_speakers_lfe_alias(const mradm::RenderService& service,
                                          mradm::ProgressSink& progress,
                                          mradm::LogSink& logs) {
    auto fixture = make_single_direct_speaker_doc("RC_LFE", 45.0F, -35.264389F);
    const auto in_path = write_input_fixture(fixture.uid, fixture.doc, "mr_ear_ds_lfe_alias_in.wav", 48000U, 1000U);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_ds_lfe_alias_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+5+0";
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: EAR DirectSpeakers LFE alias render failed: " << res.error.message << "\n";
        return false;
    }

    constexpr std::size_t k_lfe_ch = 3U;
    const auto sums = read_channel_sums(out_path, 6U);
    bool ok = true;
    ok &= check(sums.size() == 6U, "EAR DirectSpeakers LFE alias: output is 5.1");
    if (sums.size() == 6U) {
        ok &= check(sums[k_lfe_ch] > 0.0, "EAR DirectSpeakers RC_LFE routes to LFE");
        for (std::size_t ch = 0; ch < sums.size(); ++ch) {
            if (ch == k_lfe_ch) {
                continue;
            }
            ok &= check(sums[ch] < 1.0e-9, "EAR DirectSpeakers RC_LFE does not leak to non-LFE channels");
        }
    }
    return ok;
}

// Write a 2-channel BW64 fixture with one Objects UID (ch0) and one DS UID (ch1).
std::filesystem::path write_mixed_input_fixture(const std::shared_ptr<adm::Document>& doc,
                                                const std::string& obj_uid_str,
                                                const std::string& ds_uid_str) {
    auto path = std::filesystem::temp_directory_path() / "mr_ear_mixed_fixture_in.wav";

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);

    auto chna = std::make_shared<bw64::ChnaChunk>(
        std::vector<bw64::AudioId>{bw64::AudioId(1U, obj_uid_str, "", ""), bw64::AudioId(2U, ds_uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    constexpr uint32_t k_frames = 1000U;
    auto writer = bw64::writeFile(path.string(), 2U, 48000U, 24U, chna, axml);
    std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U);
    for (uint32_t frame = 0; frame < k_frames; ++frame) {
        const auto base = static_cast<std::size_t>(frame) * 2U;
        samples[base] = 0.5F;      // Objects channel
        samples[base + 1U] = 0.4F; // DS channel
    }
    writer->write(samples.data(), k_frames);

    return path;
}

struct MixedTrack {
    std::shared_ptr<adm::AudioTrackUid> uid;
    std::shared_ptr<adm::AudioObject> object;
};

struct MixedDoc {
    std::shared_ptr<adm::Document> doc;
    std::string obj_uid_str;
    std::string ds_uid_str;
};

MixedTrack add_mixed_objects_track(const std::shared_ptr<adm::Document>& doc) {
    auto obj_cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"MixObjCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        obj_cf->add(block);
    }
    doc->add(obj_cf);
    auto obj_pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"MixObjPF"}, adm::TypeDefinition::OBJECTS);
    obj_pf->addReference(obj_cf);
    doc->add(obj_pf);
    auto obj_sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"MixObjSF"}, adm::FormatDefinition::PCM);
    obj_sf->setReference(obj_cf);
    doc->add(obj_sf);
    auto obj_tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"MixObjTF"}, adm::FormatDefinition::PCM);
    obj_tf->setReference(obj_sf);
    obj_sf->addReference(obj_tf);
    doc->add(obj_tf);
    auto obj_uid = adm::AudioTrackUid::create();
    obj_uid->setReference(obj_tf);
    obj_uid->setReference(obj_pf);
    doc->add(obj_uid);
    auto obj_audio_object = adm::AudioObject::create(adm::AudioObjectName{"MixObjObject"});
    obj_audio_object->addReference(obj_uid);
    doc->add(obj_audio_object);

    return {obj_uid, obj_audio_object};
}

MixedTrack add_mixed_direct_speakers_track(const std::shared_ptr<adm::Document>& doc) {
    auto ds_cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"MixDsCF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    {
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}, adm::Distance{1.0F}}};
        block.add(adm::SpeakerLabel{"M+030"});
        ds_cf->add(block);
    }
    doc->add(ds_cf);
    auto ds_pf =
        adm::AudioPackFormat::create(adm::AudioPackFormatName{"MixDsPF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    ds_pf->addReference(ds_cf);
    doc->add(ds_pf);
    auto ds_sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"MixDsSF"}, adm::FormatDefinition::PCM);
    ds_sf->setReference(ds_cf);
    doc->add(ds_sf);
    auto ds_tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"MixDsTF"}, adm::FormatDefinition::PCM);
    ds_tf->setReference(ds_sf);
    ds_sf->addReference(ds_tf);
    doc->add(ds_tf);
    auto ds_uid = adm::AudioTrackUid::create();
    ds_uid->setReference(ds_tf);
    ds_uid->setReference(ds_pf);
    doc->add(ds_uid);
    auto ds_audio_object = adm::AudioObject::create(adm::AudioObjectName{"MixDsObject"});
    ds_audio_object->addReference(ds_uid);
    doc->add(ds_audio_object);

    return {ds_uid, ds_audio_object};
}

MixedDoc make_mixed_doc() {
    auto doc = adm::Document::create();
    auto obj_track = add_mixed_objects_track(doc);
    auto ds_track = add_mixed_direct_speakers_track(doc);

    auto content = adm::AudioContent::create(adm::AudioContentName{"MixedContent"});
    content->addReference(obj_track.object);
    content->addReference(ds_track.object);
    doc->add(content);
    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"MixedProgramme"});
    programme->addReference(content);
    doc->add(programme);
    adm::reassignIds(doc);

    return {doc,
            adm::formatId(obj_track.uid->get<adm::AudioTrackUidId>()),
            adm::formatId(ds_track.uid->get<adm::AudioTrackUidId>())};
}

bool verify_mixed_render_fixture(const mradm::RenderService& service,
                                 mradm::ProgressSink& progress,
                                 mradm::LogSink& logs) {
    const auto fixture = make_mixed_doc();
    const auto in_path = write_mixed_input_fixture(fixture.doc, fixture.obj_uid_str, fixture.ds_uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_mixed_fixture_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+2+0";
    request.options.internal_allow_speaker_stereo = true;
    request.options.renderer = mradm::RendererSelection::ear;

    const mradm::RenderResult result = service.render(request, progress, logs);
    bool ok = true;

    if (!result.success()) {
        std::cerr << "FAIL: mixed render failed: " << result.error.message << "\n";
        return false;
    }

    auto out_reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!out_reader_res) {
        std::cerr << "FAIL: cannot open mixed output: " << out_reader_res.error().message << "\n";
        return false;
    }
    auto& out_reader = *out_reader_res;

    ok &= check(out_reader.channels() == 2U, "mixed output: 2 channels");
    ok &= check(out_reader.sample_rate() == 48000U, "mixed output: sample rate 48000");
    ok &= check(out_reader.frame_count() == 1000U, "mixed output: 1000 frames");

    if (ok) {
        const auto n_frames = static_cast<std::size_t>(out_reader.frame_count());
        std::vector<float> out_samples(n_frames * 2U);
        out_reader.read(out_samples.data(), out_reader.frame_count());

        double sum_l = 0.0;
        double sum_r = 0.0;
        for (std::size_t f = 0; f < n_frames; f++) {
            sum_l += std::fabs(static_cast<double>(out_samples[2U * f]));
            sum_r += std::fabs(static_cast<double>(out_samples[(2U * f) + 1U]));
        }
        // Objects at center contributes equally to L and R; both must be non-silent.
        ok &= check(sum_l > 0.0, "mixed output: left channel non-silent");
        ok &= check(sum_r > 0.0, "mixed output: right channel non-silent");
    }

    return ok;
}

// ── M5: Cartesian Objects ─────────────────────────────────────────────────────

// Objects doc with CartesianPosition (X=0, Y=1, Z=0) = front center.
std::pair<std::shared_ptr<adm::Document>, std::string> make_cartesian_objects_doc() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"CartCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::CartesianPosition{adm::X{0.0F}, adm::Y{1.0F}, adm::Z{0.0F}}};
        block.set(adm::Gain{1.0F});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"CartPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"CartSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"CartTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"CartObject"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"CartContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"CartProgramme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// Verifies that Cartesian Objects (which would cause libear to throw not_implemented
// before M5) are now successfully converted to polar and rendered.
bool verify_ear_cartesian_objects(const mradm::RenderService& service,
                                  mradm::NullProgressSink& progress,
                                  mradm::NullLogSink& logs) {
    auto [doc, uid_str] = make_cartesian_objects_doc();

    const auto in_path = std::filesystem::temp_directory_path() / "mr_ear_cart_in.wav";
    {
        std::ostringstream xml_buf;
        adm::writeXml(xml_buf, doc);
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
        auto writer = bw64::writeFile(in_path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> samples(1000U, 0.5F);
        writer->write(samples.data(), 1000U);
    }
    FileGuard in_g{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_cart_out.wav";
    FileGuard out_g{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+2+0";
    req.options.internal_allow_speaker_stereo = true;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: Cartesian Objects EAR render failed: " << res.error.message << "\n";
        return false;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        return false;
    }
    auto& reader = *reader_res;
    bool ok = check(reader.frame_count() == 1000U, "cartesian: output has 1000 frames");

    if (ok) {
        const auto n = static_cast<std::size_t>(reader.frame_count());
        std::vector<float> buf(n * 2U);
        reader.read(buf.data(), reader.frame_count());
        double sum_l = 0.0;
        double sum_r = 0.0;
        for (std::size_t f = 0; f < n; ++f) {
            sum_l += std::fabs(static_cast<double>(buf[2U * f]));
            sum_r += std::fabs(static_cast<double>(buf[(2U * f) + 1U]));
        }
        ok &= check(sum_l > 0.0, "cartesian: left channel not silent");
        ok &= check(sum_r > 0.0, "cartesian: right channel not silent");
        const double ratio = (sum_l > 0.0) ? (sum_r / sum_l) : 0.0;
        ok &= check(ratio > 0.9 && ratio < 1.1, "cartesian: L≈R energy for front-center object (Y=1)");
    }
    return ok;
}

// ── M7: AudioObject positionOffset ────────────────────────────────────────────

// Objects doc with az=30 block (left-biased) + positionOffset azimuth=-30 → center.
std::pair<std::shared_ptr<adm::Document>, std::string> make_offset_objects_doc() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"OffCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"OffPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"OffSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"OffTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"OffObject"});
    obj->addReference(uid);
    // positionOffset shifts az=30 by -30 → effective az=0 (front center)
    obj->set(adm::SphericalPositionOffset{adm::AzimuthOffset{-30.0F}});
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"OffContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"OffProgramme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// Verifies that AudioObject positionOffset is applied: block at az=30 with
// offset az=-30 should produce L≈R output (effective position = center).
bool verify_position_offset(const mradm::RenderService& service,
                            mradm::NullProgressSink& progress,
                            mradm::NullLogSink& logs) {
    auto [doc, uid_str] = make_offset_objects_doc();

    const auto in_path = std::filesystem::temp_directory_path() / "mr_ear_offset_in.wav";
    {
        std::ostringstream xml_buf;
        adm::writeXml(xml_buf, doc);
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
        auto writer = bw64::writeFile(in_path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> samples(1000U, 0.5F);
        writer->write(samples.data(), 1000U);
    }
    FileGuard in_g{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_offset_out.wav";
    FileGuard out_g{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+2+0";
    req.options.internal_allow_speaker_stereo = true;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: positionOffset EAR render failed: " << res.error.message << "\n";
        return false;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        return false;
    }
    auto& reader = *reader_res;
    const auto n = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> buf(n * 2U);
    reader.read(buf.data(), reader.frame_count());

    double sum_l = 0.0;
    double sum_r = 0.0;
    for (std::size_t f = 0; f < n; ++f) {
        sum_l += std::fabs(static_cast<double>(buf[2U * f]));
        sum_r += std::fabs(static_cast<double>(buf[(2U * f) + 1U]));
    }
    bool ok = true;
    ok &= check(sum_l > 0.0, "positionOffset: output not silent");
    const double ratio = (sum_l > 0.0) ? (sum_r / sum_l) : 0.0;
    ok &= check(ratio > 0.9 && ratio < 1.1, "positionOffset: L≈R for az=30+offset(-30)=center");
    return ok;
}

// ── M4: Diffuse bus tests ─────────────────────────────────────────────────────

std::filesystem::path write_diffuse_fixture(float diffuse_value, uint32_t frames = 1000U) {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"DiffCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0});
        block.set(adm::Diffuse{diffuse_value});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"DiffPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"DiffSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"DiffTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"DiffObj"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"DiffContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"DiffProg"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    const std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());

    auto path = std::filesystem::temp_directory_path() / "mr_ear_diffuse_in.wav";
    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    std::vector<float> samples(frames, 0.5F);
    writer->write(samples.data(), frames);
    return path;
}

// Returns total |sample| energy of the stereo EAR output, or -1 on failure.
double render_ear_diffuse_energy(float diffuse_value,
                                 const std::filesystem::path& out_path,
                                 mradm::RenderService& service,
                                 mradm::NullProgressSink& progress,
                                 mradm::NullLogSink& logs) {
    const auto in_path = write_diffuse_fixture(diffuse_value);
    FileGuard in_g{in_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+2+0";
    req.options.internal_allow_speaker_stereo = true;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: EAR diffuse render (d=" << diffuse_value << ") failed: " << res.error.message << "\n";
        return -1.0;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        return -1.0;
    }
    auto& reader = *reader_res;
    const auto n = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> buf(n * 2U);
    reader.read(buf.data(), reader.frame_count());

    return std::transform_reduce(
        buf.begin(),
        buf.end(),
        0.0,
        [](double a, double b) { return a + b; },
        [](float s) { return std::fabs(static_cast<double>(s)); });
}

bool verify_diffuse_bus(mradm::RenderService& service, mradm::NullProgressSink& progress, mradm::NullLogSink& logs) {
    bool ok = true;

    {
        const auto out = std::filesystem::temp_directory_path() / "mr_ear_diff0_out.wav";
        FileGuard g{out};
        const double e = render_ear_diffuse_energy(0.0F, out, service, progress, logs);
        ok &= check(e > 0.0, "diffuse=0.0: EAR output is not silent");
    }
    {
        // Before M4 fix: diffuse=1 → direct_gains *= √0 = 0 → silence.
        // After fix: diffuse bus carries the signal through the decorrelator.
        const auto out = std::filesystem::temp_directory_path() / "mr_ear_diff1_out.wav";
        FileGuard g{out};
        const double e = render_ear_diffuse_energy(1.0F, out, service, progress, logs);
        ok &= check(e > 0.0, "diffuse=1.0: EAR output is not silent (diffuse bus active)");
    }
    {
        const auto out = std::filesystem::temp_directory_path() / "mr_ear_diff05_out.wav";
        FileGuard g{out};
        const double e = render_ear_diffuse_energy(0.5F, out, service, progress, logs);
        ok &= check(e > 0.0, "diffuse=0.5: EAR output is not silent");
    }

    // Verify the direct bus delay: with diffuse=0, the first comp_delay (255)
    // output frames are silent because the delay buffer is initialised to zero.
    {
        const auto in_path = write_diffuse_fixture(0.0F);
        FileGuard in_g{in_path};
        const auto out = std::filesystem::temp_directory_path() / "mr_ear_delay_out.wav";
        FileGuard out_g{out};

        mradm::RenderRequest req;
        req.input_path = in_path;
        req.output_path = out;
        req.options.output_layout = "0+2+0";
        req.options.internal_allow_speaker_stereo = true;
        req.options.renderer = mradm::RendererSelection::ear;
        req.options.peak_limit = false;

        const auto res = service.render(req, progress, logs);
        if (!res.success()) {
            std::cerr << "FAIL: EAR delay test render failed: " << res.error.message << "\n";
            return false;
        }

        auto reader_res = mradm::audio::FloatWavReader::open(out.string());
        if (!reader_res) {
            return false;
        }
        auto& reader = *reader_res;
        const auto n_frames = static_cast<std::size_t>(reader.frame_count());
        std::vector<float> buf(n_frames * 2U);
        reader.read(buf.data(), reader.frame_count());

        constexpr std::size_t k_delay = 255U;
        double pre_energy = 0.0;
        for (std::size_t f = 0; f < std::min(k_delay, n_frames); ++f) {
            pre_energy += std::fabs(static_cast<double>(buf[f * 2U]));
            pre_energy += std::fabs(static_cast<double>(buf[(f * 2U) + 1U]));
        }
        double post_energy = 0.0;
        for (std::size_t f = k_delay; f < n_frames; ++f) {
            post_energy += std::fabs(static_cast<double>(buf[f * 2U]));
            post_energy += std::fabs(static_cast<double>(buf[(f * 2U) + 1U]));
        }
        ok &= check(pre_energy == 0.0, "direct delay: first 255 frames are silent");
        ok &= check(post_energy > 0.0, "direct delay: frames after 255 carry audio");
    }

    return ok;
}

// Verifies that the EAR renderer handles a block shorter than the FIR history
// (511 samples) and the direct delay (255 samples) without out-of-bounds access.
bool verify_ear_short_block(const mradm::RenderService& service,
                            mradm::NullProgressSink& progress,
                            mradm::NullLogSink& logs) {
    auto [doc, uid_str] = make_objects_doc();

    const auto in_path = std::filesystem::temp_directory_path() / "mr_ear_short_block_in.wav";
    {
        std::ostringstream xml_buf;
        adm::writeXml(xml_buf, doc);
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
        // 200 frames < min(255=comp_delay, 511=FIR history): exercises both short-block paths.
        constexpr uint32_t k_short = 200U;
        auto writer = bw64::writeFile(in_path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> samples(k_short, 0.5F);
        writer->write(samples.data(), k_short);
    }
    FileGuard in_g{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_short_block_out.wav";
    FileGuard out_g{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+2+0";
    req.options.internal_allow_speaker_stereo = true;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: short-block EAR render failed: " << res.error.message << "\n";
        return false;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        std::cerr << "FAIL: cannot open short-block output\n";
        return false;
    }
    return check(reader_res->frame_count() == 200U, "short-block: output has 200 frames");
}

// Creates an Objects document with channelLock/objectDivergence/screenRef set.
// EAR now preprocesses channelLock and objectDivergence before calling libear;
// screenRef still warns because referenceScreen geometry is not available.
bool verify_p2_degrade_gracefully(const mradm::RenderService& service, mradm::NullProgressSink& progress) {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"P2CF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        adm::ChannelLock cl;
        cl.set(adm::ChannelLockFlag{true});
        block.set(cl);
        adm::ObjectDivergence od;
        od.set(adm::Divergence{0.5F});
        block.set(od);
        block.set(adm::ScreenRef{true});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"P2PF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"P2SF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"P2TF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"P2Obj"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"P2Content"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"P2Programme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);

    const auto in_path = std::filesystem::temp_directory_path() / "mr_ear_p2_in.wav";
    {
        std::ostringstream xml_buf;
        adm::writeXml(xml_buf, doc);
        const std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
        auto writer = bw64::writeFile(in_path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> samples(1000U, 0.5F);
        writer->write(samples.data(), 1000U);
    }
    FileGuard in_g{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_p2_out.wav";
    FileGuard out_g{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+2+0";
    req.options.internal_allow_speaker_stereo = true;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;

    CapturingLogSink capturing_logs;
    const auto res = service.render(req, progress, capturing_logs);
    if (!res.success()) {
        std::cerr << "FAIL: P2 degrade test render failed (expected warn+degrade, got error): " << res.error.message
                  << "\n";
        return false;
    }

    bool ok = true;
    ok &= check(!capturing_logs.has_warning_containing("channelLock"),
                "P2 support: no unsupported warning emitted for channelLock");
    ok &= check(!capturing_logs.has_warning_containing("objectDivergence"),
                "P2 support: no unsupported warning emitted for objectDivergence");
    ok &= check(capturing_logs.has_warning_containing("screenRef"), "P2 support: warning emitted for screenRef");

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        return false;
    }
    auto& reader = *reader_res;
    const auto n = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> buf(n * 2U);
    reader.read(buf.data(), reader.frame_count());
    const double energy = std::transform_reduce(
        buf.begin(),
        buf.end(),
        0.0,
        [](double a, double b) { return a + b; },
        [](float s) { return std::fabs(static_cast<double>(s)); });
    ok &= check(energy > 0.0, "P2 support: output is not silent");
    return ok;
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_ear_object_semantics_doc(
    float azimuth, bool channel_lock, std::optional<float> max_distance, float divergence, float azimuth_range) {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"EarSemCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{azimuth}, adm::Elevation{0.0F}}};
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        if (channel_lock) {
            adm::ChannelLock lock;
            lock.set(adm::ChannelLockFlag{true});
            if (max_distance.has_value()) {
                lock.set(adm::MaxDistance{*max_distance});
            }
            block.set(lock);
        }
        if (divergence > 0.0F) {
            adm::ObjectDivergence od;
            od.set(adm::Divergence{divergence});
            od.set(adm::AzimuthRange{azimuth_range});
            block.set(od);
        }
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"EarSemPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"EarSemSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"EarSemTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"EarSemObj"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"EarSemContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"EarSemProgramme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::vector<double> render_ear_semantics_sums(const mradm::RenderService& service,
                                              mradm::ProgressSink& progress,
                                              mradm::LogSink& logs,
                                              const std::shared_ptr<adm::Document>& doc,
                                              const std::string& uid_str,
                                              std::string_view stem) {
    const auto in_path = write_input_fixture(uid_str, doc, stem, 48000U, 1000U);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / (std::string{stem} + "_out.wav");
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+5+0";
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: EAR semantics render failed: " << res.error.message << "\n";
        return {};
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res || reader_res->channels() != 6U) {
        std::cerr << "FAIL: EAR semantics output is not 5.1\n";
        return {};
    }
    auto& reader = *reader_res;
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(n_frames * 6U);
    reader.read(samples.data(), reader.frame_count());

    std::vector<double> sums(6U, 0.0);
    for (std::size_t frame = 0; frame < n_frames; ++frame) {
        for (std::size_t ch = 0; ch < 6U; ++ch) {
            sums[ch] += std::fabs(static_cast<double>(samples[(frame * 6U) + ch]));
        }
    }
    return sums;
}

bool verify_ear_channel_lock(const mradm::RenderService& service,
                             mradm::ProgressSink& progress,
                             mradm::NullLogSink& logs) {
    auto [doc, uid_str] = make_ear_object_semantics_doc(20.0F, true, std::nullopt, 0.0F, 45.0F);
    const auto sums = render_ear_semantics_sums(service, progress, logs, doc, uid_str, "mr_ear_channel_lock_in.wav");

    bool ok = true;
    ok &= check(sums.size() == 6U, "EAR channelLock: output is 5.1");
    if (sums.size() == 6U) {
        ok &= check(sums[0] > 100.0, "EAR channelLock: nearest left speaker carries energy");
        ok &= check(sums[0] > sums[2] * 10.0, "EAR channelLock: left speaker dominates center");
        ok &= check(sums[3] < 1.0e-9, "EAR channelLock: LFE is ignored");
    }
    return ok;
}

bool verify_ear_object_divergence(const mradm::RenderService& service,
                                  mradm::ProgressSink& progress,
                                  mradm::NullLogSink& logs) {
    auto [doc, uid_str] = make_ear_object_semantics_doc(0.0F, false, std::nullopt, 1.0F, 30.0F);
    const auto sums = render_ear_semantics_sums(service, progress, logs, doc, uid_str, "mr_ear_divergence_in.wav");

    bool ok = true;
    ok &= check(sums.size() == 6U, "EAR objectDivergence: output is 5.1");
    if (sums.size() == 6U) {
        ok &= check(sums[0] > 1.0, "EAR objectDivergence: left virtual source contributes");
        ok &= check(sums[1] > 1.0, "EAR objectDivergence: right virtual source contributes");
        ok &= check(sums[0] > sums[2], "EAR objectDivergence: left energy exceeds center");
        ok &= check(sums[1] > sums[2], "EAR objectDivergence: right energy exceeds center");
    }
    return ok;
}

// DS CartesianSpeakerPosition → M8b converts to polar at import time, no libear throw.
// CartesianSpeakerPosition{X=-0.5, Y=0.866, Z=0} converts via az=atan2(-X,Y)≈+30°.
// No SpeakerLabel → libear routes by position; source at az≈+30° must land left-biased
// in a 0+2+0 layout (M+030=ch0 at +30°, M-030=ch1 at -30°).
bool verify_ds_cartesian_speaker_position(const mradm::RenderService& service,
                                          mradm::ProgressSink& progress,
                                          mradm::NullLogSink& logs) {
    auto doc = adm::Document::create();
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"DsCartesianObj"});
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"DsCartesianCF"},
                                              adm::TypeDefinition::DIRECT_SPEAKERS);
    {
        // az=+30° → Cartesian: X=-sin(30°)=-0.5, Y=cos(30°)≈0.866
        // No SpeakerLabel: importer produces polar az≈+30°; libear routes by position only.
        adm::AudioBlockFormatDirectSpeakers block{
            adm::CartesianSpeakerPosition{adm::X{-0.5F}, adm::Y{0.866F}, adm::Z{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);
    auto pf =
        adm::AudioPackFormat::create(adm::AudioPackFormatName{"DsCartesianPF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"DsCartesianSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"DsCartesianTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"DsCartesianContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"DsCartesianProg"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    const auto uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());

    const auto in_path = write_input_fixture(uid_str, doc);
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_ds_cartesian_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+2+0";
    req.options.internal_allow_speaker_stereo = true;
    req.options.renderer = mradm::RendererSelection::ear;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: DS CartesianSpeakerPosition render failed: " << res.error.message << "\n";
        return false;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        std::cerr << "FAIL: cannot open DS cartesian output\n";
        return false;
    }
    auto& reader = *reader_res;
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(n_frames * 2U);
    reader.read(samples.data(), reader.frame_count());

    // Verify that the converted az≈+30° position drives the LEFT channel (ch0=M+030)
    // harder than the right channel (ch1=M-030). This confirms the Cartesian→polar
    // formula is correct: wrong sign on X would give az≈-30° and flip the result.
    double ch0_sq = 0.0;
    double ch1_sq = 0.0;
    for (std::size_t f = 0; f < n_frames; ++f) {
        const auto s0 = static_cast<double>(samples[(f * 2U) + 0U]);
        const auto s1 = static_cast<double>(samples[(f * 2U) + 1U]);
        ch0_sq += s0 * s0;
        ch1_sq += s1 * s1;
    }
    bool ok = true;
    ok &= check(ch0_sq > 100.0, "DS Cartesian: left channel (M+030) has energy");
    ok &= check(ch0_sq > ch1_sq * 5.0, "DS Cartesian: left channel louder than right (az≈+30° → left-biased)");
    return ok;
}

bool verify_ear_multiblock_inside_render_window(const mradm::RenderService& service,
                                                mradm::ProgressSink& progress,
                                                mradm::NullLogSink& logs) {
    auto [doc, uid_str] = make_two_block_jump_objects_doc();
    const auto in_path = write_input_fixture(uid_str, doc, "mr_ear_multiblock_window_in.wav", 1000U, 1000U);
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_multiblock_window_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+2+0";
    req.options.internal_allow_speaker_stereo = true;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.peak_limit = false;
    req.options.object_smoothing_frames = 0;

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: EAR multiblock window render failed: " << res.error.message << "\n";
        return false;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        std::cerr << "FAIL: cannot open EAR multiblock output\n";
        return false;
    }
    auto& reader = *reader_res;
    bool ok = check(reader.frame_count() == 1000U, "EAR multiblock: output has 1000 frames");
    if (!ok) {
        return false;
    }

    std::vector<float> samples(static_cast<std::size_t>(reader.frame_count()) * 2U);
    reader.read(samples.data(), reader.frame_count());

    double early_l = 0.0;
    double early_r = 0.0;
    for (std::size_t f = 320U; f < 470U; ++f) {
        early_l += std::fabs(static_cast<double>(samples[(f * 2U) + 0U]));
        early_r += std::fabs(static_cast<double>(samples[(f * 2U) + 1U]));
    }

    double late_l = 0.0;
    double late_r = 0.0;
    for (std::size_t f = 820U; f < 970U; ++f) {
        late_l += std::fabs(static_cast<double>(samples[(f * 2U) + 0U]));
        late_r += std::fabs(static_cast<double>(samples[(f * 2U) + 1U]));
    }

    ok &= check(early_r > 1.0, "EAR multiblock: first block contributes before boundary");
    ok &= check(early_r > early_l * 5.0, "EAR multiblock: first block routes right");
    ok &= check(late_l > 1.0, "EAR multiblock: second block contributes inside same render window");
    ok &= check(late_l > late_r * 5.0, "EAR multiblock: second block routes left");
    return ok;
}

// EarStream (realtime) reproduces the offline render_window render and is independent of
// the caller's pull chunk size (canonical block + FIFO). Uses a diffuse object > k_block_size
// so the FIR decorrelator overlap-add + the compensation delay are carried across multiple
// internal blocks. Drives the renderer directly (no RenderService post-processing).
bool verify_ear_stream_matches_window() {
    const auto in_path = write_diffuse_fixture(1.0F, 4096U);
    FileGuard in_guard{in_path};

    auto scene = mradm::io::import_scene(in_path.string());
    if (!check(scene.has_value(), "ear stream: import scene")) {
        return false;
    }
    mradm::RenderPlan plan;
    plan.input_path = in_path.string();
    plan.output_layout = "0+5+0";
    plan.scene = *scene;

    auto renderer = mradm::create_ear_renderer();
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "ear stream: prepare")) {
        return false;
    }

    const auto ref_path = std::filesystem::temp_directory_path() / "mr_ear_stream_ref.wav";
    FileGuard ref_guard{ref_path};
    mradm::RenderPlan window_plan = plan;
    window_plan.output_path = ref_path.string();
    if (!check(renderer->render_window(**prepared, window_plan, progress, logs).has_value(),
               "ear stream: reference render_window")) {
        return false;
    }
    auto reader = mradm::audio::FloatWavReader::open(ref_path.string());
    if (!check(reader.has_value(), "ear stream: reference WAV opens")) {
        return false;
    }
    const auto ch = static_cast<std::size_t>(reader->channels());
    std::vector<float> ref(ch * reader->frame_count());
    reader->read(ref.data(), reader->frame_count());

    auto pull_stream = [&](const std::vector<std::size_t>& chunk_pattern) -> std::vector<float> {
        auto stream = renderer->open_stream(**prepared, plan, logs);
        std::vector<float> out;
        if (!stream.has_value()) {
            check(false, "ear stream: open_stream");
            return out;
        }
        const uint32_t oc = (*stream)->out_channels();
        std::vector<float> buf;
        std::size_t pi = 0;
        while (true) {
            const std::size_t frames = chunk_pattern[pi % chunk_pattern.size()];
            ++pi;
            buf.assign(frames * oc, 0.0F);
            auto produced = (*stream)->process(std::span<float>(buf), frames);
            if (!produced || *produced == 0) {
                break;
            }
            out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*produced * oc));
        }
        return out;
    };

    const auto uniform = pull_stream({1024});
    const auto varied = pull_stream({333, 1000, 512, 7});

    bool ok = check(uniform.size() == ref.size(), "ear stream output frame count matches render_window");
    ok &= check(uniform == varied, "ear stream output is identical regardless of pull chunk size");
    double max_diff = 0.0;
    const std::size_t n = std::min(uniform.size(), ref.size());
    for (std::size_t i = 0; i < n; ++i) {
        max_diff = std::max(max_diff, std::fabs(static_cast<double>(uniform[i]) - static_cast<double>(ref[i])));
    }
    ok &= check(max_diff < 1.0e-4, "ear stream output matches the offline render_window render");
    return ok;
}

} // namespace

int main() {
    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;

    bool ok = true;
    ok &= verify_objects_render_fixture(service, progress, logs);
    ok &= verify_direct_speakers_render_fixture(service, progress, logs);
    ok &= verify_ear_custom_916_objects(service, progress, logs);
    ok &= verify_ear_custom_916_direct_speakers(service, progress, logs);
    ok &= verify_ear_direct_speakers_lfe_alias(service, progress, logs);
    ok &= verify_mixed_render_fixture(service, progress, logs);
    ok &= verify_ear_cartesian_objects(service, progress, logs);
    ok &= verify_position_offset(service, progress, logs);
    ok &= verify_diffuse_bus(service, progress, logs);
    ok &= verify_ear_short_block(service, progress, logs);
    ok &= verify_p2_degrade_gracefully(service, progress);
    ok &= verify_ear_channel_lock(service, progress, logs);
    ok &= verify_ear_object_divergence(service, progress, logs);
    ok &= verify_ds_cartesian_speaker_position(service, progress, logs);
    ok &= verify_ear_multiblock_inside_render_window(service, progress, logs);
    ok &= verify_ear_stream_matches_window();

    if (ok) {
        std::cout << "ear_render fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
