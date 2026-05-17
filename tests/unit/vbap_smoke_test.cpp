#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm/render.h"
#include "adm/render_vbap.h"

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

enum class ObjectPositionMode : std::uint8_t {
    polar_front,
    cartesian_front,
};

std::pair<std::shared_ptr<adm::Document>, std::string> make_objects_doc(ObjectPositionMode mode) {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"VbapCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block =
            (mode == ObjectPositionMode::cartesian_front)
                ? adm::AudioBlockFormatObjects{adm::CartesianPosition{adm::X{0.0F}, adm::Y{1.0F}, adm::Z{0.0F}}}
                : adm::AudioBlockFormatObjects{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"VbapPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"VbapSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"VbapTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"VbapObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"VbapContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"VbapProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::filesystem::path write_input_fixture(const std::shared_ptr<adm::Document>& doc, const std::string& uid_str) {
    auto path = std::filesystem::temp_directory_path() / "mr_vbap_fixture_in.wav";

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    constexpr uint32_t k_frames = 1000U;
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    std::vector<float> samples(k_frames, 0.5F);
    writer->write(samples.data(), k_frames);

    return path;
}

bool verify_vbap_render_fixture(ObjectPositionMode mode, const char* label) {
    auto [doc, uid_str] = make_objects_doc(mode);
    const auto in_path = write_input_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_fixture_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+2+0";
    request.options.renderer = mradm::RendererSelection::saf;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        std::cerr << "FAIL: " << label << " VBAP fixture render failed: " << result.error.message << "\n";
        return false;
    }

    try {
        auto out_reader = bw64::readFile(out_path.string());
        bool ok = true;
        ok &= check(out_reader->channels() == 2U, "VBAP output has 2 channels");
        ok &= check(out_reader->sampleRate() == 48000U, "VBAP output sample rate == 48000");
        ok &= check(out_reader->numberOfFrames() == 1000U, "VBAP output frame count == 1000");

        if (ok) {
            const auto n_frames = static_cast<std::size_t>(out_reader->numberOfFrames());
            std::vector<float> out_samples(n_frames * 2U);
            out_reader->read(out_samples.data(), out_reader->numberOfFrames());

            double sum_l = 0.0;
            double sum_r = 0.0;
            for (std::size_t frame = 0; frame < n_frames; frame++) {
                sum_l += std::fabs(static_cast<double>(out_samples[2U * frame]));
                sum_r += std::fabs(static_cast<double>(out_samples[(2U * frame) + 1U]));
            }
            ok &= check(sum_l > 0.0, "VBAP left channel is not silent");
            ok &= check(sum_r > 0.0, "VBAP right channel is not silent");
            const double ratio = (sum_l > 0.0) ? (sum_r / sum_l) : 0.0;
            ok &= check(ratio > 0.95 && ratio < 1.05, "VBAP front object has L≈R energy");
        }
        return ok;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: cannot open VBAP output: " << e.what() << "\n";
        return false;
    }
}

// Verify MDAP spread: Objects with width=0.8 rendered to 4+5+0 (9ch, has
// elevated speakers at ±45°) should distribute energy to elevated channels.
// Compare against the same source with width=0 (pure VBAP) to confirm spread
// actually changes the gain distribution.
bool verify_mdap_spread_fixture() {
    // Build two ADM docs: one with width=0 (no spread), one with width=0.8.
    auto make_doc = [](float width) {
        auto doc = adm::Document::create();
        auto cf =
            adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"MdapCF"}, adm::TypeDefinition::OBJECTS);
        {
            adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
            block.set(adm::Gain{1.0F});
            block.set(adm::Width{width});
            cf->add(block);
        }
        doc->add(cf);
        auto pf =
            adm::AudioPackFormat::create(adm::AudioPackFormatName{"MdapPF"}, adm::TypeDefinition::OBJECTS);
        pf->addReference(cf);
        doc->add(pf);
        auto sf =
            adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"MdapSF"}, adm::FormatDefinition::PCM);
        sf->setReference(cf);
        doc->add(sf);
        auto tf =
            adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"MdapTF"}, adm::FormatDefinition::PCM);
        tf->setReference(sf);
        sf->addReference(tf);
        doc->add(tf);
        auto uid = adm::AudioTrackUid::create();
        uid->setReference(tf);
        uid->setReference(pf);
        doc->add(uid);
        auto obj = adm::AudioObject::create(adm::AudioObjectName{"MdapObj"});
        obj->addReference(uid);
        doc->add(obj);
        auto content = adm::AudioContent::create(adm::AudioContentName{"MdapContent"});
        content->addReference(obj);
        doc->add(content);
        auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"MdapProg"});
        prog->addReference(content);
        doc->add(prog);
        adm::reassignIds(doc);
        return std::make_pair(doc, adm::formatId(uid->get<adm::AudioTrackUidId>()));
    };

    auto render_and_sum_elevated = [&](float width, double& out_elevated_sum) -> bool {
        auto [doc, uid_str] = make_doc(width);
        auto path_in = std::filesystem::temp_directory_path() / "mr_vbap_mdap_in.wav";
        FileGuard in_guard{path_in};
        {
            std::ostringstream xml_buf;
            adm::writeXml(xml_buf, doc);
            auto chna =
                std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
            auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
            constexpr uint32_t k_frames = 1000U;
            auto writer = bw64::writeFile(path_in.string(), 1U, 48000U, 24U, chna, axml);
            std::vector<float> samples(k_frames, 0.5F);
            writer->write(samples.data(), k_frames);
        }

        auto path_out = std::filesystem::temp_directory_path() / "mr_vbap_mdap_out.wav";
        FileGuard out_guard{path_out};

        mradm::RenderRequest req;
        req.input_path = path_in;
        req.output_path = path_out;
        req.options.output_layout = "4+5+0"; // 9ch: ch0-4 horizontal, ch5-8 elevated
        req.options.renderer = mradm::RendererSelection::saf;

        mradm::RenderService service;
        mradm::NullProgressSink progress;
        mradm::NullLogSink logs;
        const auto res = service.render(req, progress, logs);
        if (!res.success()) {
            std::cerr << "FAIL: MDAP render (width=" << width << ") failed: " << res.error.message << "\n";
            return false;
        }

        auto reader = bw64::readFile(path_out.string());
        constexpr std::size_t k_num_ch = 9U;
        const auto n_frames = static_cast<std::size_t>(reader->numberOfFrames());
        std::vector<float> samples(n_frames * k_num_ch);
        reader->read(samples.data(), reader->numberOfFrames());

        // Channels 5–8 are the elevated speakers (elevation ±45°).
        out_elevated_sum = 0.0;
        for (std::size_t f = 0; f < n_frames; f++) {
            for (std::size_t ch = 5U; ch < k_num_ch; ch++) {
                out_elevated_sum += std::fabs(static_cast<double>(samples[f * k_num_ch + ch]));
            }
        }
        return true;
    };

    double elevated_no_spread = 0.0;
    double elevated_with_spread = 0.0;
    bool ok = true;
    ok &= render_and_sum_elevated(0.0F, elevated_no_spread);
    ok &= render_and_sum_elevated(0.8F, elevated_with_spread);
    if (!ok) {
        return false;
    }
    ok &= check(elevated_with_spread > elevated_no_spread,
                "MDAP spread: elevated channels get more energy with width=0.8 than width=0");
    return ok;
}

} // namespace

int main() {
    bool ok = true;

    // ── Capabilities ──────────────────────────────────────────────────────────
    const auto caps = mradm::vbap_capabilities();

    if (caps.backend_name != "saf-vbap") {
        std::cerr << "FAIL: expected backend_name 'saf-vbap', got '" << caps.backend_name << "'\n";
        ok = false;
    }
    if (caps.supported_layouts.empty()) {
        std::cerr << "FAIL: supported_layouts must not be empty\n";
        ok = false;
    }
    if (!caps.supports_objects) {
        std::cerr << "FAIL: saf-vbap must declare supports_objects\n";
        ok = false;
    }

    // ── Engine routing ────────────────────────────────────────────────────────
    // Nonexistent file → io_error at scene import; proves the backend is
    // recognised by RenderService (no "renderer not available" short-circuit).
    mradm::RenderRequest request;
    request.input_path = "/tmp/nonexistent_mr_vbap_test_xyz.wav";
    request.output_path = "/tmp/mr_vbap_test_out_xyz.wav";
    request.options.renderer = mradm::RendererSelection::saf;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);

    if (result.success()) {
        std::cerr << "FAIL: expected error for nonexistent file, got success\n";
        ok = false;
    }
    // io_error comes from scene import; unsupported would mean engine rejected
    // the backend before even trying. Both are wrong outcomes here.
    if (result.error.code != mradm::ErrorCode::io_error) {
        std::cerr << "FAIL: expected io_error (scene import), got code " << static_cast<int>(result.error.code)
                  << " — message: " << result.error.message << "\n";
        ok = false;
    }

    ok &= verify_vbap_render_fixture(ObjectPositionMode::polar_front, "polar front");
    ok &= verify_vbap_render_fixture(ObjectPositionMode::cartesian_front, "cartesian front");
    ok &= verify_mdap_spread_fixture();

    if (ok) {
        std::cout << "vbap smoke test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
