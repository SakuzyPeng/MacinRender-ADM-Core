#include <algorithm>
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

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_hoa.h"

namespace {

constexpr int k_hoa3_channels = 16;
constexpr float k_amplitude = 0.5F;
constexpr uint32_t k_frames = 1000U;

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

std::pair<std::shared_ptr<adm::Document>, std::string> make_objects_doc(float az_deg, float el_deg) {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"HoaCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{az_deg}, adm::Elevation{el_deg}}};
        block.set(adm::Gain{static_cast<double>(k_amplitude)});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"HoaPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"HoaSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"HoaTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"HoaObj"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"HoaContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"HoaProg"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    const auto uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());
    return {doc, uid_str};
}

std::filesystem::path write_fixture(const std::shared_ptr<adm::Document>& doc, const std::string& uid_str) {
    auto path = std::filesystem::temp_directory_path() / "mr_hoa_enc_in.wav";
    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    std::vector<float> samples(k_frames, 1.0F);
    writer->write(samples.data(), k_frames);
    return path;
}

// Read per-channel RMS energy from a 16ch HOA3 output file.
std::vector<double> read_channel_rms(const std::filesystem::path& path) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        return std::vector<double>(k_hoa3_channels, 0.0);
    }
    auto& reader = *reader_res;
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(n_frames * k_hoa3_channels);
    reader.read(samples.data(), reader.frame_count());

    std::vector<double> rms(k_hoa3_channels, 0.0);
    for (std::size_t f = 0; f < n_frames; ++f) {
        for (std::size_t ch = 0; ch < static_cast<std::size_t>(k_hoa3_channels); ++ch) {
            const auto s = static_cast<double>(samples[(f * static_cast<std::size_t>(k_hoa3_channels)) + ch]);
            rms[ch] += s * s;
        }
    }
    std::ranges::transform(
        rms, rms.begin(), [n_frames](double r) { return std::sqrt(r / static_cast<double>(n_frames)); });
    return rms;
}

// Front source (az=0, el=0): in SN3D, W=gain and X=gain; Y=Z=0.
bool verify_front_source() {
    auto [doc, uid_str] = make_objects_doc(0.0F, 0.0F);
    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_front.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "hoa3";
    req.options.renderer = mradm::RendererSelection::hoa;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: front source render failed: " << res.error.message << "\n";
        return false;
    }

    auto hdr_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!hdr_res) {
        std::cerr << "FAIL: cannot open HOA3 output: " << hdr_res.error().message << "\n";
        return false;
    }
    auto& hdr = *hdr_res;
    bool ok = true;
    ok &= check(static_cast<int>(hdr.channels()) == k_hoa3_channels, "HOA3 output has 16 channels");
    ok &= check(hdr.sample_rate() == 48000U, "HOA3 output sample rate == 48000");
    ok &= check(hdr.frame_count() == k_frames, "HOA3 output frame count == 1000");
    if (!ok) {
        return false;
    }

    const auto rms = read_channel_rms(out_path);
    // Front source: W (ch0) = gain, X (ch3) = gain, Y (ch1) = Z (ch2) = 0.
    const double w = rms[0];
    const double y = rms[1];
    const double z = rms[2];
    const double x = rms[3];
    ok &= check(w > 0.4 && w < 0.6, "HOA3 front: W (ACN 0) ≈ gain");
    ok &= check(x > 0.4 && x < 0.6, "HOA3 front: X (ACN 3) ≈ gain");
    ok &= check(y < 1e-4, "HOA3 front: Y (ACN 1) ≈ 0");
    ok &= check(z < 1e-4, "HOA3 front: Z (ACN 2) ≈ 0");
    return ok;
}

// Left source (az=90, el=0): W=gain, Y=gain, X=Z=0.
bool verify_left_source() {
    auto [doc, uid_str] = make_objects_doc(90.0F, 0.0F);
    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_left.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "hoa3";
    req.options.renderer = mradm::RendererSelection::hoa;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: left source render failed: " << res.error.message << "\n";
        return false;
    }

    const auto rms = read_channel_rms(out_path);
    // Left source: W=gain, Y=gain, X=Z=0
    bool ok = true;
    ok &= check(rms[0] > 0.4 && rms[0] < 0.6, "HOA3 left: W (ACN 0) ≈ gain");
    ok &= check(rms[1] > 0.4 && rms[1] < 0.6, "HOA3 left: Y (ACN 1) ≈ gain");
    ok &= check(rms[2] < 1e-4, "HOA3 left: Z (ACN 2) ≈ 0");
    ok &= check(rms[3] < 1e-4, "HOA3 left: X (ACN 3) ≈ 0");
    return ok;
}

} // namespace

int main() {
    bool ok = true;

    // ── Capabilities ──────────────────────────────────────────────────────────
    const auto caps = mradm::hoa_capabilities();
    if (caps.backend_name != "hoa-encode") {
        std::cerr << "FAIL: expected backend_name 'hoa-encode', got '" << caps.backend_name << "'\n";
        ok = false;
    }
    if (caps.supported_layouts.empty()) {
        std::cerr << "FAIL: supported_layouts must not be empty\n";
        ok = false;
    }

    // ── Fixtures ──────────────────────────────────────────────────────────────
    ok &= verify_front_source();
    ok &= verify_left_source();

    if (ok) {
        std::cout << "hoa encode fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
