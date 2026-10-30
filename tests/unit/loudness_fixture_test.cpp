#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include "adm/audio_io.h"
#include "adm/logging.h"
#include "adm/render.h"

#include "../support/adm_fixture.h"
#include "../support/wav_fixture.h"
#include "meter.h"

namespace {

class NullLog final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel /*level*/, std::string_view /*module*/, std::string_view /*message*/) override {}
};

bool fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return false;
}

int fail_exit(const std::string& msg) {
    fail(msg);
    return EXIT_FAILURE;
}

// Writes a mono 48 kHz ADM Objects WAV (front centre) with a 440 Hz sine.
void write_objects_sine_wav(float amplitude, double duration_s, const std::filesystem::path& path) {
    constexpr uint32_t k_sr = 48000U;
    const auto k_frames = static_cast<uint32_t>(duration_s * k_sr);
    constexpr float k_freq = 440.0F;

    auto doc = fixture::Document::create();
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"LoudCF"},
                                                  fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);
    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"LoudPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);
    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"LoudSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);
    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"LoudTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);
    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{"LoudObj"});
    obj->add_reference(uid);
    doc->add(obj);
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"LoudContent"});
    content->add_reference(obj);
    doc->add(content);
    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"LoudProgramme"});
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

// Renders the input to 5.0 with loudness normalisation to -23 LUFS (True Peak limiter off).
mradm::RenderResult render_normalised(const std::filesystem::path& in_path, const std::filesystem::path& out_path) {
    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::saf;
    req.options.output_layout = "0+5+0";
    req.options.measure_loudness = true;
    req.options.loudness_target_lufs = -23.0F;
    req.options.peak_limit = false;
    mradm::RenderService service;
    mradm::NullProgressSink progress;
    NullLog logs;
    return service.render(req, progress, logs);
}

// Returns LUFS, or NaN on failure.
double measure_lufs(const std::string& path) {
    auto reader_res = mradm::audio::FloatWavReader::open(path);
    if (!reader_res) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    auto& reader = *reader_res;

    auto st = mradm::dsp::Meter::create(reader.channels(), reader.sample_rate(), mradm::dsp::MeterMode::integrated);
    if (!st) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    constexpr std::size_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(reader.channels()) * k_block);
    uint64_t left = reader.frame_count();
    while (left > 0) {
        const uint64_t n = std::min(static_cast<uint64_t>(k_block), left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        if (!st->add_frames(buf.data(), static_cast<std::size_t>(got))) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        left -= got;
    }

    const auto loudness = st->integrated();
    return loudness.value_or(std::numeric_limits<double>::quiet_NaN());
}

} // namespace

int main() try {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto loud_input = tmp / "loudness_norm_input.wav";
    const auto norm_path = tmp / "loudness_norm_test.wav";
    const auto silence_input = tmp / "loudness_silence_input.wav";
    const auto silence_output = tmp / "loudness_silence_test.wav";

    // ── Test 1: normalise loud signal to -23 LUFS within 1.5 LU ─────────────
    // 440 Hz sine at -6 dBFS for 5 seconds — safely above gating threshold.
    write_objects_sine_wav(0.5F, 5.0, loud_input);
    const auto res = render_normalised(loud_input, norm_path);
    std::filesystem::remove(loud_input);
    if (!res.success()) {
        std::filesystem::remove(norm_path);
        return fail_exit("loudness-normalised render returned error: " + res.error.message);
    }

    const double after = measure_lufs(norm_path.string());
    std::filesystem::remove(norm_path);

    if (!std::isfinite(after)) {
        return fail_exit("failed to measure integrated loudness after normalization");
    }
    const double error_lu = std::abs(after - (-23.0));
    if (error_lu >= 1.5) {
        return fail_exit("loudness error too large: |" + std::to_string(after) +
                         " - (-23.0)| = " + std::to_string(error_lu) + " LU (limit 1.5)");
    }

    // ── Test 2: silence is a no-op (no error) ────────────────────────────────
    write_objects_sine_wav(0.0F, 3.0, silence_input);
    {
        const auto silent = render_normalised(silence_input, silence_output);
        std::filesystem::remove(silence_input);
        std::filesystem::remove(silence_output);
        if (!silent.success()) {
            return fail_exit("loudness-normalised render of silence returned error: " + silent.error.message);
        }
    }

    // ── Test 3: missing input is an I/O error, not a silent no-op ────────────
    {
        const auto missing_path = tmp / "loudness_missing_test.wav";
        std::filesystem::remove(missing_path);
        const auto missing = render_normalised(missing_path, tmp / "loudness_missing_output.wav");
        if (missing.success()) {
            return fail_exit("loudness-normalised render of a missing file unexpectedly succeeded");
        }
        if (missing.error.code != mradm::ErrorCode::io_error) {
            return fail_exit("loudness-normalised render of a missing file returned non-io_error");
        }
    }

    std::cout << "loudness fixture test passed (after=" << after << " LUFS, error=" << error_lu << " LU)\n";
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << "fixture setup failed: " << error.what() << '\n';
    return EXIT_FAILURE;
}
