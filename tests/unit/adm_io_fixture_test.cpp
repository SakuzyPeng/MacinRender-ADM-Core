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

    if (ok) {
        std::cout << "adm_io fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
