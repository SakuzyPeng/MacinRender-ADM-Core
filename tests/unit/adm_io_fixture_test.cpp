#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "../support/adm_fixture.h"

// Our library (mradm namespace)
#include "adm/io.h"

// libadm – used here only for constructing the fixture document

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
std::pair<std::shared_ptr<fixture::Document>, std::string> make_objects_doc() {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"TestCF"},
                                                  fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{30.0F}, fixture::Elevation{10.0F}}};
        block.set(fixture::Gain{0.8F});
        cf->add(block);
    }
    doc->add(cf);

    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"TestPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);

    // AudioStreamFormat + AudioTrackFormat are required for a valid UID reference chain.
    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"TestSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"TestTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"TestObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"TestContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"TestProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<fixture::Document>, std::string> make_fractional_timing_objects_doc() {
    using namespace std::chrono_literals;

    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"FractionalCF"},
                                                  fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block0{
            fixture::SphericalPosition{fixture::Azimuth{-30.0F}, fixture::Elevation{0.0F}}};
        block0.set(fixture::Rtime{fixture::Time{0ns}});
        block0.set(fixture::Duration{fixture::Time{5'408'630'000ns}});
        block0.set(fixture::JumpPosition{fixture::JumpPositionFlag{true}});
        cf->add(block0);

        fixture::AudioBlockFormatObjects block1{
            fixture::SphericalPosition{fixture::Azimuth{30.0F}, fixture::Elevation{0.0F}}};
        block1.set(fixture::Rtime{fixture::Time{5'408'630'000ns}});
        block1.set(fixture::Duration{fixture::Time{1'040'000ns}});
        block1.set(fixture::JumpPosition{fixture::JumpPositionFlag{true}});
        cf->add(block1);

        fixture::AudioBlockFormatObjects block2{
            fixture::SphericalPosition{fixture::Azimuth{-30.0F}, fixture::Elevation{0.0F}}};
        block2.set(fixture::Rtime{fixture::Time{5'409'670'000ns}});
        block2.set(fixture::Duration{fixture::Time{1'040'000ns}});
        block2.set(fixture::JumpPosition{fixture::JumpPositionFlag{true}});
        cf->add(block2);
    }
    doc->add(cf);

    auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"FractionalPF"},
                                               fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf = fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"FractionalSF"},
                                                 fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf = fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"FractionalTF"},
                                                fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"FractionalObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"FractionalContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"FractionalProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<fixture::Document>, std::string>
make_object_lock_divergence_doc(bool explicit_ranges = true) {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"LockDivCF"},
                                                  fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{20.0F}, fixture::Elevation{0.0F}}};
        fixture::ChannelLock lock;
        lock.set(fixture::ChannelLockFlag{true});
        if (explicit_ranges) {
            lock.set(fixture::MaxDistance{1.0F});
        }
        block.set(lock);

        fixture::ObjectDivergence divergence;
        divergence.set(fixture::Divergence{0.5F});
        if (explicit_ranges) {
            divergence.set(fixture::AzimuthRange{60.0F});
            divergence.set(fixture::PositionRange{0.25F});
        }
        block.set(divergence);
        cf->add(block);
    }
    doc->add(cf);

    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"LockDivPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"LockDivSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"LockDivTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"LockDivObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"LockDivContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"LockDivProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

// Build a DirectSpeakers-chain document: uid → packformat → channelformat → block.
// label=M+030 az=30 el=0 gain=0.7 — enough metadata to route without libadm in renderers.
std::pair<std::shared_ptr<fixture::Document>, std::string> make_direct_speakers_doc() {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"DsCF"},
                                                  fixture::TypeDefinition::direct_speakers);
    {
        fixture::AudioBlockFormatDirectSpeakers block{fixture::SphericalSpeakerPosition{
            fixture::Azimuth{30.0F}, fixture::Elevation{0.0F}, fixture::Distance{1.0F}}};
        block.add(fixture::SpeakerLabel{"M+030"});
        block.set(fixture::Gain{0.7F});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"DsPF"},
                                               fixture::TypeDefinition::direct_speakers);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"DsSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf = fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"DsTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"DsObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"DsContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"DsProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

// Build one block of the requested type with independently optional object- and
// block-level headLocked values. This lets the import test distinguish omitted
// block metadata from an explicitly authored false value.
std::pair<std::shared_ptr<fixture::Document>, std::string> make_head_locked_doc(fixture::TypeDescriptor type,
                                                                                std::optional<bool> object_head_locked,
                                                                                std::optional<bool> block_head_locked) {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"HeadLockedCF"}, type);
    if (type == fixture::TypeDefinition::objects) {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
        if (block_head_locked.has_value()) {
            block.set(fixture::HeadLocked{*block_head_locked});
        }
        cf->add(block);
    } else if (type == fixture::TypeDefinition::direct_speakers) {
        fixture::AudioBlockFormatDirectSpeakers block{fixture::SphericalSpeakerPosition{
            fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}, fixture::Distance{1.0F}}};
        block.add(fixture::SpeakerLabel{"M+000"});
        if (block_head_locked.has_value()) {
            block.set(fixture::HeadLocked{*block_head_locked});
        }
        cf->add(block);
    } else {
        fixture::AudioBlockFormatHoa block{fixture::Order{0}, fixture::Degree{0}};
        if (block_head_locked.has_value()) {
            block.set(fixture::HeadLocked{*block_head_locked});
        }
        cf->add(block);
    }
    doc->add(cf);

    std::shared_ptr<fixture::AudioPackFormat> pf;
    if (type == fixture::TypeDefinition::hoa) {
        pf = fixture::AudioPackFormatHoa::create(fixture::AudioPackFormatName{"HeadLockedPF"});
    } else {
        pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"HeadLockedPF"}, type);
    }
    pf->add_reference(cf);
    doc->add(pf);

    auto sf = fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"HeadLockedSF"},
                                                 fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);
    auto tf = fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"HeadLockedTF"},
                                                fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);
    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"HeadLockedObject"});
    if (object_head_locked.has_value()) {
        object->set(fixture::HeadLocked{*object_head_locked});
    }
    object->add_reference(uid);
    doc->add(object);
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"HeadLockedContent"});
    content->add_reference(object);
    doc->add(content);
    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"HeadLockedProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

// Build a minimal ADM document: 1 programme → 1 content → 1 object → 1 uid.
// Returns the document and the UID string (e.g. "ATU_00000001") for use in CHNA.
std::pair<std::shared_ptr<fixture::Document>, std::string> make_minimal_doc() {
    auto doc = fixture::Document::create();

    auto uid = fixture::AudioTrackUid::create();
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"TestObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"TestContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"TestProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);

    std::string uid_str = fixture::format_id(uid->get<fixture::AudioTrackUidId>());
    return {doc, uid_str};
}

// Write the ADM document to an XML string (for the AXML chunk).
std::string serialize_doc(const std::shared_ptr<fixture::Document>& doc) {
    std::ostringstream buf;
    fixture::write_xml(buf, doc);
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

bool verify_hoa_pack_metadata() {
    auto [doc, uid] = make_head_locked_doc(fixture::TypeDefinition::hoa, std::nullopt, std::nullopt);
    for (const auto& pack : doc->get_elements<fixture::AudioPackFormatHoa>()) {
        pack->set(fixture::Normalization{"N3D"});
        pack->set(fixture::NfcRefDist{2.0F});
        pack->set(fixture::ScreenRef{true});
    }
    const auto path = write_fixture(uid, serialize_doc(doc));
    const FileGuard guard{path};
    const auto scene = mradm::io::import_scene(path.string());
    if (!scene) {
        std::cerr << "FAIL: HOA pack metadata import: " << scene.error().message << '\n';
        return false;
    }
    if (!check(scene->hoa_tracks.size() == 1U, "HOA pack metadata: one pack")) {
        return false;
    }
    const auto& pack = scene->hoa_tracks.front();
    return check(pack.normalization == "N3D", "HOA pack normalization attribute") &&
           check(pack.nfc_ref_dist == 2.0, "HOA pack nfcRefDist attribute") &&
           check(pack.screen_ref, "HOA pack screenRef attribute");
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

bool verify_object_semantic_provenance() {
    using namespace std::chrono_literals;
    auto [doc, uid] = make_objects_doc();
    const auto object = *doc->get_elements<fixture::AudioObject>().begin();
    object->set(fixture::Gain::from_db(-6.020599913279624));
    object->set(fixture::Mute{true});
    object->set(fixture::Start{fixture::Time{1s}});
    object->set(fixture::Duration{fixture::Time{2s}});
    const auto channel = *doc->get_elements<fixture::AudioChannelFormat>().begin();
    auto& block = *channel->get_elements<fixture::AudioBlockFormatObjects>().begin();
    block.set(fixture::Gain::from_db(-12.041199826559248));
    block.set(fixture::Rtime{fixture::Time{10ms}});
    block.set(fixture::Duration{fixture::Time{20ms}});
    const auto path = write_fixture(uid, serialize_doc(doc));
    FileGuard guard{path};
    const auto imported = mradm::io::import_scene(path.string());
    if (!check(imported.has_value(), "semantic provenance fixture imports")) {
        return false;
    }
    const auto& result = imported->objects.front();
    const auto& event = result.tracks.front().blocks.front();
    if (!result.adm_source || !event.adm_source) {
        return check(false, "source provenance is retained");
    }
    bool ok = true;
    ok &= check(result.adm_source->gain.present && result.adm_source->gain.decibels &&
                    std::fabs(result.gain - .5F) < 1e-6F,
                "object gain retains dB authorship and normalizes once");
    ok &= check(result.adm_source->mute_present && result.adm_source->mute &&
                    result.adm_source->start_samples == 48000 && result.end_sample == 144000,
                "object authored mute/start/duration remain available");
    ok &= check(event.start_sample == 48480 && event.end_sample == 49440 && event.adm_source->rtime_samples == 480 &&
                    event.adm_source->duration_samples == 960 && event.adm_source->gain.decibels &&
                    std::fabs(event.gain - .25F) < 1e-6F,
                "relative source time survives canonical absolute timing");
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
            ok &= check(!blk.channel_lock, "channelLock defaults to false");
            ok &= check(!blk.channel_lock_max_distance.has_value(), "channelLock maxDistance defaults to unset");
            ok &= check(std::fabs(blk.divergence) < 0.01F, "objectDivergence divergence defaults to 0");
            ok &= check(std::fabs(blk.divergence_azimuth_range - 45.0F) < 0.01F,
                        "objectDivergence azimuthRange defaults to 45");
            ok &=
                check(std::fabs(blk.divergence_position_range) < 0.01F, "objectDivergence positionRange defaults to 0");
        }
    }

    return ok;
}

bool verify_fractional_block_boundaries_are_contiguous() {
    bool ok = true;
    auto [doc, uid_str] = make_fractional_timing_objects_doc();
    auto path = write_fixture(uid_str, serialize_doc(doc));
    FileGuard guard{path};

    auto result = mradm::io::import_scene(path.string());
    if (!result.has_value()) {
        std::cerr << "FAIL: fractional timing import failed: " << result.error().message << "\n";
        return false;
    }

    const auto& scene = result.value();
    ok &= check(scene.objects.size() == 1, "fractional timing: 1 object");
    if (scene.objects.size() == 1 && scene.objects[0].tracks.size() == 1) {
        const auto& blocks = scene.objects[0].tracks[0].blocks;
        ok &= check(blocks.size() == 3, "fractional timing: 3 blocks");
        if (blocks.size() == 3) {
            ok &= check(blocks[0].end_sample == blocks[1].start_sample, "block0 end == block1 start");
            ok &= check(blocks[1].end_sample == blocks[2].start_sample, "block1 end == block2 start");
            ok &= check(blocks[1].start_sample == 259614, "block1 start rounded to nearest sample");
            ok &= check(blocks[1].end_sample == 259664, "block1 end rounded to nearest sample");
        }
    }

    return ok;
}

bool verify_object_lock_divergence_imported() {
    bool ok = true;
    auto [doc, uid_str] = make_object_lock_divergence_doc(true);
    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_lock_divergence.wav";
    FileGuard guard{path};

    {
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
        auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> silence(1U, 0.0F);
        writer->write(silence.data(), 1U);
    }

    auto result = mradm::io::import_scene(path.string());
    if (!result) {
        std::cerr << "FAIL: lock/divergence import_scene error: " << result.error().message << "\n";
        return false;
    }

    const auto& scene = result.value();
    ok &= check(scene.objects.size() == 1U, "lock/divergence: 1 object");
    if (scene.objects.size() == 1U && scene.objects[0].tracks.size() == 1U &&
        scene.objects[0].tracks[0].blocks.size() == 1U) {
        const auto& block = scene.objects[0].tracks[0].blocks[0];
        ok &= check(block.channel_lock, "channelLock flag imported");
        ok &= check(block.channel_lock_max_distance.has_value(), "channelLock maxDistance imported");
        if (block.channel_lock_max_distance.has_value()) {
            ok &= check(std::fabs(*block.channel_lock_max_distance - 1.0F) < 0.01F, "channelLock maxDistance ≈ 1");
        }
        ok &= check(std::fabs(block.divergence - 0.5F) < 0.01F, "objectDivergence divergence ≈ 0.5");
        ok &= check(std::fabs(block.divergence_azimuth_range - 60.0F) < 0.01F, "objectDivergence azimuthRange ≈ 60");
        ok &=
            check(std::fabs(block.divergence_position_range - 0.25F) < 0.01F, "objectDivergence positionRange ≈ 0.25");
    } else {
        ok &= check(false, "lock/divergence: expected one track block");
    }

    return ok;
}

bool verify_object_divergence_defaults_imported() {
    bool ok = true;
    auto [doc, uid_str] = make_object_lock_divergence_doc(false);
    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_divergence_defaults.wav";
    FileGuard guard{path};

    {
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
        auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> silence(1U, 0.0F);
        writer->write(silence.data(), 1U);
    }

    auto result = mradm::io::import_scene(path.string());
    if (!result) {
        std::cerr << "FAIL: divergence defaults import_scene error: " << result.error().message << "\n";
        return false;
    }

    const auto& scene = result.value();
    ok &= check(scene.objects.size() == 1U, "divergence defaults: 1 object");
    if (scene.objects.size() == 1U && scene.objects[0].tracks.size() == 1U &&
        scene.objects[0].tracks[0].blocks.size() == 1U) {
        const auto& block = scene.objects[0].tracks[0].blocks[0];
        ok &= check(block.channel_lock, "channelLock flag imported with unset maxDistance");
        ok &= check(!block.channel_lock_max_distance.has_value(), "channelLock maxDistance remains unset");
        ok &= check(std::fabs(block.divergence - 0.5F) < 0.01F, "objectDivergence divergence imported");
        ok &= check(std::fabs(block.divergence_azimuth_range - 45.0F) < 0.01F,
                    "objectDivergence default azimuthRange == 45");
        ok &= check(std::fabs(block.divergence_position_range) < 0.01F, "objectDivergence default positionRange == 0");
    } else {
        ok &= check(false, "divergence defaults: expected one track block");
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

bool verify_head_locked_precedence() {
    struct Case {
        std::optional<bool> object_value{std::nullopt};
        std::optional<bool> block_value{std::nullopt};
        bool expected{false};
        const char* name{""};
    };
    const std::array types{
        fixture::TypeDefinition::objects, fixture::TypeDefinition::direct_speakers, fixture::TypeDefinition::hoa};
    const std::array cases{
        Case{std::nullopt, std::nullopt, false, "missing -> default false"},
        Case{true, std::nullopt, true, "object true + block missing"},
        Case{true, false, false, "block explicit false overrides object true"},
        Case{false, true, true, "block explicit true overrides object false"},
    };

    bool ok = true;
    for (const auto type : types) {
        const char* type_name = "HOA";
        if (type == fixture::TypeDefinition::objects) {
            type_name = "Objects";
        } else if (type == fixture::TypeDefinition::direct_speakers) {
            type_name = "DirectSpeakers";
        }
        for (const auto& test_case : cases) {
            auto [doc, uid] = make_head_locked_doc(type, test_case.object_value, test_case.block_value);
            auto path = write_fixture(uid, serialize_doc(doc));
            const FileGuard guard{path};
            const auto result = mradm::io::import_scene(path.string());
            if (!result) {
                std::cerr << "FAIL: headLocked " << type_name << " / " << test_case.name
                          << " import failed: " << result.error().message << "\n";
                ok = false;
                continue;
            }

            const bool expected_object = test_case.object_value.value_or(false);
            bool shape_ok = result->objects.size() == 1U && result->objects[0].tracks.size() == 1U;
            bool effective = false;
            if (shape_ok && type == fixture::TypeDefinition::objects) {
                shape_ok = result->objects[0].tracks[0].blocks.size() == 1U;
                effective = shape_ok ? result->objects[0].tracks[0].blocks[0].head_locked : false;
            } else if (shape_ok && type == fixture::TypeDefinition::direct_speakers) {
                shape_ok = result->objects[0].tracks[0].ds_blocks.size() == 1U;
                effective = shape_ok ? result->objects[0].tracks[0].ds_blocks[0].head_locked : false;
            } else if (shape_ok) {
                shape_ok = result->hoa_tracks.size() == 1U && result->hoa_tracks[0].channels.size() == 1U &&
                           result->hoa_tracks[0].channels[0].blocks.size() == 1U;
                effective = shape_ok ? result->hoa_tracks[0].channels[0].blocks[0].head_locked : false;
                if (shape_ok && result->hoa_tracks[0].head_locked != expected_object) {
                    std::cerr << "FAIL: headLocked HOA pack fallback / " << test_case.name << "\n";
                    ok = false;
                }
            }

            if (!shape_ok || result->objects[0].head_locked != expected_object || effective != test_case.expected) {
                std::cerr << "FAIL: headLocked " << type_name << " / " << test_case.name
                          << " (object=" << result->objects[0].head_locked << ", effective=" << effective << ")\n";
                ok = false;
            }
        }
    }
    return ok;
}

// Build a document with one Objects AudioObject and one DirectSpeakers AudioObject
// sharing a single AudioContent and AudioProgramme.
// Returns {doc, objects_uid_str, ds_uid_str}.
std::tuple<std::shared_ptr<fixture::Document>, std::string, std::string> make_mixed_doc() {
    auto doc = fixture::Document::create();

    // Objects chain
    auto obj_cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"MixObjCF"},
                                                      fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
        obj_cf->add(block);
    }
    doc->add(obj_cf);
    auto obj_pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"MixObjPF"}, fixture::TypeDefinition::objects);
    obj_pf->add_reference(obj_cf);
    doc->add(obj_pf);
    auto obj_sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"MixObjSF"}, fixture::FormatDefinition::pcm);
    obj_sf->set_reference(obj_cf);
    doc->add(obj_sf);
    auto obj_tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"MixObjTF"}, fixture::FormatDefinition::pcm);
    obj_tf->set_reference(obj_sf);
    obj_sf->add_reference(obj_tf);
    doc->add(obj_tf);
    auto obj_uid = fixture::AudioTrackUid::create();
    obj_uid->set_reference(obj_tf);
    obj_uid->set_reference(obj_pf);
    doc->add(obj_uid);
    auto obj_audio_object = fixture::AudioObject::create(fixture::AudioObjectName{"MixObjObject"});
    obj_audio_object->add_reference(obj_uid);
    doc->add(obj_audio_object);

    // DirectSpeakers chain
    auto ds_cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"MixDsCF"},
                                                     fixture::TypeDefinition::direct_speakers);
    {
        fixture::AudioBlockFormatDirectSpeakers block{fixture::SphericalSpeakerPosition{
            fixture::Azimuth{30.0F}, fixture::Elevation{0.0F}, fixture::Distance{1.0F}}};
        block.add(fixture::SpeakerLabel{"M+030"});
        ds_cf->add(block);
    }
    doc->add(ds_cf);
    auto ds_pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"MixDsPF"},
                                                  fixture::TypeDefinition::direct_speakers);
    ds_pf->add_reference(ds_cf);
    doc->add(ds_pf);
    auto ds_sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"MixDsSF"}, fixture::FormatDefinition::pcm);
    ds_sf->set_reference(ds_cf);
    doc->add(ds_sf);
    auto ds_tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"MixDsTF"}, fixture::FormatDefinition::pcm);
    ds_tf->set_reference(ds_sf);
    ds_sf->add_reference(ds_tf);
    doc->add(ds_tf);
    auto ds_uid = fixture::AudioTrackUid::create();
    ds_uid->set_reference(ds_tf);
    ds_uid->set_reference(ds_pf);
    doc->add(ds_uid);
    auto ds_audio_object = fixture::AudioObject::create(fixture::AudioObjectName{"MixDsObject"});
    ds_audio_object->add_reference(ds_uid);
    doc->add(ds_audio_object);

    // Shared content and programme
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"MixedContent"});
    content->add_reference(obj_audio_object);
    content->add_reference(ds_audio_object);
    doc->add(content);
    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"MixedProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc,
            fixture::format_id(obj_uid->get<fixture::AudioTrackUidId>()),
            fixture::format_id(ds_uid->get<fixture::AudioTrackUidId>())};
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

// One DirectSpeakers pack may contain several channel formats.  Each
// AudioTrackUid must import only the channel format selected by its
// TrackFormat/StreamFormat chain; otherwise every bed input channel is routed to
// every speaker in the pack.
// NOLINTNEXTLINE(readability-function-size)
bool verify_direct_speakers_pack_channels_are_track_scoped() {
    bool ok = true;
    auto doc = fixture::Document::create();

    auto left_cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"BedLeftCF"},
                                                       fixture::TypeDefinition::direct_speakers);
    {
        fixture::AudioBlockFormatDirectSpeakers block{fixture::SphericalSpeakerPosition{
            fixture::Azimuth{30.0F}, fixture::Elevation{0.0F}, fixture::Distance{1.0F}}};
        block.add(fixture::SpeakerLabel{"M+030"});
        left_cf->add(block);
    }
    doc->add(left_cf);

    auto right_cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"BedRightCF"},
                                                        fixture::TypeDefinition::direct_speakers);
    {
        fixture::AudioBlockFormatDirectSpeakers block{fixture::SphericalSpeakerPosition{
            fixture::Azimuth{-30.0F}, fixture::Elevation{0.0F}, fixture::Distance{1.0F}}};
        block.add(fixture::SpeakerLabel{"M-030"});
        right_cf->add(block);
    }
    doc->add(right_cf);

    auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"BedPF"},
                                               fixture::TypeDefinition::direct_speakers);
    pf->add_reference(left_cf);
    pf->add_reference(right_cf);
    doc->add(pf);

    auto make_uid = [&](const std::shared_ptr<fixture::AudioChannelFormat>& cf, std::string_view name) {
        auto sf = fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{std::string{name} + "SF"},
                                                     fixture::FormatDefinition::pcm);
        sf->set_reference(cf);
        doc->add(sf);

        auto tf = fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{std::string{name} + "TF"},
                                                    fixture::FormatDefinition::pcm);
        tf->set_reference(sf);
        sf->add_reference(tf);
        doc->add(tf);

        auto uid = fixture::AudioTrackUid::create();
        uid->set_reference(tf);
        uid->set_reference(pf);
        doc->add(uid);
        return uid;
    };

    auto left_uid = make_uid(left_cf, "Left");
    auto right_uid = make_uid(right_cf, "Right");

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"BedObject"});
    object->add_reference(left_uid);
    object->add_reference(right_uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"BedContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"BedProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    left_uid->set(fixture::AudioTrackUidId{fixture::AudioTrackUidIdValue{0x0AU}});
    right_uid->set(fixture::AudioTrackUidId{fixture::AudioTrackUidIdValue{0x0BU}});
    const auto left_uid_str = fixture::format_id(left_uid->get<fixture::AudioTrackUidId>());
    const auto right_uid_str = fixture::format_id(right_uid->get<fixture::AudioTrackUidId>());
    auto upper_uid = [](std::string uid) {
        std::ranges::transform(uid, uid.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return uid;
    };

    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_ds_track_scoped.wav";
    FileGuard guard{path};
    {
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{
            bw64::AudioId(1, upper_uid(left_uid_str), "", ""),
            bw64::AudioId(2, upper_uid(right_uid_str), "", ""),
        });
        auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
        auto writer = bw64::writeFile(path.string(), 2U, 48000U, 24U, chna, axml);
        std::vector<float> silence(2, 0.0F);
        writer->write(silence.data(), 1U);
    }

    auto result = mradm::io::import_scene(path.string());
    if (!result.has_value()) {
        std::cerr << "FAIL: DS track-scoped import failed: " << result.error().message << "\n";
        return false;
    }

    const auto& scene = result.value();
    ok &= check(scene.objects.size() == 1, "DS track-scoped: 1 object");
    if (scene.objects.empty()) {
        return false;
    }
    const auto& tracks = scene.objects[0].tracks;
    ok &= check(tracks.size() == 2, "DS track-scoped: 2 tracks");
    if (tracks.size() != 2U) {
        return false;
    }

    ok &= check(tracks[0].ds_blocks.size() == 1, "DS track-scoped: left track has one DS block");
    ok &= check(tracks[1].ds_blocks.size() == 1, "DS track-scoped: right track has one DS block");
    if (!tracks[0].ds_blocks.empty() && !tracks[1].ds_blocks.empty()) {
        const auto& left = tracks[0].ds_blocks[0];
        const auto& right = tracks[1].ds_blocks[0];
        ok &= check(std::ranges::find(left.speaker_labels, "M+030") != left.speaker_labels.end(),
                    "DS track-scoped: left track label M+030");
        ok &= check(std::ranges::find(left.speaker_labels, "M-030") == left.speaker_labels.end(),
                    "DS track-scoped: left track does not inherit right label");
        ok &= check(std::ranges::find(right.speaker_labels, "M-030") != right.speaker_labels.end(),
                    "DS track-scoped: right track label M-030");
        ok &= check(std::ranges::find(right.speaker_labels, "M+030") == right.speaker_labels.end(),
                    "DS track-scoped: right track does not inherit left label");
    }

    return ok;
}

// Build a one-channel Binaural ADM document so we can verify the import warning.
std::pair<std::shared_ptr<fixture::Document>, std::string> make_binaural_doc() {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"BinauralCF"},
                                                  fixture::TypeDefinition::binaural);
    doc->add(cf);

    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"BinauralPF"}, fixture::TypeDefinition::binaural);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf = fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"BinauralSF"},
                                                 fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"BinauralTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"BinauralObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"BinauralContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"BinauralProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

// Build a one-channel DirectSpeakers ADM document with channelFrequency.lowPass
// set on the CF to identify it as an LFE channel.
std::pair<std::shared_ptr<fixture::Document>, std::string> make_ds_lfe_doc() {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"LfeCF"},
                                                  fixture::TypeDefinition::direct_speakers);
    cf->set(fixture::Frequency{fixture::LowPass{120.0F}});
    {
        fixture::AudioBlockFormatDirectSpeakers block{fixture::SphericalSpeakerPosition{
            fixture::Azimuth{45.0F}, fixture::Elevation{-30.0F}, fixture::Distance{1.0F}}};
        block.add(fixture::SpeakerLabel{"LFE1"});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"LfePF"},
                                               fixture::TypeDefinition::direct_speakers);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"LfeSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf = fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"LfeTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"LfeObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"LfeContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"LfeProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

// Build a minimal Objects ADM document with LoudnessMetadata on its programme.
std::pair<std::shared_ptr<fixture::Document>, std::string> make_loudness_doc() {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"LoudnessCF"},
                                                  fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);

    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"LoudnessPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf = fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"LoudnessSF"},
                                                 fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"LoudnessTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"LoudnessObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"LoudnessContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"LoudnessProgramme"});
    programme->add_reference(content);
    {
        fixture::LoudnessMetadata lm;
        lm.set(fixture::IntegratedLoudness{-23.0F});
        lm.set(fixture::MaxTruePeak{-1.0F});
        lm.set(fixture::LoudnessRange{6.5F});
        lm.set(fixture::MaxMomentary{-18.5F});
        lm.set(fixture::MaxShortTerm{-20.0F});
        lm.set(fixture::DialogueLoudness{-24.0F});
        lm.set(fixture::LoudnessMethod{"ITU-R BS.1770-4"});
        programme->set(fixture::LoudnessMetadatas{lm});
    }
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
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
std::pair<std::shared_ptr<fixture::Document>, std::string> make_content_metadata_doc() {
    auto doc = fixture::Document::create();

    auto cf =
        fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"CMdCF"}, fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);

    auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"CMdPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"CMdSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf = fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"CMdTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"CMdObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"CMdContent"});
    content->add_reference(object);
    content->set(fixture::AudioContentLanguage{"en"});
    {
        fixture::Label label;
        label.set(fixture::LabelValue{"English Commentary"});
        label.set(fixture::LabelLanguage{"en"});
        content->add(label);
    }
    {
        fixture::LoudnessMetadata lm;
        lm.set(fixture::IntegratedLoudness{-24.0F});
        lm.set(fixture::MaxTruePeak{-2.0F});
        content->set(fixture::LoudnessMetadatas{lm});
    }
    content->set(fixture::DialogueContent::voiceover);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"CMdProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
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
std::pair<std::shared_ptr<fixture::Document>, std::string> make_reference_screen_doc() {
    auto doc = fixture::Document::create();

    auto uid = fixture::AudioTrackUid::create();
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"RSObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"RSContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"RSProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
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

std::pair<std::shared_ptr<fixture::Document>, std::string> make_programme_extended_doc() {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"PExtCF"},
                                                  fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);

    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"PExtPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"PExtSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"PExtTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"PExtObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"PExtContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"PExtProgramme"});
    programme->add_reference(content);
    programme->set(fixture::AudioProgrammeLanguage{"fr"});
    {
        fixture::Label label;
        label.set(fixture::LabelValue{"French Main Mix"});
        label.set(fixture::LabelLanguage{"fr"});
        programme->add(label);
    }
    // start at 1 second, end at 5 seconds (at 48000 Hz)
    programme->set(fixture::Start{fixture::Time{std::chrono::nanoseconds{1'000'000'000LL}}});
    programme->set(fixture::End{fixture::Time{std::chrono::nanoseconds{5'000'000'000LL}}});
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

bool verify_programme_language_labels_time_imported() {
    bool ok = true;
    auto [doc, uid_str] = make_programme_extended_doc();
    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_prog_ext.wav";
    FileGuard guard{path};

    {
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc));
        auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> silence(48000, 0.0F);
        writer->write(silence.data(), 48000U);
    }

    auto result = mradm::io::import_scene(path.string());
    if (!result) {
        std::cerr << "FAIL: programme_extended import_scene error: " << result.error().message << "\n";
        return false;
    }
    const auto& scene = result.value();

    ok &= check(!scene.programmes.empty(), "programme extended: 1 programme");
    if (scene.programmes.empty()) {
        return false;
    }
    const auto& prog = scene.programmes[0];

    ok &= check(prog.language.has_value() && *prog.language == "fr", "programme: language == 'fr'");
    ok &= check(!prog.labels.empty() && prog.labels[0] == "French Main Mix", "programme: label value");
    ok &= check(prog.start_sample == 48000U, "programme: start_sample == 48000");
    ok &= check(prog.end_sample.has_value() && *prog.end_sample == 240000U, "programme: end_sample == 240000");

    if (ok) {
        std::cout << "PASS: verify_programme_language_labels_time_imported\n";
    }
    return ok;
}

std::pair<std::shared_ptr<fixture::Document>, std::string> make_object_extended_doc() {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"OExtCF"},
                                                  fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);

    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"OExtPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"OExtSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"OExtTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"OExtObject"});
    object->add_reference(uid);
    {
        fixture::Label label;
        label.set(fixture::LabelValue{"Main Dialogue"});
        label.set(fixture::LabelLanguage{"en"});
        object->add(label);
    }
    object->set(fixture::Importance{7});
    object->set(fixture::DialogueId{1}); // dialogue
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"OExtContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"OExtProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

bool verify_object_labels_importance_dialogue_imported() {
    bool ok = true;
    auto [doc, uid_str] = make_object_extended_doc();
    auto path = std::filesystem::temp_directory_path() / "mr_adm_io_obj_ext.wav";
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
        std::cerr << "FAIL: object_extended import_scene error: " << result.error().message << "\n";
        return false;
    }
    const auto& scene = result.value();

    ok &= check(!scene.objects.empty(), "object extended: 1 object");
    if (scene.objects.empty()) {
        return false;
    }
    const auto& obj = scene.objects[0];

    ok &= check(!obj.labels.empty() && obj.labels[0] == "Main Dialogue", "object: label value");
    ok &= check(obj.importance.has_value() && *obj.importance == 7, "object: importance == 7");
    ok &= check(obj.dialogue_id.has_value() && *obj.dialogue_id == 1U, "object: dialogue_id == 1");

    if (ok) {
        std::cout << "PASS: verify_object_labels_importance_dialogue_imported\n";
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_minimal_fixture();
    ok &= verify_object_semantic_provenance();
    ok &= verify_objects_blocks_fixture();
    ok &= verify_fractional_block_boundaries_are_contiguous();
    ok &= verify_direct_speakers_blocks_fixture();
    ok &= verify_head_locked_precedence();
    ok &= verify_hoa_pack_metadata();
    ok &= verify_mixed_blocks_fixture();
    ok &= verify_direct_speakers_pack_channels_are_track_scoped();
    ok &= verify_binaural_skipped_produces_import_warning();
    ok &= verify_ds_lfe_channel_frequency_imported();
    ok &= verify_programme_loudness_metadata_imported();
    ok &= verify_content_metadata_imported();
    ok &= verify_reference_screen_flag_imported();
    ok &= verify_programme_language_labels_time_imported();
    ok &= verify_object_labels_importance_dialogue_imported();
    ok &= verify_object_lock_divergence_imported();
    ok &= verify_object_divergence_defaults_imported();

    if (ok) {
        std::cout << "adm_io fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
