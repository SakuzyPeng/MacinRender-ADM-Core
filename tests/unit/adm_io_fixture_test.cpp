#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

// Our library (mradm namespace)
#include "adm/io.h"

// libadm – used here only for constructing the fixture document
#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>

// libbw64 – used here only for writing the fixture BW64 file
#include <bw64/bw64.hpp>

namespace {

// RAII guard that removes a file on scope exit.
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

// Build a full Objects-chain document: uid → packformat → channelformat → block.
// az=30 el=10 gain=0.8 — non-default values that survive the round-trip through AdmScene.
std::pair<std::shared_ptr<adm::Document>, std::string> make_objects_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"TestCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{10.0F}}};
        block.set(adm::Gain{0.8F});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"TestPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    // AudioStreamFormat + AudioTrackFormat are required for a valid UID reference chain.
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"TestSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TestTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"TestObject"});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"TestContent"});
    content->addReference(object);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"TestProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// Build a DirectSpeakers-chain document: uid → packformat → channelformat → block.
// label=M+030 az=30 el=0 gain=0.7 — enough metadata to route without libadm in renderers.
std::pair<std::shared_ptr<adm::Document>, std::string> make_direct_speakers_doc() {
    auto doc = adm::Document::create();

    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"DsCF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    {
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}, adm::Distance{1.0F}}};
        block.add(adm::SpeakerLabel{"M+030"});
        block.set(adm::Gain{0.7F});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"DsPF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"DsSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"DsTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"DsObject"});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"DsContent"});
    content->addReference(object);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"DsProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// Build a minimal ADM document: 1 programme → 1 content → 1 object → 1 uid.
// Returns the document and the UID string (e.g. "ATU_00000001") for use in CHNA.
std::pair<std::shared_ptr<adm::Document>, std::string> make_minimal_doc() {
    auto doc = adm::Document::create();

    auto uid = adm::AudioTrackUid::create();
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"TestObject"});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"TestContent"});
    content->addReference(object);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"TestProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);

    std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());
    return {doc, uid_str};
}

// Write the ADM document to an XML string (for the AXML chunk).
std::string serialize_doc(const std::shared_ptr<adm::Document>& doc) {
    std::ostringstream buf;
    adm::writeXml(buf, doc);
    return buf.str();
}

std::string with_programme_reference_screen(std::string xml) {
    // libadm parses audioProgrammeReferenceScreen but does not write the empty
    // element back out, so inject it into fixture XML to exercise import_scene().
    constexpr std::string_view marker = "</audioProgramme>";
    const auto pos = xml.find(marker);
    if (pos != std::string::npos) {
        xml.insert(pos, "    <audioProgrammeReferenceScreen />\n");
    }
    return xml;
}

// Create a temp BW64 file with 1 channel and the given ADM metadata.
// Returns the file path; caller owns cleanup via FileGuard.
std::filesystem::path write_fixture(const std::string& uid_str, const std::string& xml_str) {
    auto path = std::filesystem::temp_directory_path() / "mr_adm_fixture_test.wav";

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_str);

    // Write an empty 1-channel 48 kHz file with CHNA and AXML chunks.
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    // No audio samples needed; writer flushes on destruction.
    (void) writer;

    return path;
}

bool check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return condition;
}

bool verify_minimal_fixture() {
    auto [doc, uid_str] = make_minimal_doc();
    std::string xml_str = serialize_doc(doc);
    auto path = write_fixture(uid_str, xml_str);
    FileGuard guard{path};

    auto result = mradm::io::import_scene(path.string());

    bool ok = true;

    if (!result.has_value()) {
        std::cerr << "FAIL: import_scene returned error: " << result.error().message << "\n";
        return false;
    }

    const auto& scene = result.value();

    ok &= check(scene.info.sample_rate == 48000, "sample_rate == 48000");
    ok &= check(scene.info.num_channels == 1, "num_channels == 1");
    ok &= check(scene.info.num_frames == 0, "num_frames == 0 (empty fixture)");

    ok &= check(scene.programmes.size() == 1, "exactly 1 programme");
    ok &= check(scene.contents.size() == 1, "exactly 1 content");
    ok &= check(scene.objects.size() == 1, "exactly 1 object");

    if (scene.programmes.size() == 1) {
        ok &= check(scene.programmes[0].name == "TestProgramme", "programme name");
        ok &= check(scene.programmes[0].content_ids.size() == 1, "programme has 1 content ref");
    }
    if (scene.contents.size() == 1) {
        ok &= check(scene.contents[0].name == "TestContent", "content name");
        ok &= check(scene.contents[0].object_ids.size() == 1, "content has 1 object ref");
    }
    if (scene.objects.size() == 1) {
        ok &= check(scene.objects[0].name == "TestObject", "object name");
        ok &= check(scene.objects[0].tracks.size() == 1, "object has 1 track ref");

        if (scene.objects[0].tracks.size() == 1) {
            const auto& ref = scene.objects[0].tracks[0];
            ok &= check(ref.track_uid == uid_str, "track_uid matches CHNA UID");
            ok &= check(ref.channel_index.has_value(), "channel_index mapped from CHNA");
            if (ref.channel_index.has_value()) {
                ok &= check(*ref.channel_index == 0, "channel_index == 0 (first track)");
            }
        }
    }

    return ok;
}

bool verify_objects_blocks_fixture() {
    bool ok = true;
    auto [doc2, uid2_str] = make_objects_doc();
    auto path2 = std::filesystem::temp_directory_path() / "mr_adm_io_blocks_fixture.wav";
    FileGuard guard2{path2};

    {
        auto chna2 = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid2_str, "", "")});
        auto axml2 = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc2));
        auto writer2 = bw64::writeFile(path2.string(), 1U, 48000U, 24U, chna2, axml2);
        (void) writer2;
    }

    auto result2 = mradm::io::import_scene(path2.string());
    if (!result2.has_value()) {
        std::cerr << "FAIL: objects import failed: " << result2.error().message << "\n";
        return false;
    }

    const auto& s2 = result2.value();
    ok &= check(s2.objects.size() == 1, "objects doc: 1 object");
    if (s2.objects.size() == 1 && s2.objects[0].tracks.size() == 1) {
        const auto& track = s2.objects[0].tracks[0];
        ok &= check(track.blocks.size() == 1, "objects track: 1 block");
        if (!track.blocks.empty()) {
            const auto& blk = track.blocks[0];
            ok &= check(!blk.position.cartesian, "position is polar");
            ok &= check(std::fabs(blk.position.azimuth - 30.0F) < 0.01F, "azimuth ≈ 30");
            ok &= check(std::fabs(blk.position.elevation - 10.0F) < 0.01F, "elevation ≈ 10");
            ok &= check(std::fabs(blk.position.distance - 1.0F) < 0.01F, "distance ≈ 1 (default)");
            ok &= check(std::fabs(blk.gain - 0.8F) < 0.01F, "gain ≈ 0.8");
        }
    }

    return ok;
}

bool verify_direct_speakers_blocks_fixture() {
    bool ok = true;
    auto [doc3, uid3_str] = make_direct_speakers_doc();
    auto path3 = std::filesystem::temp_directory_path() / "mr_adm_io_ds_blocks_fixture.wav";
    FileGuard guard3{path3};

    {
        auto chna3 = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid3_str, "", "")});
        auto axml3 = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc3));
        auto writer3 = bw64::writeFile(path3.string(), 1U, 48000U, 24U, chna3, axml3);
        (void) writer3;
    }

    auto result3 = mradm::io::import_scene(path3.string());
    if (!result3.has_value()) {
        std::cerr << "FAIL: DirectSpeakers import failed: " << result3.error().message << "\n";
        return false;
    }

    const auto& s3 = result3.value();
    ok &= check(s3.objects.size() == 1, "direct speakers doc: 1 object");
    if (s3.objects.size() == 1 && s3.objects[0].tracks.size() == 1) {
        const auto& track = s3.objects[0].tracks[0];
        ok &= check(track.blocks.empty(), "direct speakers track: no Objects blocks");
        ok &= check(track.ds_blocks.size() == 1, "direct speakers track: 1 DS block");
        if (!track.ds_blocks.empty()) {
            const auto& blk = track.ds_blocks[0];
            const auto has_label = std::ranges::find(blk.speaker_labels, "M+030") != blk.speaker_labels.end();
            ok &= check(has_label, "DS speaker label M+030");
            ok &= check(!blk.pack_format_id.empty(), "DS pack format id captured");
            ok &= check(blk.has_position, "DS position captured");
            ok &= check(std::fabs(blk.azimuth - 30.0F) < 0.01F, "DS azimuth ≈ 30");
            ok &= check(std::fabs(blk.elevation) < 0.01F, "DS elevation ≈ 0");
            ok &= check(std::fabs(blk.distance - 1.0F) < 0.01F, "DS distance ≈ 1");
            ok &= check(std::fabs(blk.gain - 0.7F) < 0.01F, "DS gain ≈ 0.7");
        }
    }

    return ok;
}

// Build a document with one Objects AudioObject and one DirectSpeakers AudioObject
// sharing a single AudioContent and AudioProgramme.
// Returns {doc, objects_uid_str, ds_uid_str}.
std::tuple<std::shared_ptr<adm::Document>, std::string, std::string> make_mixed_doc() {
    auto doc = adm::Document::create();

    // Objects chain
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

    // DirectSpeakers chain
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

    // Shared content and programme
    auto content = adm::AudioContent::create(adm::AudioContentName{"MixedContent"});
    content->addReference(obj_audio_object);
    content->addReference(ds_audio_object);
    doc->add(content);
    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"MixedProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);
    return {
        doc, adm::formatId(obj_uid->get<adm::AudioTrackUidId>()), adm::formatId(ds_uid->get<adm::AudioTrackUidId>())};
}

bool verify_mixed_blocks_fixture() {
    bool ok = true;
    auto [doc, obj_uid_str, ds_uid_str] = make_mixed_doc();
    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_mixed_fixture.wav";
    FileGuard guard{path};

    {
        auto chna = std::make_shared<bw64::ChnaChunk>(
            std::vector<bw64::AudioId>{bw64::AudioId(1, obj_uid_str, "", ""), bw64::AudioId(2, ds_uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
        auto writer = bw64::writeFile(path.string(), 2U, 48000U, 24U, chna, axml);
        (void) writer;
    }

    auto result = mradm::io::import_scene(path.string());
    if (!result.has_value()) {
        std::cerr << "FAIL: mixed import failed: " << result.error().message << "\n";
        return false;
    }

    const auto& scene = result.value();
    ok &= check(scene.objects.size() == 2, "mixed doc: 2 objects");

    const mradm::SceneObject* obj_scene_obj = nullptr;
    const mradm::SceneObject* ds_scene_obj = nullptr;
    for (const auto& o : scene.objects) {
        if (!o.tracks.empty()) {
            if (!o.tracks[0].blocks.empty()) {
                obj_scene_obj = &o;
            }
            if (!o.tracks[0].ds_blocks.empty()) {
                ds_scene_obj = &o;
            }
        }
    }

    ok &= check(obj_scene_obj != nullptr, "mixed: Objects object found");
    if (obj_scene_obj != nullptr) {
        ok &= check(obj_scene_obj->tracks[0].blocks.size() == 1, "mixed Objects track: 1 block");
        ok &= check(obj_scene_obj->tracks[0].ds_blocks.empty(), "mixed Objects track: no DS blocks");
    }

    ok &= check(ds_scene_obj != nullptr, "mixed: DirectSpeakers object found");
    if (ds_scene_obj != nullptr) {
        ok &= check(ds_scene_obj->tracks[0].ds_blocks.size() == 1, "mixed DS track: 1 DS block");
        ok &= check(ds_scene_obj->tracks[0].blocks.empty(), "mixed DS track: no Objects blocks");
        if (!ds_scene_obj->tracks[0].ds_blocks.empty()) {
            const auto& blk = ds_scene_obj->tracks[0].ds_blocks[0];
            const auto has_label = std::ranges::find(blk.speaker_labels, "M+030") != blk.speaker_labels.end();
            ok &= check(has_label, "mixed DS block: label M+030");
        }
    }

    return ok;
}

// Build a one-channel Binaural ADM document so we can verify the import warning.
std::pair<std::shared_ptr<adm::Document>, std::string> make_binaural_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"BinauralCF"}, adm::TypeDefinition::BINAURAL);
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"BinauralPF"}, adm::TypeDefinition::BINAURAL);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"BinauralSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"BinauralTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"BinauralObject"});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"BinauralContent"});
    content->addReference(object);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"BinauralProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// Build a one-channel DirectSpeakers ADM document with channelFrequency.lowPass
// set on the CF to identify it as an LFE channel.
std::pair<std::shared_ptr<adm::Document>, std::string> make_ds_lfe_doc() {
    auto doc = adm::Document::create();

    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"LfeCF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    cf->set(adm::Frequency{adm::LowPass{120.0F}});
    {
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{45.0F}, adm::Elevation{-30.0F}, adm::Distance{1.0F}}};
        block.add(adm::SpeakerLabel{"LFE1"});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"LfePF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"LfeSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"LfeTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"LfeObject"});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"LfeContent"});
    content->addReference(object);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"LfeProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// Build a minimal Objects ADM document with LoudnessMetadata on its programme.
std::pair<std::shared_ptr<adm::Document>, std::string> make_loudness_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"LoudnessCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"LoudnessPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"LoudnessSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"LoudnessTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"LoudnessObject"});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"LoudnessContent"});
    content->addReference(object);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"LoudnessProgramme"});
    programme->addReference(content);
    {
        adm::LoudnessMetadata lm;
        lm.set(adm::IntegratedLoudness{-23.0F});
        lm.set(adm::MaxTruePeak{-1.0F});
        lm.set(adm::LoudnessRange{6.5F});
        lm.set(adm::MaxMomentary{-18.5F});
        lm.set(adm::MaxShortTerm{-20.0F});
        lm.set(adm::DialogueLoudness{-24.0F});
        lm.set(adm::LoudnessMethod{"ITU-R BS.1770-4"});
        programme->set(adm::LoudnessMetadatas{lm});
    }
    doc->add(programme);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

bool verify_programme_loudness_metadata_imported() {
    bool ok = true;
    auto [doc, uid_str] = make_loudness_doc();
    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_loudness.wav";
    FileGuard guard{path};

    {
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
        auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> silence(1, 0.0F);
        writer->write(silence.data(), 1U);
    }

    auto result = mradm::io::import_scene(path.string());
    if (!result) {
        std::cerr << "FAIL: import_scene returned error: " << result.error().message << "\n";
        return false;
    }
    const auto& scene = result.value();

    if (scene.programmes.empty()) {
        std::cerr << "FAIL: no programmes imported\n";
        return false;
    }
    const auto& prog = scene.programmes[0];

    if (!prog.loudness) {
        std::cerr << "FAIL: programme has no loudness metadata\n";
        return false;
    }
    const auto& lm = *prog.loudness;

    auto check_float = [&](const std::optional<float>& val, const char* name, float expected) {
        if (!val) {
            std::cerr << "FAIL: " << name << " not imported\n";
            ok = false;
            return;
        }
        if (std::fabs(*val - expected) > 0.01F) {
            std::cerr << "FAIL: " << name << " = " << *val << ", expected " << expected << "\n";
            ok = false;
        }
    };

    check_float(lm.integrated_loudness, "integrated_loudness", -23.0F);
    check_float(lm.max_true_peak, "max_true_peak", -1.0F);
    check_float(lm.loudness_range, "loudness_range", 6.5F);
    check_float(lm.max_momentary, "max_momentary", -18.5F);
    check_float(lm.max_short_term, "max_short_term", -20.0F);
    check_float(lm.dialogue_loudness, "dialogue_loudness", -24.0F);

    if (!lm.loudness_method || *lm.loudness_method != "ITU-R BS.1770-4") {
        std::cerr << "FAIL: loudness_method = " << (lm.loudness_method ? *lm.loudness_method : "<absent>") << "\n";
        ok = false;
    }

    if (ok) {
        std::cout << "PASS: verify_programme_loudness_metadata_imported\n";
    }
    return ok;
}

bool verify_binaural_skipped_produces_import_warning() {
    bool ok = true;
    auto [doc, uid_str] = make_binaural_doc();
    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_binaural_warn.wav";
    FileGuard guard{path};

    {
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
        auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
        (void) writer;
    }

    auto result = mradm::io::import_scene(path.string());
    if (!result.has_value()) {
        std::cerr << "FAIL: Binaural import failed: " << result.error().message << "\n";
        return false;
    }

    const auto& scene = result.value();
    ok &= check(!scene.import_warnings.empty(), "Binaural typeDefinition: import_warnings non-empty");
    const bool has_binaural_warn = std::ranges::any_of(
        scene.import_warnings, [](const auto& w) { return w.find("Binaural") != std::string::npos; });
    ok &= check(has_binaural_warn, "Binaural typeDefinition: warning mentions 'Binaural'");

    return ok;
}

bool verify_ds_lfe_channel_frequency_imported() {
    bool ok = true;
    auto [doc, uid_str] = make_ds_lfe_doc();
    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_ds_lfe_freq.wav";
    FileGuard guard{path};

    {
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
        auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
        (void) writer;
    }

    auto result = mradm::io::import_scene(path.string());
    if (!result.has_value()) {
        std::cerr << "FAIL: LFE DS import failed: " << result.error().message << "\n";
        return false;
    }

    const auto& scene = result.value();
    ok &= check(scene.import_warnings.empty(), "LFE DS: no import warnings (known typeDefinition)");
    ok &= check(scene.objects.size() == 1, "LFE DS: 1 object");
    if (!scene.objects.empty() && !scene.objects[0].tracks.empty() && !scene.objects[0].tracks[0].ds_blocks.empty()) {
        const auto& blk = scene.objects[0].tracks[0].ds_blocks[0];
        ok &= check(blk.low_pass_hz.has_value(), "LFE DS: low_pass_hz populated");
        if (blk.low_pass_hz) {
            ok &= check(std::fabs(*blk.low_pass_hz - 120.0F) < 0.5F, "LFE DS: low_pass_hz ≈ 120 Hz");
        }
    } else {
        std::cerr << "FAIL: LFE DS: expected 1 DS block\n";
        ok = false;
    }

    return ok;
}

// Build a one-channel Objects ADM document with AudioContent metadata:
// language, label, loudness, and dialogue=dialogue / voiceover kind.
std::pair<std::shared_ptr<adm::Document>, std::string> make_content_metadata_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"CMdCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"CMdPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"CMdSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"CMdTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"CMdObject"});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"CMdContent"});
    content->addReference(object);
    content->set(adm::AudioContentLanguage{"en"});
    {
        adm::Label label;
        label.set(adm::LabelValue{"English Commentary"});
        label.set(adm::LabelLanguage{"en"});
        content->add(label);
    }
    {
        adm::LoudnessMetadata lm;
        lm.set(adm::IntegratedLoudness{-24.0F});
        lm.set(adm::MaxTruePeak{-2.0F});
        content->set(adm::LoudnessMetadatas{lm});
    }
    content->set(adm::DialogueContent::VOICEOVER);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"CMdProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

bool verify_content_metadata_imported() {
    bool ok = true;
    auto [doc, uid_str] = make_content_metadata_doc();
    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_content_metadata.wav";
    FileGuard guard{path};

    {
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
        auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> silence(1, 0.0F);
        writer->write(silence.data(), 1U);
    }

    auto result = mradm::io::import_scene(path.string());
    if (!result) {
        std::cerr << "FAIL: import_scene returned error: " << result.error().message << "\n";
        return false;
    }
    const auto& scene = result.value();

    ok &= check(scene.contents.size() == 1, "content metadata: 1 content");
    if (scene.contents.empty()) {
        return false;
    }
    const auto& ct = scene.contents[0];

    ok &= check(ct.language.has_value() && *ct.language == "en", "content: language == 'en'");
    ok &= check(!ct.labels.empty() && ct.labels[0] == "English Commentary", "content: label value");
    ok &=
        check(ct.dialogue_kind.has_value() && *ct.dialogue_kind == "dialogue", "content: dialogue_kind == 'dialogue'");
    ok &= check(ct.content_kind.has_value() && *ct.content_kind == "voiceover", "content: content_kind == 'voiceover'");

    if (!ct.loudness) {
        std::cerr << "FAIL: content loudness absent\n";
        return false;
    }
    ok &= check(ct.loudness->integrated_loudness.has_value() &&
                    std::fabs(*ct.loudness->integrated_loudness - (-24.0F)) < 0.01F,
                "content: integrated_loudness == -24 LUFS");
    ok &= check(ct.loudness->max_true_peak.has_value() && std::fabs(*ct.loudness->max_true_peak - (-2.0F)) < 0.01F,
                "content: max_true_peak == -2 dBTP");

    if (ok) {
        std::cout << "PASS: verify_content_metadata_imported\n";
    }
    return ok;
}

// Reuse make_minimal_doc() — no reference screen.
// Build a second doc with audioProgrammeReferenceScreen set.
std::pair<std::shared_ptr<adm::Document>, std::string> make_reference_screen_doc() {
    auto doc = adm::Document::create();

    auto uid = adm::AudioTrackUid::create();
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"RSObject"});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"RSContent"});
    content->addReference(object);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"RSProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

bool verify_reference_screen_flag_imported() {
    bool ok = true;

    // Without reference screen: flag must be false.
    {
        auto [doc, uid_str] = make_minimal_doc();
        auto path = std::filesystem::temp_directory_path() / "mr_adm_io_no_refscreen.wav";
        FileGuard guard{path};
        {
            auto chna =
                std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
            auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
            auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
            (void) writer;
        }
        auto result = mradm::io::import_scene(path.string());
        if (!result) {
            std::cerr << "FAIL: no-refscreen import_scene error\n";
            return false;
        }
        ok &= check(!result->programmes.empty() && !result->programmes[0].has_reference_screen,
                    "no refscreen: has_reference_screen == false");
    }

    // With reference screen: flag must be true.
    {
        auto [doc, uid_str] = make_reference_screen_doc();
        auto path = std::filesystem::temp_directory_path() / "mr_adm_io_refscreen.wav";
        FileGuard guard{path};
        {
            auto chna =
                std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
            auto axml = std::make_shared<bw64::AxmlChunk>(with_programme_reference_screen(serialize_doc(doc)));
            auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
            (void) writer;
        }
        auto result = mradm::io::import_scene(path.string());
        if (!result) {
            std::cerr << "FAIL: refscreen import_scene error\n";
            return false;
        }
        ok &= check(!result->programmes.empty() && result->programmes[0].has_reference_screen,
                    "with refscreen: has_reference_screen == true");
    }

    if (ok) {
        std::cout << "PASS: verify_reference_screen_flag_imported\n";
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_minimal_fixture();
    ok &= verify_objects_blocks_fixture();
    ok &= verify_direct_speakers_blocks_fixture();
    ok &= verify_mixed_blocks_fixture();
    ok &= verify_binaural_skipped_produces_import_warning();
    ok &= verify_ds_lfe_channel_frequency_imported();
    ok &= verify_programme_loudness_metadata_imported();
    ok &= verify_content_metadata_imported();
    ok &= verify_reference_screen_flag_imported();

    if (ok) {
        std::cout << "adm_io fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
