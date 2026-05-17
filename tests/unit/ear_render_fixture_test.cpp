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

// libbw64 – write and read BW64 files
#include <bw64/bw64.hpp>

// Our engine
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

} // namespace

int main() {
    auto [doc, uid_str] = make_objects_doc();
    const auto in_path = write_input_fixture(uid_str, doc);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_ear_fixture_out.wav";
    FileGuard out_guard{out_path};

    // Render via RenderService (full engine stack)
    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+2+0";
    request.options.renderer = mradm::RendererSelection::ear;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);

    bool ok = true;

    if (!result.success()) {
        std::cerr << "FAIL: render failed: " << result.error.message << "\n";
        return EXIT_FAILURE;
    }

    // Open and verify output (use auto: bw64 reader type name is internal)
    try {
        auto out_reader = bw64::readFile(out_path.string());

        ok &= check(out_reader->channels() == 2U, "output has 2 channels");
        ok &= check(out_reader->sampleRate() == 48000U, "output sample rate == 48000");
        ok &= check(out_reader->numberOfFrames() == 1000U, "output frame count == 1000");

        if (ok) {
            // Read output samples; verify non-silence and L≈R symmetry for center object.
            const auto n_frames = static_cast<std::size_t>(out_reader->numberOfFrames());
            std::vector<float> out_samples(n_frames * 2U);
            out_reader->read(out_samples.data(), out_reader->numberOfFrames());

            double sum_l = 0.0;
            double sum_r = 0.0;
            for (std::size_t f = 0; f < n_frames; f++) {
                sum_l += std::fabs(static_cast<double>(out_samples[2U * f]));
                sum_r += std::fabs(static_cast<double>(out_samples[(2U * f) + 1U]));
            }
            ok &= check(sum_l > 0.0, "left channel is not silent");
            ok &= check(sum_r > 0.0, "right channel is not silent");

            // Center object (az=0) → equal L and R gains (within 5%)
            const double ratio = (sum_l > 0.0) ? (sum_r / sum_l) : 0.0;
            ok &= check(ratio > 0.95 && ratio < 1.05, "L≈R energy for center object");
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: cannot open output: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    if (ok) {
        std::cout << "ear_render fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
