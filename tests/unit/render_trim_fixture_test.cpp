#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm/audio_io.h"
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

std::filesystem::path temp_path(std::string_view stem, std::string_view ext) {
    static std::atomic<int> s_seq{0};
    const auto name = std::string(stem) + "_" + std::to_string(static_cast<int>(::getpid())) + "_" +
                      std::to_string(s_seq.fetch_add(1)) + std::string(ext);
    return std::filesystem::temp_directory_path() / name;
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_render_doc() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"TrimCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        cf->add(block);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"TrimPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"TrimSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TrimTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"TrimObject"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"TrimContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"TrimProgramme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

constexpr uint32_t k_sr = 48000U;
constexpr uint32_t k_frames = k_sr * 2U; // 2 seconds

std::filesystem::path write_render_fixture() {
    const auto [doc, uid_str] = make_render_doc();
    auto path = temp_path("mr_trim_engine_input", ".wav");

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    auto writer = bw64::writeFile(path.string(), 1U, k_sr, 24U, chna, axml);
    // Silent first second, active second second. The render pipeline is causal
    // (decorrelator FIR + direct delay), so the active half cannot leak backwards:
    // the first half stays exactly silent, letting a window-aware meter tell the
    // halves apart (silent half has no peak, active half does).
    std::vector<float> samples(k_frames, 0.0F);
    for (uint64_t f = k_frames / 2U; f < k_frames; ++f) {
        samples[f] = 0.5F;
    }
    writer->write(samples.data(), k_frames);
    return path;
}

// A diffuse, moving object so rendering exercises stateful paths the windowed
// pre-roll must warm up (direct compensation delay / decorrelator overlap) and
// block-edge smoothing must keep aligned with the full-render block grid.
std::pair<std::shared_ptr<adm::Document>, std::string> make_diffuse_doc() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"DiffCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block0{adm::SphericalPosition{adm::Azimuth{-30.0F}, adm::Elevation{0.0F}}};
        block0.set(adm::Rtime{adm::Time{std::chrono::milliseconds{0}}});
        block0.set(adm::Duration{adm::Time{std::chrono::milliseconds{520}}});
        block0.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        block0.set(adm::Gain{1.0F});
        block0.set(adm::Diffuse{0.5F}); // split energy across direct + decorrelated diffuse bus
        cf->add(block0);

        adm::AudioBlockFormatObjects block1{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}}};
        block1.set(adm::Rtime{adm::Time{std::chrono::milliseconds{520}}});
        block1.set(adm::JumpPosition{adm::JumpPositionFlag{false}});
        block1.set(adm::Gain{1.0F});
        block1.set(adm::Diffuse{0.5F});
        cf->add(block1);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"DiffPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"DiffSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"DiffTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"DiffObject"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"DiffContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"DiffProgramme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// 2 s of a per-sample-varying signal (so any pre-roll/delay error is visible) carried
// by a diffuse object.
std::filesystem::path write_varying_fixture() {
    const auto [doc, uid_str] = make_diffuse_doc();
    auto path = temp_path("mr_trim_vary_input", ".wav");
    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
    auto writer = bw64::writeFile(path.string(), 1U, k_sr, 24U, chna, axml);
    std::vector<float> samples(k_frames);
    for (uint64_t f = 0; f < k_frames; ++f) {
        samples[f] = 0.5F * std::sin(static_cast<float>(f) * 0.013F);
    }
    writer->write(samples.data(), k_frames);
    return path;
}

// Render the 2-second fixture to a WAV with the given trim. Defaults to the ear
// backend at 5.1; the renderer/layout are overridable for backend-specific checks.
mradm::RenderResult render_with_trim(const std::filesystem::path& in_path,
                                     const std::filesystem::path& out_path,
                                     double start_sec,
                                     std::optional<double> end_sec,
                                     mradm::RendererSelection renderer = mradm::RendererSelection::ear,
                                     const std::string& layout = "0+5+0") {
    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = renderer;
    req.options.output_layout = layout;
    req.options.peak_limit = false;
    req.options.measure_loudness = false;
    req.options.render_start_sec = start_sec;
    req.options.render_end_sec = end_sec;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    return service.render(req, progress, logs);
}

bool wav_frame_count(const std::filesystem::path& path, uint64_t& out_frames) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        return false;
    }
    out_frames = reader_res->frame_count();
    return true;
}

// --end clips the tail: a 2 s input rendered with --end 1.0 yields ~1 s.
bool verify_end_trim() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_end_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 0.0, 1.0);
    if (!check(res.success(), "render with --end 1.0 succeeds")) {
        return false;
    }
    uint64_t frames = 0;
    if (!check(wav_frame_count(out_path, frames), "open --end output")) {
        return false;
    }
    return check(frames == k_sr, "--end 1.0 produces 1 second of output");
}

// --start and --end together select a 1 s window from the middle.
bool verify_start_end_window() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_win_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 0.5, 1.5);
    if (!check(res.success(), "render with --start 0.5 --end 1.5 succeeds")) {
        return false;
    }
    uint64_t frames = 0;
    if (!check(wav_frame_count(out_path, frames), "open window output")) {
        return false;
    }
    return check(frames == k_sr, "[0.5s, 1.5s) window produces 1 second of output");
}

// No trim options: output keeps the full 2 s duration.
bool verify_no_trim_full_duration() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_full_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 0.0, std::nullopt);
    if (!check(res.success(), "render without trim succeeds")) {
        return false;
    }
    uint64_t frames = 0;
    if (!check(wav_frame_count(out_path, frames), "open full output")) {
        return false;
    }
    return check(frames == k_frames, "no trim keeps full 2 second duration");
}

// --start beyond the input duration is rejected before rendering writes output.
bool verify_start_beyond_duration_rejected() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_oob_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 3.0, std::nullopt);
    if (!check(!res.success(), "render with --start past duration fails")) {
        return false;
    }
    if (!check(res.error.code == mradm::ErrorCode::invalid_argument, "out-of-range start returns invalid_argument")) {
        return false;
    }
    return check(!std::filesystem::exists(out_path), "no output written for out-of-range start");
}

// --end <= --start is rejected.
bool verify_end_not_after_start_rejected() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_bad_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 1.0, 1.0);
    if (!check(!res.success(), "render with --end == --start fails")) {
        return false;
    }
    return check(res.error.code == mradm::ErrorCode::invalid_argument, "--end <= --start returns invalid_argument");
}

// A window that passes the seconds-level --end > --start check but collapses to
// zero frames once rounded must be rejected before rendering, leaving no output.
bool verify_subframe_window_rejected() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_subframe_out", ".wav");
    const FileGuard out_guard(out_path);

    // start at frame 48000; end only 0.4 of a frame later still rounds to 48000.
    const double start_sec = 1.0;
    const double end_sec = 1.0 + (0.4 / static_cast<double>(k_sr));
    const auto res = render_with_trim(in_path, out_path, start_sec, end_sec);
    if (!check(!res.success(), "render with sub-frame window fails")) {
        return false;
    }
    if (!check(res.error.code == mradm::ErrorCode::invalid_argument, "sub-frame window returns invalid_argument")) {
        return false;
    }
    return check(!std::filesystem::exists(out_path), "no output written for sub-frame window");
}

// The backend meters only the trimmed window, so loudness/True-Peak describe the
// kept segment: the silent first half reports no peak, the active second half
// does. If metering ignored the window, the silent half would still report the
// full render's peak.
bool verify_metrics_follow_trim_window() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);

    const auto out_silent = temp_path("mr_trim_metric_silent", ".wav");
    const FileGuard guard_silent(out_silent);
    const auto res_silent = render_with_trim(in_path, out_silent, 0.0, 1.0);
    if (!check(res_silent.success(), "render silent-half segment succeeds")) {
        return false;
    }
    if (!check(res_silent.metrics.has_value() && !res_silent.metrics->measured_peak_dbtp.has_value(),
               "silent first-half segment reports no true-peak (meter followed the window)")) {
        return false;
    }

    const auto out_active = temp_path("mr_trim_metric_active", ".wav");
    const FileGuard guard_active(out_active);
    const auto res_active = render_with_trim(in_path, out_active, 1.0, std::nullopt);
    if (!check(res_active.success(), "render active-half segment succeeds")) {
        return false;
    }
    return check(res_active.metrics.has_value() && res_active.metrics->measured_peak_dbtp.has_value(),
                 "active second-half segment reports a true-peak");
}

// HOA-specific check: the HOA backend measures loudness/True-Peak by decoding the
// ambisonics output to a 7.1.4 reference domain (AllRAD) with LFE separated. This
// confirms the meter window is honoured through that decode path too: silent first
// half reports no peak, active second half does.
bool verify_hoa_metrics_follow_trim_window() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);

    const auto out_silent = temp_path("mr_trim_hoa_silent", ".wav");
    const FileGuard guard_silent(out_silent);
    const auto res_silent = render_with_trim(in_path, out_silent, 0.0, 1.0, mradm::RendererSelection::hoa, "hoa3");
    if (!check(res_silent.success(), "HOA render silent-half segment succeeds")) {
        return false;
    }
    if (!check(res_silent.metrics.has_value() && !res_silent.metrics->measured_peak_dbtp.has_value(),
               "HOA silent first-half reports no true-peak (window honoured through AllRAD decode)")) {
        return false;
    }

    const auto out_active = temp_path("mr_trim_hoa_active", ".wav");
    const FileGuard guard_active(out_active);
    const auto res_active =
        render_with_trim(in_path, out_active, 1.0, std::nullopt, mradm::RendererSelection::hoa, "hoa3");
    if (!check(res_active.success(), "HOA render active-half segment succeeds")) {
        return false;
    }
    return check(res_active.metrics.has_value() && res_active.metrics->measured_peak_dbtp.has_value(),
                 "HOA active second-half reports a true-peak");
}

// Phase 1 core guarantee: on-demand window rendering (seek + pre-roll) is
// sample-identical to a full render then sliced. The window starts well past one
// k_block_size block so the seek + pre-roll path actually engages; the diffuse +
// varying-signal fixture means a missing/short pre-roll would corrupt the window head.
bool window_bit_exact(const std::filesystem::path& in_path,
                      mradm::RendererSelection renderer,
                      const std::string& layout,
                      const char* label) {
    const auto full_path = temp_path("mr_trim_be_full", ".wav");
    const FileGuard full_guard(full_path);
    const auto win_path = temp_path("mr_trim_be_win", ".wav");
    const FileGuard win_guard(win_path);

    // [24000, 40000) frames. start 24000 spans >2 default blocks (k_block_size 8875),
    // forcing a real reader seek and (for stateful backends) a pre-roll block.
    constexpr uint64_t k_start = 24000U;
    constexpr uint64_t k_count = 16000U;
    const double start_sec = static_cast<double>(k_start) / k_sr;
    const double end_sec = static_cast<double>(k_start + k_count) / k_sr;

    if (!check(render_with_trim(in_path, full_path, 0.0, std::nullopt, renderer, layout).success(),
               "full render succeeds")) {
        return false;
    }
    if (!check(render_with_trim(in_path, win_path, start_sec, end_sec, renderer, layout).success(),
               "windowed render succeeds")) {
        return false;
    }

    auto full = mradm::audio::FloatWavReader::open(full_path.string());
    auto win = mradm::audio::FloatWavReader::open(win_path.string());
    if (!check(static_cast<bool>(full) && static_cast<bool>(win), "open both render outputs")) {
        return false;
    }
    const uint32_t ch = full->channels();
    if (!check(win->channels() == ch, "channel counts match") ||
        !check(win->frame_count() == k_count, "windowed output is exactly the window length")) {
        return false;
    }

    std::vector<float> full_buf(full->frame_count() * ch);
    full->read(full_buf.data(), full->frame_count());
    std::vector<float> win_buf(k_count * ch);
    win->read(win_buf.data(), k_count);

    bool exact = true;
    for (uint64_t i = 0; i < k_count * ch && exact; ++i) {
        exact = (win_buf[i] == full_buf[(k_start * ch) + i]);
    }
    return check(exact, label);
}

// EAR (5.1), SAF/VBAP (5.1), HOA (hoa3) windowed render must each match a full render
// then sliced, sample for sample. The diffuse fixture exercises EAR's decorrelator +
// comp delay and HOA's 1024-tap diffuse delay line; VBAP is DSP-stateless.
bool verify_window_bit_exact_vs_full() {
    const auto in_path = write_varying_fixture();
    const FileGuard in_guard(in_path);
    bool ok = window_bit_exact(in_path, mradm::RendererSelection::ear, "0+5+0", "ear: window == full sliced");
    ok = window_bit_exact(in_path, mradm::RendererSelection::saf, "0+5+0", "vbap: window == full sliced") && ok;
    ok = window_bit_exact(in_path, mradm::RendererSelection::hoa, "hoa3", "hoa: window == full sliced") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_end_trim();
    ok &= verify_start_end_window();
    ok &= verify_no_trim_full_duration();
    ok &= verify_start_beyond_duration_rejected();
    ok &= verify_end_not_after_start_rejected();
    ok &= verify_subframe_window_rejected();
    ok &= verify_metrics_follow_trim_window();
    ok &= verify_hoa_metrics_follow_trim_window();
    ok &= verify_window_bit_exact_vs_full();
    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "render trim fixture tests passed (9/9)\n";
    return EXIT_SUCCESS;
}
