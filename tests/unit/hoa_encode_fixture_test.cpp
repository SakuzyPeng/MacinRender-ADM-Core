#include <algorithm>
#include <chrono>
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

std::pair<std::shared_ptr<adm::Document>, std::string> make_objects_doc(float az_deg,
                                                                        float el_deg,
                                                                        float diffuse = 0.0F,
                                                                        float width = 0.0F,
                                                                        float height = 0.0F,
                                                                        float depth = 0.0F,
                                                                        float divergence = 0.0F,
                                                                        float divergence_range = 45.0F) {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"HoaCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{az_deg}, adm::Elevation{el_deg}}};
        block.set(adm::Gain{static_cast<double>(k_amplitude)});
        if (diffuse > 0.0F) {
            block.set(adm::Diffuse{diffuse});
        }
        if (width > 0.0F) {
            block.set(adm::Width{width});
        }
        if (height > 0.0F) {
            block.set(adm::Height{height});
        }
        if (depth > 0.0F) {
            block.set(adm::Depth{depth});
        }
        if (divergence > 0.0F) {
            adm::ObjectDivergence od;
            od.set(adm::Divergence{divergence});
            od.set(adm::AzimuthRange{divergence_range});
            block.set(od);
        }
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

std::filesystem::path write_fixture_samples(const std::shared_ptr<adm::Document>& doc,
                                            const std::string& uid_str,
                                            const std::string& suffix,
                                            const std::vector<float>& samples) {
    auto path = std::filesystem::temp_directory_path() / ("mr_hoa_enc_" + suffix + ".wav");
    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    writer->write(samples.data(), static_cast<uint32_t>(samples.size()));
    return path;
}

std::vector<float> make_noise_samples(std::size_t frames) {
    std::vector<float> samples(frames, 0.0F);
    uint32_t state = 0x5a17c0deU;
    for (float& sample : samples) {
        state = (state * 1664525U) + 1013904223U;
        const float unit = static_cast<float>((state >> 8U) & 0xffffU) / 32768.0F;
        sample = (unit - 1.0F) * 0.5F;
    }
    return samples;
}

// Read per-channel RMS energy from a 16ch HOA3 output file.
std::vector<double> read_channel_rms(const std::filesystem::path& path) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        std::vector<double> empty(k_hoa3_channels, 0.0);
        return empty;
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

// Read all samples from a 16ch HOA output as a flat frame-interleaved buffer.
std::vector<float> read_hoa_raw_samples(const std::filesystem::path& path) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        return {};
    }
    auto& reader = *reader_res;
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> buf(n_frames * static_cast<std::size_t>(k_hoa3_channels), 0.0F);
    reader.read(buf.data(), reader.frame_count());
    return buf;
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

// Two-block doc: block0 at az=0 (front) for 10 ms, block1 at az=90 (left) with jump_position.
std::pair<std::shared_ptr<adm::Document>, std::string> make_two_block_objects_doc() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"HoaCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block0{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block0.set(adm::Gain{static_cast<double>(k_amplitude)});
        block0.set(adm::Duration{adm::Time{std::chrono::milliseconds{10}}});
        cf->add(block0);

        adm::AudioBlockFormatObjects block1{adm::SphericalPosition{adm::Azimuth{90.0F}, adm::Elevation{0.0F}}};
        block1.set(adm::Gain{static_cast<double>(k_amplitude)});
        block1.set(adm::Rtime{adm::Time{std::chrono::milliseconds{10}}});
        block1.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block1);
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
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_two_block_diffuse_doc() {
    auto doc = adm::Document::create();
    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"HoaDiffuseCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block0{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block0.set(adm::Gain{static_cast<double>(k_amplitude)});
        block0.set(adm::Diffuse{0.0F});
        block0.set(adm::Duration{adm::Time{std::chrono::milliseconds{10}}});
        cf->add(block0);

        adm::AudioBlockFormatObjects block1{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block1.set(adm::Gain{static_cast<double>(k_amplitude)});
        block1.set(adm::Diffuse{1.0F});
        block1.set(adm::Rtime{adm::Time{std::chrono::milliseconds{10}}});
        cf->add(block1);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"HoaDiffusePF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"HoaDiffuseSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"HoaDiffuseTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"HoaDiffuseObj"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"HoaDiffuseContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"HoaDiffuseProg"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

double
channel_rms_range(const std::vector<float>& raw, std::size_t channel, std::size_t begin_frame, std::size_t frames) {
    constexpr auto k_ch = static_cast<std::size_t>(k_hoa3_channels);
    double energy = 0.0;
    for (std::size_t f = begin_frame; f < begin_frame + frames; ++f) {
        const auto s = static_cast<double>(raw.at((f * k_ch) + channel));
        energy += s * s;
    }
    return std::sqrt(energy / static_cast<double>(frames));
}

// All-muted AudioObject must produce silent output (not render_failed).
bool verify_hoa_mute_writes_silence() {
    auto [doc, uid_str] = make_objects_doc(0.0F, 0.0F);
    for (const auto& obj : doc->getElements<adm::AudioObject>()) {
        obj->set(adm::Mute{true});
    }
    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_mute.wav";
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
        std::cerr << "FAIL: all-muted HOA must write silence, not fail: " << res.error.message << "\n";
        return false;
    }
    const auto rms = read_channel_rms(out_path);
    bool ok = true;
    for (std::size_t ch = 0; ch < static_cast<std::size_t>(k_hoa3_channels); ++ch) {
        ok &= check(rms[ch] < 1e-6, "HOA muted: all channels silent");
    }
    return ok;
}

// AudioObject.gain applied correctly (obj.gain=0.5 halves block energy).
bool verify_hoa_obj_gain() {
    auto [doc, uid_str] = make_objects_doc(0.0F, 0.0F);
    for (const auto& obj : doc->getElements<adm::AudioObject>()) {
        obj->set(adm::Gain{0.5});
    }
    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_objgain.wav";
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
        std::cerr << "FAIL: HOA obj_gain render failed: " << res.error.message << "\n";
        return false;
    }
    const auto rms = read_channel_rms(out_path);
    // block.gain = k_amplitude = 0.5, obj.gain = 0.5 → combined gain = 0.25
    // W (ch0) at front: SH[0,0] = 1.0, so W ≈ 0.25; accept 0.15–0.35
    return check(rms[0] > 0.15 && rms[0] < 0.35, "HOA obj_gain=0.5: W ≈ 0.25");
}

// jump_position=true switches SH coefficients instantaneously.
bool verify_hoa_jump_position() {
    auto [doc, uid_str] = make_two_block_objects_doc();
    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_jump.wav";
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
        std::cerr << "FAIL: HOA jump_position render failed: " << res.error.message << "\n";
        return false;
    }
    const auto rms = read_channel_rms(out_path);
    bool ok = true;
    // Block0 (front, 10ms): contributes W and X. Block1 (left, ~11ms): contributes W and Y.
    ok &= check(rms[0] > 0.3, "HOA jump: W non-silent (both blocks contribute)");
    ok &= check(rms[1] > 0.1, "HOA jump: Y non-silent (left block contributes)");
    ok &= check(rms[3] > 0.1, "HOA jump: X non-silent (front block contributes)");
    return ok;
}

// jumpPosition=false: linear ramp from block0 (front) to block1 (left).
// At ramp start (frame 480, delta=0): gains = prev block (front) → ch3(X)≈0.5, ch1(Y)≈0.
// After ramp  (frame 720, delta=interp_len): gains = cur block (left) → ch3(X)≈0, ch1(Y)≈0.5.
bool verify_hoa_ramp_interpolation() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"HoaCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block0{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block0.set(adm::Gain{static_cast<double>(k_amplitude)});
        block0.set(adm::Duration{adm::Time{std::chrono::milliseconds{10}}});
        cf->add(block0);

        adm::AudioBlockFormatObjects block1{adm::SphericalPosition{adm::Azimuth{90.0F}, adm::Elevation{0.0F}}};
        block1.set(adm::Gain{static_cast<double>(k_amplitude)});
        block1.set(adm::Rtime{adm::Time{std::chrono::milliseconds{10}}});
        // JumpPosition defaults to false → linear interpolation ramp
        cf->add(block1);
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

    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_ramp.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "hoa3";
    req.options.renderer = mradm::RendererSelection::hoa;
    req.options.object_smoothing_frames = 0;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: HOA ramp render failed: " << res.error.message << "\n";
        return false;
    }

    const auto raw = read_hoa_raw_samples(out_path);
    if (raw.empty()) {
        std::cerr << "FAIL: cannot read HOA ramp output\n";
        return false;
    }
    // Block0 ends at sample 480; block1 starts there with 5ms ramp (240 samples at 48kHz).
    // Frame 480 (delta=0, alpha=0): output = block0 gains → X active, Y silent.
    // Frame 720 (delta=240=interp_len): output = block1 gains → Y active, X silent.
    constexpr auto k_ch = static_cast<std::size_t>(k_hoa3_channels);
    const float x_ramp_start = raw.at((480U * k_ch) + 3U);
    const float y_ramp_start = raw.at((480U * k_ch) + 1U);
    const float x_after_ramp = raw.at((720U * k_ch) + 3U);
    const float y_after_ramp = raw.at((720U * k_ch) + 1U);
    bool ok = true;
    ok &= check(x_ramp_start > 0.1F, "HOA ramp: X (front) active at ramp start (prev-block contribution)");
    ok &= check(y_ramp_start < 0.01F, "HOA ramp: Y (left) silent at ramp start");
    ok &= check(x_after_ramp < 0.01F, "HOA ramp: X (front) silent after ramp completes");
    ok &= check(y_after_ramp > 0.3F, "HOA ramp: Y (left) active after ramp completes");
    return ok;
}

// positionOffset azimuth=+90° rotates a front source (az=0) to left (az=90).
// HOA output must match verify_left_source(): Y (ch1) active, X (ch3) ≈ 0.
bool verify_hoa_position_offset() {
    auto [doc, uid_str] = make_objects_doc(0.0F, 0.0F);
    for (const auto& ao : doc->getElements<adm::AudioObject>()) {
        adm::SphericalPositionOffset spo;
        spo.set(adm::AzimuthOffset{90.0F});
        ao->set(adm::PositionOffset{spo});
    }
    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_posoffset.wav";
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
        std::cerr << "FAIL: HOA positionOffset render failed: " << res.error.message << "\n";
        return false;
    }
    const auto rms = read_channel_rms(out_path);
    // az = 0 + 90 = 90 (left): Y (ch1) ≈ gain, X (ch3) ≈ 0.
    bool ok = true;
    ok &= check(rms[1] > 0.3, "HOA positionOffset: Y (ACN 1) active (source rotated to left)");
    ok &= check(rms[3] < 0.05, "HOA positionOffset: X (ACN 3) ≈ 0 (rotated away from front)");
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

bool verify_hoa_diffuse_cloud() {
    constexpr std::size_t k_diffuse_frames = 4096U;
    const auto samples = make_noise_samples(k_diffuse_frames);

    auto [direct_doc, direct_uid] = make_objects_doc(0.0F, 0.0F, 0.0F);
    const auto direct_in = write_fixture_samples(direct_doc, direct_uid, "diffuse_direct_in", samples);
    FileGuard direct_in_guard{direct_in};
    const auto direct_out = std::filesystem::temp_directory_path() / "mr_hoa_enc_diffuse_direct_out.wav";
    FileGuard direct_out_guard{direct_out};

    auto [diffuse_doc, diffuse_uid] = make_objects_doc(0.0F, 0.0F, 1.0F);
    const auto diffuse_in = write_fixture_samples(diffuse_doc, diffuse_uid, "diffuse_cloud_in", samples);
    FileGuard diffuse_in_guard{diffuse_in};
    const auto diffuse_out = std::filesystem::temp_directory_path() / "mr_hoa_enc_diffuse_cloud_out.wav";
    FileGuard diffuse_out_guard{diffuse_out};

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;

    mradm::RenderRequest direct_req;
    direct_req.input_path = direct_in;
    direct_req.output_path = direct_out;
    direct_req.options.output_layout = "hoa3";
    direct_req.options.renderer = mradm::RendererSelection::hoa;
    direct_req.options.object_smoothing_frames = 0;
    direct_req.options.peak_limit = false;

    auto res = service.render(direct_req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: HOA diffuse direct baseline failed: " << res.error.message << "\n";
        return false;
    }

    mradm::RenderRequest diffuse_req = direct_req;
    diffuse_req.input_path = diffuse_in;
    diffuse_req.output_path = diffuse_out;
    res = service.render(diffuse_req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: HOA diffuse cloud render failed: " << res.error.message << "\n";
        return false;
    }

    const auto direct_rms = read_channel_rms(direct_out);
    const auto diffuse_rms = read_channel_rms(diffuse_out);
    bool ok = true;
    ok &= check(direct_rms[1] < 1e-5, "HOA diffuse baseline: front source has no Y component");
    ok &= check(direct_rms[2] < 1e-5, "HOA diffuse baseline: front source has no Z component");
    ok &= check(diffuse_rms[0] > 0.01, "HOA diffuse=1: W component is not silent");
    ok &= check(diffuse_rms[1] > 0.01, "HOA diffuse=1: diffuse cloud activates Y component");
    ok &= check(diffuse_rms[2] > 0.01, "HOA diffuse=1: diffuse cloud activates Z component");
    ok &= check(diffuse_rms[3] < direct_rms[3] * 0.9, "HOA diffuse=1: direct front X component is attenuated");
    return ok;
}

bool verify_hoa_diffuse_smoothing_path() {
    constexpr std::size_t k_diffuse_frames = 4096U;
    const auto samples = make_noise_samples(k_diffuse_frames);
    auto [doc, uid] = make_two_block_diffuse_doc();
    const auto in_path = write_fixture_samples(doc, uid, "diffuse_smoothing_in", samples);
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_diffuse_smoothing_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "hoa3";
    req.options.renderer = mradm::RendererSelection::hoa;
    req.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: HOA diffuse smoothing render failed: " << res.error.message << "\n";
        return false;
    }

    const auto raw = read_hoa_raw_samples(out_path);
    if (raw.empty()) {
        std::cerr << "FAIL: cannot read HOA diffuse smoothing output\n";
        return false;
    }

    constexpr std::size_t k_window = 256U;
    const double x_start = channel_rms_range(raw, 3U, 0U, k_window);
    const double x_end = channel_rms_range(raw, 3U, k_diffuse_frames - k_window, k_window);
    const double y_end = channel_rms_range(raw, 1U, k_diffuse_frames - k_window, k_window);
    const double z_end = channel_rms_range(raw, 2U, k_diffuse_frames - k_window, k_window);

    bool ok = true;
    ok &= check(x_start > 0.05, "HOA diffuse smoothing: direct X starts active");
    ok &= check(x_end < x_start * 0.8, "HOA diffuse smoothing: direct X is attenuated by end of block");
    ok &= check(y_end > 0.005, "HOA diffuse smoothing: cloud Y appears by end of block");
    ok &= check(z_end > 0.005, "HOA diffuse smoothing: cloud Z appears by end of block");
    return ok;
}

std::vector<double> render_object_rms(
    float width, float height, float depth, float divergence, float divergence_range, const std::string& suffix) {
    auto [doc, uid] = make_objects_doc(0.0F, 0.0F, 0.0F, width, height, depth, divergence, divergence_range);
    const auto in_path = write_fixture(doc, uid);
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / ("mr_hoa_enc_extent_" + suffix + ".wav");
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "hoa3";
    req.options.renderer = mradm::RendererSelection::hoa;
    req.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: HOA extent render failed: " << res.error.message << "\n";
        return {};
    }
    return read_channel_rms(out_path);
}

std::vector<double> render_object_rms(float width, float height, float depth, const std::string& suffix) {
    return render_object_rms(width, height, depth, 0.0F, 45.0F, suffix);
}

bool verify_hoa_extent_spread() {
    const auto point = render_object_rms(0.0F, 0.0F, 0.0F, "point");
    const auto width = render_object_rms(1.0F, 0.0F, 0.0F, "width");
    const auto height = render_object_rms(0.0F, 1.0F, 0.0F, "height");
    const auto depth = render_object_rms(0.0F, 0.0F, 1.0F, "depth");
    if (point.empty() || width.empty() || height.empty() || depth.empty()) {
        return false;
    }

    bool ok = true;
    ok &= check(point[0] > 0.4 && point[0] < 0.6, "HOA extent baseline: W remains point-source gain");
    ok &= check(width[0] > 0.4 && width[0] < 0.6, "HOA width: W remains close to point-source gain");
    ok &= check(width[3] < point[3] * 0.9, "HOA width: front X directionality is reduced");
    ok &= check(width[1] < 1e-4, "HOA width: symmetric horizontal spread keeps first-order Y centered");
    ok &= check(height[0] > 0.4 && height[0] < 0.6, "HOA height: W remains close to point-source gain");
    ok &= check(height[3] < point[3] * 0.95, "HOA height: front X directionality is reduced");
    ok &= check(height[2] < 1e-4, "HOA height: symmetric vertical spread keeps first-order Z centered");
    ok &= check(depth[3] < point[3] * 0.98, "HOA depth: isotropic angular broadening reduces front X");
    return ok;
}

bool verify_hoa_object_divergence() {
    const auto point = render_object_rms(0.0F, 0.0F, 0.0F, "divergence_point");
    const auto divergence = render_object_rms(0.0F, 0.0F, 0.0F, 1.0F, 60.0F, "divergence");
    if (point.empty() || divergence.empty()) {
        return false;
    }

    bool ok = true;
    ok &= check(divergence[0] > 0.4 && divergence[0] < 0.6, "HOA objectDivergence: W keeps summed gain");
    ok &= check(divergence[1] < 1e-4, "HOA objectDivergence: symmetric side sources keep Y centered");
    ok &= check(divergence[3] < point[3] * 0.7, "HOA objectDivergence: front X directionality is reduced");
    return ok;
}

// ── DirectSpeakers helpers ────────────────────────────────────────────────────

struct DsDocOptions {
    std::optional<float> azimuth; // absent → no SphericalSpeakerPosition set (has_position=false)
    std::optional<float> elevation;
    std::string label;
};

std::pair<std::shared_ptr<adm::Document>, std::string> make_ds_doc(const DsDocOptions& opts) {
    auto doc = adm::Document::create();

    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"DsCF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    adm::AudioBlockFormatDirectSpeakers block{};
    if (opts.azimuth.has_value()) {
        adm::SphericalSpeakerPosition spos;
        spos.set(adm::Azimuth{*opts.azimuth});
        spos.set(adm::Elevation{opts.elevation.value_or(0.0F)});
        block.set(spos);
    }
    if (!opts.label.empty()) {
        block.add(adm::SpeakerLabel{opts.label});
    }
    cf->add(block);
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

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"DsObj"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"DsContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"DsProg"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    const auto uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());
    return {doc, uid_str};
}

mradm::RenderResult render_ds(const std::filesystem::path& in_path, const std::filesystem::path& out_path) {
    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "hoa3";
    req.options.renderer = mradm::RendererSelection::hoa;
    req.options.peak_limit = false;
    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    return service.render(req, progress, logs);
}

// DirectSpeakers with explicit position (az=0, el=0) encodes as front source:
// W (ACN 0) ≈ 1, X (ACN 3) ≈ 1, Y (ACN 1) ≈ 0.
bool verify_ds_with_position() {
    auto [doc, uid_str] = make_ds_doc({.azimuth = 0.0F, .elevation = 0.0F, .label = "M+000"});
    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_ds_pos.wav";
    FileGuard out_guard{out_path};

    const auto res = render_ds(in_path, out_path);
    if (!res.success()) {
        std::cerr << "FAIL: ds_with_position render failed: " << res.error.message << "\n";
        return false;
    }

    const auto rms = read_channel_rms(out_path);
    bool ok = true;
    ok &= check(rms[0] > 0.9, "DS front: W (ACN 0) ≈ 1");
    ok &= check(rms[1] < 1e-3, "DS front: Y (ACN 1) ≈ 0");
    ok &= check(rms[2] < 1e-3, "DS front: Z (ACN 2) ≈ 0");
    ok &= check(rms[3] > 0.9, "DS front: X (ACN 3) ≈ 1");
    return ok;
}

// DirectSpeakers labelled RC_LFE without a lowPass ChannelFrequency element must
// still be encoded omnidirectionally (W only). Before the label-check fix this
// channel was encoded as a directional source at its nominal az=45, el=-30 position.
bool verify_ds_lfe_label_no_lowpass() {
    auto [doc, uid_str] = make_ds_doc({.azimuth = 45.0F, .elevation = -30.0F, .label = "RC_LFE"});
    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_ds_lfe.wav";
    FileGuard out_guard{out_path};

    const auto res = render_ds(in_path, out_path);
    if (!res.success()) {
        std::cerr << "FAIL: ds_lfe_label render failed: " << res.error.message << "\n";
        return false;
    }

    const auto rms = read_channel_rms(out_path);
    bool ok = true;
    ok &= check(rms[0] > 0.9, "DS RC_LFE: W (ACN 0) ≈ 1 (omnidirectional)");
    // All higher-order channels must be silent — no directional encoding.
    for (int ch = 1; ch < k_hoa3_channels; ++ch) {
        ok &= check(rms[static_cast<std::size_t>(ch)] < 1e-3, "DS RC_LFE: higher-order ACN channel ≈ 0");
    }
    return ok;
}

// LFE-only DirectSpeakers: LUFS must be absent; True Peak must be present.
// Validates the separation between the AllRAD 7.1.4 LUFS path (LFE excluded via
// EBUR128_UNUSED) and the dedicated per-frame LFE True Peak tracker.
bool verify_lfe_metrics_separation() {
    constexpr float k_lfe_amp = 0.8F;
    constexpr uint32_t k_lfe_frames = 48000U; // 1 s at 48 kHz

    // RC_LFE label triggers W-only HOA encoding; position is irrelevant.
    auto [doc, uid_str] = make_ds_doc({.azimuth = 0.0F, .elevation = 0.0F, .label = "RC_LFE"});

    const auto in_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_lfe_sep_in.wav";
    FileGuard in_guard{in_path};
    {
        std::ostringstream xml_buf;
        adm::writeXml(xml_buf, doc);
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
        auto writer = bw64::writeFile(in_path.string(), 1U, 48000U, 24U, chna, axml);
        std::vector<float> samples(k_lfe_frames, k_lfe_amp);
        writer->write(samples.data(), k_lfe_frames);
    }

    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_lfe_sep_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "hoa3";
    req.options.renderer = mradm::RendererSelection::hoa;
    req.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: LFE metrics separation render failed: " << res.error.message << "\n";
        return false;
    }
    if (!res.metrics.has_value()) {
        std::cerr << "FAIL: LFE metrics separation: no metrics in RenderResult\n";
        return false;
    }
    const auto& m = *res.metrics;

    bool ok = true;
    // TP must reflect the LFE signal amplitude (≈ -1 dBTP for 0.8F), NOT the AllRAD
    // W-spread value per spatial speaker (0.8 × AllRAD_W_coeff ≈ 0.24F → ≈ -12 dBTP).
    // A threshold of -6 dBTP lies between these two values and will catch regression if
    // the LFE TP tracker is removed.
    ok &= check(m.measured_peak_dbtp.has_value(), "LFE-only HOA: measured_peak_dbtp present (LFE TP tracker active)");
    if (m.measured_peak_dbtp.has_value()) {
        ok &= check(*m.measured_peak_dbtp > -6.0,
                    "LFE-only HOA: TP > -6 dBTP (LFE tracker at 0.8F; AllRAD-only would give ~-12 dBTP)");
    }
    ok &= check(!m.measured_lufs.has_value(), "LFE-only HOA: measured_lufs absent (LFE excluded from LUFS path)");
    return ok;
}

// DirectSpeakers at az=+30°, el=0° (left-front): verify non-trivial SH coefficients.
// W≈1, X≈cos(30°)≈0.87, Y≈sin(30°)≈0.5, Z≈0.
// Note: libadm always initialises AudioBlockFormatDirectSpeakers with a default
// SphericalSpeakerPosition (boost::variant first alternative), so a block with no
// explicit position always resolves to az=0,el=0 through the normal import path.
// The label-only fallback in build_gain_matrix therefore cannot be exercised via the
// full render pipeline; parse_speaker_label is verified by code inspection.
bool verify_ds_off_axis_position() {
    auto [doc, uid_str] = make_ds_doc({.azimuth = 30.0F, .elevation = 0.0F, .label = "M+030"});
    const auto in_path = write_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_enc_ds_offaxis.wav";
    FileGuard out_guard{out_path};

    const auto res = render_ds(in_path, out_path);
    if (!res.success()) {
        std::cerr << "FAIL: ds_off_axis render failed: " << res.error.message << "\n";
        return false;
    }

    const auto rms = read_channel_rms(out_path);
    bool ok = true;
    ok &= check(rms[0] > 0.9, "DS az=30: W (ACN 0) ≈ 1");
    ok &= check(rms[1] > 0.3 && rms[1] < 0.7, "DS az=30: Y (ACN 1) ≈ sin(30°) ≈ 0.5");
    ok &= check(rms[2] < 1e-3, "DS az=30: Z (ACN 2) ≈ 0");
    ok &= check(rms[3] > 0.7 && rms[3] < 0.95, "DS az=30: X (ACN 3) ≈ cos(30°) ≈ 0.87");
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
    ok &= check(caps.supports_object_divergence, "HOA capabilities: objectDivergence supported");
    ok &= check(caps.supports_diffuse, "HOA capabilities: diffuse supported");

    // ── Fixtures ──────────────────────────────────────────────────────────────
    ok &= verify_front_source();
    ok &= verify_left_source();
    ok &= verify_hoa_mute_writes_silence();
    ok &= verify_hoa_obj_gain();
    ok &= verify_hoa_jump_position();
    ok &= verify_hoa_ramp_interpolation();
    ok &= verify_hoa_position_offset();
    ok &= verify_hoa_diffuse_cloud();
    ok &= verify_hoa_diffuse_smoothing_path();
    ok &= verify_hoa_extent_spread();
    ok &= verify_hoa_object_divergence();
    ok &= verify_ds_with_position();
    ok &= verify_ds_lfe_label_no_lowpass();
    ok &= verify_ds_off_axis_position();
    ok &= verify_lfe_metrics_separation();

    if (ok) {
        std::cout << "hoa encode fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
