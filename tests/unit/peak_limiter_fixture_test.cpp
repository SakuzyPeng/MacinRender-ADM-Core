#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "adm/audio_io.h"
#include "adm/logging.h"
#include "adm/peak.h"
#include "adm/render.h"

#include "../support/adm_fixture.h"
#include "../support/wav_fixture.h"

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

// Write a 1ch 48kHz WAV with a 440 Hz sine wave at the given amplitude.
// 440 Hz is well below Nyquist, so True Peak ≈ sample peak (negligible inter-sample overshoot).
void write_sine_wav(float amplitude, const std::filesystem::path& path) {
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = k_sr; // 1 second
    constexpr float k_freq = 440.0F;

    auto chna = std::make_shared<fixture::WaveChna>(std::vector<fixture::WaveAudioId>{});
    auto axml = std::make_shared<fixture::WaveAxml>("");
    auto writer = fixture::write_wave(path.string(), 1U, k_sr, 24U, chna, axml);

    std::vector<float> samples(k_frames);
    for (uint32_t n = 0; n < k_frames; ++n) {
        samples[n] = amplitude * std::sin(2.0F * std::numbers::pi_v<float> * k_freq * static_cast<float>(n) /
                                          static_cast<float>(k_sr));
    }
    writer->write(samples.data(), static_cast<uint64_t>(k_frames));
}

void write_objects_sine_wav(float amplitude, const std::filesystem::path& path) {
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = k_sr; // 1 second
    constexpr float k_freq = 440.0F;

    auto doc = fixture::Document::create();
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"PeakCF"},
                                                  fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);
    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"PeakPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);
    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"PeakSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);
    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"PeakTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);
    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{"PeakObj"});
    obj->add_reference(uid);
    doc->add(obj);
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"PeakContent"});
    content->add_reference(obj);
    doc->add(content);
    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"PeakProgramme"});
    programme->add_reference(content);
    doc->add(programme);
    fixture::reassign_ids(doc);

    std::ostringstream xml_buf;
    fixture::write_xml(xml_buf, doc);
    const std::string uid_str = fixture::format_id(uid->get<fixture::AudioTrackUidId>());
    auto chna = std::make_shared<fixture::WaveChna>(
        std::vector<fixture::WaveAudioId>{fixture::WaveAudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<fixture::WaveAxml>(xml_buf.str());
    auto writer = fixture::write_wave(path.string(), 1U, k_sr, 24U, chna, axml);

    std::vector<float> samples(k_frames);
    for (uint32_t n = 0; n < k_frames; ++n) {
        samples[n] = amplitude * std::sin(2.0F * std::numbers::pi_v<float> * k_freq * static_cast<float>(n) /
                                          static_cast<float>(k_sr));
    }
    writer->write(samples.data(), static_cast<uint64_t>(k_frames));
}

double max_abs_sample(const std::filesystem::path& path) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        return 0.0;
    }
    auto& reader = *reader_res;
    const auto n_ch = reader.channels();
    constexpr std::size_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(n_ch) * k_block);
    uint64_t left = reader.frame_count();
    double max_val = 0.0;
    while (left > 0) {
        const uint64_t n = std::min(static_cast<uint64_t>(k_block), left);
        reader.read(buf.data(), n);
        const auto samples = static_cast<std::size_t>(n_ch) * static_cast<std::size_t>(n);
        max_val = std::accumulate(
            buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(samples), max_val, [](double current, float sample) {
                return std::max(current, std::abs(static_cast<double>(sample)));
            });
        left -= n;
    }
    return max_val;
}

// Over-limit: 440 Hz sine at 0.95 amplitude → True Peak > -1.0 dBTP → limiter applies.
// After limiting the sample peak must be ≤ target and not have collapsed.
bool verify_limiter_active() {
    const auto path = std::filesystem::temp_directory_path() / "mr_peak_sine_high.wav";
    FileGuard guard{path};
    write_sine_wav(0.95F, path);

    mradm::NullLogSink logs;
    const auto res = mradm::apply_peak_limit(path.string(), -1.0F, logs);
    if (!res) {
        std::cerr << "FAIL [active]: " << res.error().message << "\n";
        return false;
    }

    const double max_sample = max_abs_sample(path);
    const double target_linear = std::pow(10.0, -1.0 / 20.0); // ≈ 0.8913

    bool ok = true;
    ok &= check(max_sample <= target_linear * 1.001, "[active] peak ≤ -1.0 dBTP after limiting");
    // For a sine wave True Peak ≈ sample peak, so gain ≈ target/0.95 ≈ 0.94 → output ≈ 0.89.
    ok &= check(max_sample > target_linear * 0.9, "[active] peak is near target (not collapsed)");
    return ok;
}

// No-op: 440 Hz sine at 0.10 amplitude → True Peak well below -1.0 dBTP → no change.
bool verify_limiter_noop() {
    const auto path = std::filesystem::temp_directory_path() / "mr_peak_sine_low.wav";
    FileGuard guard{path};
    write_sine_wav(0.10F, path);

    mradm::NullLogSink logs;
    const auto res = mradm::apply_peak_limit(path.string(), -1.0F, logs);
    if (!res) {
        std::cerr << "FAIL [noop]: " << res.error().message << "\n";
        return false;
    }

    const double max_sample = max_abs_sample(path);
    // No gain was applied; sample peak should remain near 0.10.
    return check(max_sample > 0.08 && max_sample < 0.12, "[noop] amplitude unchanged");
}

bool verify_render_service_peak_normalize_to_limit() {
    const auto in_path = std::filesystem::temp_directory_path() / "mr_peak_norm_input.wav";
    const auto out_path = std::filesystem::temp_directory_path() / "mr_peak_norm_output.wav";
    FileGuard in_guard{in_path};
    FileGuard out_guard{out_path};
    write_objects_sine_wav(0.01F, in_path);

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::saf;
    req.options.output_layout = "0+5+0";
    req.options.peak_limit = true;
    req.options.peak_limit_dbtp = -6.0F;
    req.options.peak_normalize_to_limit = true;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL [normalize]: " << res.error.message << "\n";
        return false;
    }

    const double max_sample = max_abs_sample(out_path);
    const double target_linear = std::pow(10.0, -6.0 / 20.0); // about 0.5012

    bool ok = true;
    ok &= check(max_sample <= target_linear * 1.02, "[normalize] peak stays below red line");
    ok &= check(max_sample > target_linear * 0.85, "[normalize] peak is raised near target");
    return ok;
}

bool verify_peak_normalize_requires_peak_limit() {
    const auto in_path = std::filesystem::temp_directory_path() / "mr_peak_norm_requires_limit_input.wav";
    const auto out_path = std::filesystem::temp_directory_path() / "mr_peak_norm_requires_limit_output.wav";
    FileGuard in_guard{in_path};
    FileGuard out_guard{out_path};
    write_objects_sine_wav(0.01F, in_path);

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::saf;
    req.options.output_layout = "0+5+0";
    req.options.peak_limit = false;
    req.options.peak_normalize_to_limit = true;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);

    bool ok = true;
    ok &= check(!res.success(), "[requires-limit] render rejected");
    ok &= check(res.error.code == mradm::ErrorCode::invalid_argument, "[requires-limit] invalid_argument error");
    return ok;
}

} // namespace

int main() try {
    bool ok = true;
    ok &= verify_limiter_active();
    ok &= verify_limiter_noop();
    ok &= verify_render_service_peak_normalize_to_limit();
    ok &= verify_peak_normalize_requires_peak_limit();

    if (ok) {
        std::cout << "peak limiter fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
} catch (const std::exception& error) {
    std::cerr << "fixture setup failed: " << error.what() << '\n';
    return 1;
}
