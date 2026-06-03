#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_apple.h"

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

std::filesystem::path temp_path(std::string_view stem, std::string_view ext) {
    static std::atomic<int> s_seq{0};
    const auto name = std::string(stem) + "_" + std::to_string(static_cast<int>(::getpid())) + "_" +
                      std::to_string(s_seq.fetch_add(1)) + std::string(ext);
    return std::filesystem::temp_directory_path() / name;
}

// Single OBJECTS object at a fixed azimuth (ADM convention: +ve = left) and linear gain.
std::pair<std::shared_ptr<adm::Document>, std::string> make_object_doc(float azimuth, float gain) {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"AppleCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{azimuth}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{gain});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"ApplePF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"AppleSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"AppleTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"AppleObject"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"AppleContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"AppleProgramme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::filesystem::path write_fixture(float azimuth, uint32_t frames, float gain) {
    constexpr uint32_t k_ch = 1U;
    constexpr uint32_t k_sr = 48000U;
    const auto [doc, uid_str] = make_object_doc(azimuth, gain);
    auto path = temp_path("mr_apple_input", ".wav");

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
    auto writer = bw64::writeFile(path.string(), k_ch, k_sr, 24U, chna, axml);
    std::vector<float> samples(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        samples[i] = 0.25F * std::sin(2.0F * std::numbers::pi_v<float> * 440.0F * static_cast<float>(i) /
                                      static_cast<float>(k_sr));
    }
    writer->write(samples.data(), frames);
    return path;
}

double channel_energy(const std::vector<float>& samples, uint32_t channels, uint32_t ch) {
    double e = 0.0;
    const std::size_t frames = samples.size() / channels;
    for (std::size_t f = 0; f < frames; ++f) {
        const double v = samples[(f * channels) + ch];
        e += v * v;
    }
    return e;
}

std::optional<std::vector<float>> render_apple_stereo(float azimuth, std::string_view stem, float gain = 1.0F) {
    const auto in = write_fixture(azimuth, 8192U, gain);
    FileGuard in_guard(in);
    const auto out = temp_path(stem, ".wav");
    FileGuard out_guard(out);

    mradm::RenderRequest req;
    req.input_path = in;
    req.output_path = out;
    req.options.renderer = mradm::RendererSelection::apple;
    req.options.output_layout = "binaural";
    req.options.peak_limit = false;
    req.options.measure_loudness = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!check(res.success(), "apple render succeeds")) {
        std::cerr << "context: " << res.error.message << " " << res.error.context << "\n";
        return std::nullopt;
    }

    auto reader = mradm::audio::FloatWavReader::open(out.string());
    if (!check(reader.has_value(), "apple output WAV opens")) {
        return std::nullopt;
    }
    if (!check(reader->channels() == 2U, "apple output is stereo")) {
        return std::nullopt;
    }
    std::vector<float> samples(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());
    return samples;
}

bool verify_capabilities() {
    const auto caps = mradm::apple_capabilities();
    bool ok = true;
    ok &= check(caps.backend_name == "apple", "backend name");
    ok &= check(caps.supports_objects, "objects supported");
    ok &= check(caps.supports_direct_speakers, "direct speakers supported");
    ok &= check(!caps.supports_hoa, "hoa unsupported");
    ok &= check(caps.supports_object_divergence, "object divergence advertised");
    ok &= check(!caps.supports_diffuse, "diffuse unsupported");
    ok &= check(!caps.supports_render_window, "render window unsupported");
    const auto it = std::ranges::find_if(caps.supported_layouts,
                                         [](const mradm::CapabilityReport::Layout& l) { return l.id == "binaural"; });
    ok &= check(it != caps.supported_layouts.end() && it->channel_count == 2U && it->is_binaural,
                "binaural layout advertised");
    return ok;
}

// ADM azimuth +ve = LEFT must end up louder in the LEFT output channel and vice versa.
// This is the end-to-end guard for the ADM->SpatialMixer azimuth sign flip.
bool verify_directional_sign() {
    const auto left = render_apple_stereo(90.0F, "mr_apple_left");
    const auto right = render_apple_stereo(-90.0F, "mr_apple_right");
    if (!left || !right) {
        return false;
    }
    const double left_l = channel_energy(*left, 2U, 0U);
    const double left_r = channel_energy(*left, 2U, 1U);
    const double right_l = channel_energy(*right, 2U, 0U);
    const double right_r = channel_energy(*right, 2U, 1U);

    bool ok = true;
    ok &= check(left_l > left_r, "ADM azimuth +90 (left) is louder in left output channel");
    ok &= check(right_r > right_l, "ADM azimuth -90 (right) is louder in right output channel");
    return ok;
}

// kSpatialMixerParam_Gain is in dB; a linear gain of 0 must map to the -120 dB floor
// (mute), not 0 dB (unity). A gain=0 object must therefore render as (near) silence.
bool verify_zero_gain_is_silent() {
    const auto out = render_apple_stereo(0.0F, "mr_apple_silence", 0.0F);
    if (!out) {
        return false;
    }
    const double energy = channel_energy(*out, 2U, 0U) + channel_energy(*out, 2U, 1U);
    return check(energy < 1.0e-2, "gain=0 object renders as silence (dB floor, not 0 dB unity)");
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_capabilities();
    ok &= verify_directional_sign();
    ok &= verify_zero_gain_is_silent();
    return ok ? 0 : 1;
}
