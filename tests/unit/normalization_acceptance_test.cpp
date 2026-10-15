// PCM consistency 2.4: final PCM after offline gain staging. Each case renders an ADM fixture
// through RenderService, re-measures the written file with the production meter (whose accuracy
// against an independent BS.1770-4 reference is checked in mradm-dsp's tests/meter_acceptance.rs)
// and requires the target to be met and the reported metrics to describe the file.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/audio_io.h"
#include "adm/render.h"

#include "../support/adm_fixture.h"
#include "../support/wav_fixture.h"
#include "meter.h"

namespace {

constexpr uint32_t k_sr = 48000U;
constexpr uint32_t k_frames = k_sr * 6U;
constexpr uint32_t k_burst_start = k_sr * 3U;
constexpr uint32_t k_burst_end = k_burst_start + (k_sr / 2U);

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

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
    }
    return condition;
}

std::filesystem::path temp_path(std::string_view stem, std::string_view ext) {
    static std::atomic<int> s_seq{0};
    const auto name = std::string(stem) + "_" + std::to_string(s_seq.fetch_add(1)) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + std::string(ext);
    return std::filesystem::temp_directory_path() / name;
}

std::pair<std::shared_ptr<fixture::Document>, std::string> make_doc() {
    auto doc = fixture::Document::create();
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"NormCF"},
                                                  fixture::TypeDefinition::objects);
    fixture::AudioBlockFormatObjects block{
        fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
    block.set(fixture::Gain{1.0F});
    cf->add(block);
    doc->add(cf);
    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"NormPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);
    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"NormSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);
    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"NormTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);
    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{"NormObject"});
    obj->add_reference(uid);
    doc->add(obj);
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"NormContent"});
    content->add_reference(obj);
    doc->add(content);
    auto prog = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"NormProgramme"});
    prog->add_reference(content);
    doc->add(prog);
    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

// A 1 kHz sine with a half-second quarter-rate burst sampled 45 degrees off its crest, so the
// true peak lies between samples. `scale` sets the burst amplitude; silence when zero.
std::filesystem::path write_input(float scale) {
    const auto [doc, uid] = make_doc();
    auto path = temp_path("mr_norm_input", ".wav");
    std::ostringstream xml;
    fixture::write_xml(xml, doc);
    auto chna =
        std::make_shared<fixture::WaveChna>(std::vector<fixture::WaveAudioId>{fixture::WaveAudioId(1U, uid, "", "")});
    auto axml = std::make_shared<fixture::WaveAxml>(xml.str());
    auto writer = fixture::write_wave(path.string(), 1U, k_sr, 32U, chna, axml);
    std::vector<float> samples;
    samples.reserve(k_frames);
    for (uint32_t n = 0; n < k_frames; ++n) {
        const double t = static_cast<double>(n) / k_sr;
        double value = 0.25 * std::sin(2.0 * std::numbers::pi * 1000.0 * t);
        if (n >= k_burst_start && n < k_burst_end) {
            value = std::sin((std::numbers::pi / 2.0 * static_cast<double>(n % 4U)) + (std::numbers::pi / 4.0));
        }
        samples.push_back(static_cast<float>(scale * value));
    }
    writer->write(samples.data(), k_frames);
    return path;
}

struct Measured {
    double lufs = 0.0;
    double peak_dbtp = 0.0;
    bool silent = true;
};

std::optional<Measured> measure(const std::filesystem::path& path) {
    auto reader = mradm::audio::FloatWavReader::open(path.string());
    if (!reader) {
        return std::nullopt;
    }
    std::vector<float> samples(static_cast<std::size_t>(reader->frame_count()) * reader->channels());
    if (reader->read(samples.data(), reader->frame_count()) != reader->frame_count()) {
        return std::nullopt;
    }
    auto meter = mradm::dsp::Meter::create(
        reader->channels(), reader->sample_rate(), mradm::dsp::MeterMode::integrated_true_peak);
    if (!meter || !meter->add_frames(samples.data(), reader->frame_count())) {
        return std::nullopt;
    }
    const auto lufs = meter->integrated();
    const auto peak = meter->max_true_peak();
    if (!lufs || !peak) {
        return std::nullopt;
    }
    Measured out{*lufs, 20.0 * std::log10(*peak), true};
    for (const float sample : samples) {
        out.silent = out.silent && sample == 0.0F;
    }
    return out;
}

struct Case {
    const char* name = "";
    float scale = 0.0F;
    mradm::RenderOptions options{};
    std::optional<double> lufs = std::nullopt;      // expected final integrated loudness
    std::optional<double> peak_dbtp = std::nullopt; // expected final true peak
};

mradm::RenderOptions base_options() {
    mradm::RenderOptions options;
    options.renderer = mradm::RendererSelection::ear;
    options.output_layout = "0+5+0";
    options.peak_limit = false;
    return options;
}

bool run_case(const Case& c) {
    const auto input = write_input(c.scale);
    const FileGuard input_guard(input);
    const auto output = temp_path("mr_norm_output", ".wav");
    const FileGuard output_guard(output);
    mradm::RenderRequest request;
    request.input_path = input;
    request.output_path = output;
    request.options = c.options;
    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto result = service.render(request, progress, logs);
    const std::string name(c.name);
    if (!check(result.success(), name + ": render succeeds")) {
        return false;
    }
    const auto measured = measure(output);
    if (!measured) {
        return check(false, name + ": final PCM can be measured");
    }
    const Measured& final_pcm = *measured;
    std::cout << std::setprecision(10) << name << ": final " << final_pcm.lufs << " LUFS, " << final_pcm.peak_dbtp
              << " dBTP\n";
    if (!c.lufs && !c.peak_dbtp) {
        return check(final_pcm.silent, name + ": silence stays digital silence") &&
               check(!result.metrics || (!result.metrics->measured_lufs && !result.metrics->measured_peak_dbtp),
                     name + ": silence reports no loudness or peak");
    }
    if (!result.metrics) {
        return check(false, name + ": metrics are reported");
    }
    const mradm::RenderMetrics& metrics = *result.metrics;
    // The f32 gain, float output and 24-bit quantization move the re-measured values by about
    // 1e-6 LU / dB on this programme.
    constexpr double k_tolerance = 1e-5;
    bool ok = true;
    if (c.lufs) {
        ok &= check(std::abs(final_pcm.lufs - *c.lufs) < k_tolerance, name + ": final loudness meets the target");
    }
    if (c.peak_dbtp) {
        ok &= check(std::abs(final_pcm.peak_dbtp - *c.peak_dbtp) < k_tolerance,
                    name + ": final true peak meets the target");
    }
    if (metrics.measured_lufs) {
        ok &= check(std::abs(*metrics.measured_lufs - final_pcm.lufs) < k_tolerance,
                    name + ": reported loudness describes the final PCM");
    }
    if (metrics.measured_peak_dbtp) {
        ok &= check(std::abs(*metrics.measured_peak_dbtp - final_pcm.peak_dbtp) < k_tolerance,
                    name + ": reported true peak describes the final PCM");
    }
    return ok;
}

} // namespace

int main() try {
    std::vector<Case> cases;
    {
        auto options = base_options();
        options.measure_loudness = true;
        options.loudness_target_lufs = -23.0F;
        cases.push_back({"loudness -23", 0.5F, options, -23.0, std::nullopt});
        options.final_gain_db = -2.5;
        cases.push_back({"loudness -23, final gain -2.5 dB", 0.5F, options, -25.5, std::nullopt});
    }
    {
        auto options = base_options();
        options.peak_limit = true;
        options.peak_normalize_to_limit = true;
        options.peak_limit_dbtp = -6.0F;
        cases.push_back({"peak makeup to -6 dBTP", 0.25F, options, std::nullopt, -6.0});
        options.output_bit_depth = mradm::OutputBitDepth::i24;
        cases.push_back({"peak makeup to -6 dBTP, 24-bit", 0.25F, options, std::nullopt, -6.0});
    }
    {
        auto options = base_options();
        options.peak_limit = true;
        cases.push_back({"default -1 dBTP ceiling on a hot input", 1.0F, options, std::nullopt, -1.0});
    }
    {
        auto options = base_options();
        options.measure_loudness = true;
        options.peak_limit = true;
        cases.push_back({"silence", 0.0F, options, std::nullopt, std::nullopt});
    }
    bool ok = true;
    for (const auto& c : cases) {
        ok &= run_case(c);
    }
    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "normalization acceptance tests passed\n";
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << "fixture setup failed: " << error.what() << '\n';
    return EXIT_FAILURE;
}
