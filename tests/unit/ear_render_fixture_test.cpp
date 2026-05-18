#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
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

struct DirectSpeakersDoc {
    std::shared_ptr<adm::Document> doc;
    std::array<std::string, 2> uids;
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

} // namespace

int main() {
    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;

    bool ok = true;
    ok &= verify_objects_render_fixture(service, progress, logs);
    ok &= verify_direct_speakers_render_fixture(service, progress, logs);
    ok &= verify_mixed_render_fixture(service, progress, logs);

    if (ok) {
        std::cout << "ear_render fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
