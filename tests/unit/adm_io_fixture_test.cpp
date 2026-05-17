#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
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

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"TestCF"},
                                              adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{
            adm::SphericalPosition{adm::Azimuth{30.0f}, adm::Elevation{10.0f}}};
        block.set(adm::Gain{0.8f});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"TestPF"},
                                           adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    // AudioStreamFormat + AudioTrackFormat are required for a valid UID reference chain.
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"TestSF"},
                                             adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TestTF"},
                                            adm::FormatDefinition::PCM);
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

} // namespace

int main() {
    auto [doc, uid_str] = make_minimal_doc();
    std::string xml_str = serialize_doc(doc);
    auto path = write_fixture(uid_str, xml_str);
    FileGuard guard{path};

    auto result = mradm::io::import_scene(path.string());

    bool ok = true;

    if (!result.has_value()) {
        std::cerr << "FAIL: import_scene returned error: " << result.error().message << "\n";
        return EXIT_FAILURE;
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

    // --- Section 2: SceneObjectBlock population from a full Objects chain ---
    {
        auto [doc2, uid2_str] = make_objects_doc();
        auto path2 = std::filesystem::temp_directory_path() / "mr_adm_io_blocks_fixture.wav";
        FileGuard guard2{path2};

        {
            auto chna2 = std::make_shared<bw64::ChnaChunk>(
                std::vector<bw64::AudioId>{bw64::AudioId(1, uid2_str, "", "")});
            auto axml2 = std::make_shared<bw64::AxmlChunk>(serialize_doc(doc2));
            auto writer2 = bw64::writeFile(path2.string(), 1U, 48000U, 24U, chna2, axml2);
            (void) writer2;
        }

        auto result2 = mradm::io::import_scene(path2.string());
        if (!result2.has_value()) {
            std::cerr << "FAIL: objects import failed: " << result2.error().message << "\n";
            return EXIT_FAILURE;
        }

        const auto& s2 = result2.value();
        ok &= check(s2.objects.size() == 1, "objects doc: 1 object");
        if (s2.objects.size() == 1 && s2.objects[0].tracks.size() == 1) {
            const auto& track = s2.objects[0].tracks[0];
            ok &= check(track.blocks.size() == 1, "objects track: 1 block");
            if (!track.blocks.empty()) {
                const auto& blk = track.blocks[0];
                ok &= check(!blk.position.cartesian, "position is polar");
                ok &= check(std::fabs(blk.position.azimuth - 30.0f) < 0.01f, "azimuth ≈ 30");
                ok &= check(std::fabs(blk.position.elevation - 10.0f) < 0.01f, "elevation ≈ 10");
                ok &= check(std::fabs(blk.position.distance - 1.0f) < 0.01f, "distance ≈ 1 (default)");
                ok &= check(std::fabs(blk.gain - 0.8f) < 0.01f, "gain ≈ 0.8");
            }
        }
    }

    if (ok) {
        std::cout << "adm_io fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
