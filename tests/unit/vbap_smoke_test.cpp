#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/render.h"
#include "adm/render_vbap.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// getpid() 在 POSIX 来自 <unistd.h>，Windows 用 <process.h> 的 _getpid()。
[[nodiscard]] int current_process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return ::getpid();
#endif
}

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

class CapturingLogSink final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel level, std::string_view module, std::string_view message) override {
        if (level == mradm::LogLevel::warning && module == "saf-vbap") {
            warnings_.emplace_back(message);
        }
    }

    [[nodiscard]] bool has_warnings() const { return !warnings_.empty(); }

    [[nodiscard]] bool has_warning_containing(std::string_view needle) const {
        return std::ranges::any_of(warnings_,
                                   [needle](const std::string& w) { return w.find(needle) != std::string::npos; });
    }

  private:
    std::vector<std::string> warnings_;
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

std::pair<std::shared_ptr<adm::Document>, std::string> make_time_varying_objects_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"VbapTimedCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects left_block{adm::SphericalPosition{adm::Azimuth{-30.0F}, adm::Elevation{0.0F}}};
        left_block.set(adm::Gain{1.0F});
        left_block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{0}}});
        left_block.set(adm::Duration{adm::Time{std::chrono::milliseconds{500}}});
        left_block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(left_block);

        adm::AudioBlockFormatObjects right_block{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}}};
        right_block.set(adm::Gain{1.0F});
        right_block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{500}}});
        right_block.set(adm::Duration{adm::Time{std::chrono::milliseconds{500}}});
        right_block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(right_block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"VbapTimedPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"VbapTimedSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"VbapTimedTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"VbapTimedObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"VbapTimedContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"VbapTimedProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_overlong_interpolation_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"VbapClampCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects left_block{adm::SphericalPosition{adm::Azimuth{-30.0F}, adm::Elevation{0.0F}}};
        left_block.set(adm::Gain{1.0F});
        left_block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{0}}});
        left_block.set(adm::Duration{adm::Time{std::chrono::milliseconds{500}}});
        left_block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(left_block);

        adm::AudioBlockFormatObjects right_block{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}}};
        right_block.set(adm::Gain{1.0F});
        right_block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{500}}});
        right_block.set(adm::Duration{adm::Time{std::chrono::milliseconds{100}}});
        right_block.set(
            adm::JumpPosition{adm::JumpPositionFlag{false}, adm::InterpolationLength{std::chrono::seconds{1}}});
        cf->add(right_block);

        adm::AudioBlockFormatObjects final_block{adm::SphericalPosition{adm::Azimuth{-30.0F}, adm::Elevation{0.0F}}};
        final_block.set(adm::Gain{1.0F});
        final_block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{600}}});
        final_block.set(adm::Duration{adm::Time{std::chrono::milliseconds{400}}});
        final_block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(final_block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"VbapClampPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"VbapClampSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"VbapClampTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"VbapClampObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"VbapClampContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"VbapClampProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_nested_start_objects_doc() {
    auto doc = adm::Document::create();

    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"VbapNestedCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{0}}});
        block.set(adm::Duration{adm::Time{std::chrono::milliseconds{500}}});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"VbapNestedPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"VbapNestedSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"VbapNestedTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto child = adm::AudioObject::create(adm::AudioObjectName{"VbapNestedChild"});
    child->set(adm::Start{adm::Time{std::chrono::milliseconds{250}}});
    child->addReference(uid);
    doc->add(child);

    auto parent = adm::AudioObject::create(adm::AudioObjectName{"VbapNestedParent"});
    parent->set(adm::Start{adm::Time{std::chrono::milliseconds{250}}});
    parent->addReference(child);
    doc->add(parent);

    auto content = adm::AudioContent::create(adm::AudioContentName{"VbapNestedContent"});
    content->addReference(parent);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"VbapNestedProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<adm::Document>, std::string>
make_direct_speakers_doc(const char* label, float azimuth, float elevation = 0.0F) {
    auto doc = adm::Document::create();

    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"VbapDsCF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    {
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{azimuth}, adm::Elevation{elevation}, adm::Distance{1.0F}}};
        block.add(adm::SpeakerLabel{label});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"VbapDsPF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"VbapDsSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"VbapDsTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"VbapDsObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"VbapDsContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"VbapDsProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::filesystem::path write_input_fixture(const std::shared_ptr<adm::Document>& doc,
                                          const std::string& uid_str,
                                          uint16_t sample_rate = 48000U,
                                          uint32_t frames = 1000U) {
    static std::atomic<int> s_seq{0};
    const auto name = "mr_vbap_in_" + std::to_string(current_process_id()) + "_" +
                      std::to_string(s_seq.fetch_add(1)) + ".wav";
    auto path = std::filesystem::temp_directory_path() / name;

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    auto writer = bw64::writeFile(path.string(), 1U, sample_rate, 24U, chna, axml);
    std::vector<float> samples(frames, 0.5F);
    writer->write(samples.data(), frames);

    return path;
}

std::vector<double> read_channel_sums(const std::filesystem::path& path, std::size_t expected_channels) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        return {};
    }
    auto& reader = *reader_res;
    const auto actual_channels = static_cast<std::size_t>(reader.channels());
    if (actual_channels != expected_channels) {
        std::cerr << "FAIL: expected " << expected_channels << " output channels, got " << actual_channels << "\n";
        return {};
    }
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(n_frames * actual_channels);
    reader.read(samples.data(), reader.frame_count());

    std::vector<double> sums(actual_channels, 0.0);
    for (std::size_t frame = 0; frame < n_frames; ++frame) {
        for (std::size_t ch = 0; ch < actual_channels; ++ch) {
            sums[ch] += std::fabs(static_cast<double>(samples[(frame * actual_channels) + ch]));
        }
    }
    return sums;
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_mdap_doc(float width) {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"MdapCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        block.set(adm::Width{width});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"MdapPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"MdapSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"MdapTF"}, adm::FormatDefinition::PCM);
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
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

double sum_elevated_channels(const std::filesystem::path& path) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        return 0.0;
    }
    auto& reader = *reader_res;
    const auto num_ch = static_cast<std::size_t>(reader.channels());
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(n_frames * num_ch);
    reader.read(samples.data(), reader.frame_count());

    // 5.1.4 output is 10ch including LFE; channels 6-9 are the elevated speakers.
    double elevated_sum = 0.0;
    for (std::size_t frame = 0; frame < n_frames; frame++) {
        for (std::size_t ch = 6U; ch < num_ch; ch++) {
            elevated_sum += std::fabs(static_cast<double>(samples[(frame * num_ch) + ch]));
        }
    }
    return elevated_sum;
}

bool render_mdap_and_sum_elevated(float width, double& out_elevated_sum) {
    auto [doc, uid_str] = make_mdap_doc(width);
    const auto path_in = write_input_fixture(doc, uid_str);
    FileGuard in_guard{path_in};

    auto path_out = std::filesystem::temp_directory_path() / "mr_vbap_mdap_out.wav";
    FileGuard out_guard{path_out};

    mradm::RenderRequest req;
    req.input_path = path_in;
    req.output_path = path_out;
    req.options.output_layout = "4+5+0"; // 10ch 5.1.4: ch6-9 are elevated
    req.options.renderer = mradm::RendererSelection::saf;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: MDAP render (width=" << width << ") failed: " << res.error.message << "\n";
        return false;
    }

    out_elevated_sum = sum_elevated_channels(path_out);
    return true;
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
    request.options.internal_allow_speaker_stereo = true;
    request.options.renderer = mradm::RendererSelection::saf;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        std::cerr << "FAIL: " << label << " VBAP fixture render failed: " << result.error.message << "\n";
        return false;
    }

    auto out_reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!out_reader_res) {
        std::cerr << "FAIL: cannot open VBAP output: " << out_reader_res.error().message << "\n";
        return false;
    }
    auto& out_reader = *out_reader_res;
    bool ok = true;
    ok &= check(out_reader.channels() == 2U, "VBAP output has 2 channels");
    ok &= check(out_reader.sample_rate() == 48000U, "VBAP output sample rate == 48000");
    ok &= check(out_reader.frame_count() == 1000U, "VBAP output frame count == 1000");

    if (ok) {
        const auto n_frames = static_cast<std::size_t>(out_reader.frame_count());
        std::vector<float> out_samples(n_frames * 2U);
        out_reader.read(out_samples.data(), out_reader.frame_count());

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
}

bool verify_time_varying_objects_blocks() {
    auto [doc, uid_str] = make_time_varying_objects_doc();
    const auto in_path = write_input_fixture(doc, uid_str, 1000U, 1000U);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_timed_blocks_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+2+0";
    request.options.internal_allow_speaker_stereo = true;
    request.options.renderer = mradm::RendererSelection::saf;
    request.options.peak_limit = false;
    request.options.object_smoothing_frames = 0;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        std::cerr << "FAIL: time-varying Objects render failed: " << result.error.message << "\n";
        return false;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        std::cerr << "FAIL: cannot open time-varying output: " << reader_res.error().message << "\n";
        return false;
    }
    auto& reader = *reader_res;
    bool ok = true;
    ok &= check(reader.channels() == 2U, "time-varying output is stereo");
    ok &= check(reader.sample_rate() == 1000U, "time-varying output sample rate == 1000");
    ok &= check(reader.frame_count() == 1000U, "time-varying output frame count == 1000");
    if (!ok) {
        return false;
    }

    std::vector<float> samples(static_cast<std::size_t>(reader.frame_count()) * 2U);
    reader.read(samples.data(), reader.frame_count());

    double first_left = 0.0;
    double first_right = 0.0;
    double second_left = 0.0;
    double second_right = 0.0;
    for (std::size_t frame = 0; frame < 500U; ++frame) {
        first_left += std::fabs(static_cast<double>(samples[2U * frame]));
        first_right += std::fabs(static_cast<double>(samples[(2U * frame) + 1U]));
    }
    for (std::size_t frame = 500U; frame < 1000U; ++frame) {
        second_left += std::fabs(static_cast<double>(samples[2U * frame]));
        second_right += std::fabs(static_cast<double>(samples[(2U * frame) + 1U]));
    }

    constexpr double k_leak_tolerance = 1.0e-3;
    // first block az=-30 → M-030 → ch1 (right); second block az=+30 → M+030 → ch0 (left)
    ok &= check(first_right > 0.0, "first timed block (az=-30) renders to right speaker (M-030, ch1)");
    ok &= check(first_left < k_leak_tolerance, "first timed block does not leak into left speaker");
    ok &= check(second_left > 0.0, "second timed block (az=+30) renders to left speaker (M+030, ch0)");
    ok &= check(second_right < k_leak_tolerance, "second timed block does not leak into right speaker");
    return ok;
}

bool verify_overlong_interpolation_is_clamped() {
    auto [doc, uid_str] = make_overlong_interpolation_doc();
    const auto in_path = write_input_fixture(doc, uid_str, 1000U, 1000U);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_interp_clamp_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+2+0";
    request.options.internal_allow_speaker_stereo = true;
    request.options.renderer = mradm::RendererSelection::saf;
    request.options.peak_limit = false;
    request.options.object_smoothing_frames = 0;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        std::cerr << "FAIL: overlong interpolation render failed: " << result.error.message << "\n";
        return false;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        std::cerr << "FAIL: cannot open interpolation clamp output: " << reader_res.error().message << "\n";
        return false;
    }
    auto& reader = *reader_res;
    bool ok = true;
    ok &= check(reader.channels() == 2U, "interpolation clamp output is stereo");
    ok &= check(reader.sample_rate() == 1000U, "interpolation clamp output sample rate == 1000");
    ok &= check(reader.frame_count() == 1000U, "interpolation clamp output frame count == 1000");
    if (!ok) {
        return false;
    }

    std::vector<float> samples(static_cast<std::size_t>(reader.frame_count()) * 2U);
    reader.read(samples.data(), reader.frame_count());

    double middle_left = 0.0;
    double middle_right = 0.0;
    for (std::size_t frame = 500U; frame < 600U; ++frame) {
        middle_left += std::fabs(static_cast<double>(samples[2U * frame]));
        middle_right += std::fabs(static_cast<double>(samples[(2U * frame) + 1U]));
    }

    // second block az=+30 → M+030 → ch0 (left); clamping means M+030 dominates in frames 500-600
    ok &= check(middle_left > middle_right * 0.8, "overlong interpolation clamps to current block duration");
    return ok;
}

bool verify_nested_audio_object_start_offsets() {
    auto [doc, uid_str] = make_nested_start_objects_doc();
    const auto in_path = write_input_fixture(doc, uid_str, 1000U, 1000U);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_nested_start_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+2+0";
    request.options.internal_allow_speaker_stereo = true;
    request.options.renderer = mradm::RendererSelection::saf;
    request.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        std::cerr << "FAIL: nested AudioObject start render failed: " << result.error.message << "\n";
        return false;
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        std::cerr << "FAIL: cannot open nested start output: " << reader_res.error().message << "\n";
        return false;
    }
    auto& reader = *reader_res;
    std::vector<float> samples(static_cast<std::size_t>(reader.frame_count()) * 2U);
    reader.read(samples.data(), reader.frame_count());

    double pre_start = 0.0;
    double active = 0.0;
    for (std::size_t frame = 0; frame < 500U; ++frame) {
        pre_start += std::fabs(static_cast<double>(samples[2U * frame]));
        pre_start += std::fabs(static_cast<double>(samples[(2U * frame) + 1U]));
    }
    for (std::size_t frame = 500U; frame < 1000U; ++frame) {
        active += std::fabs(static_cast<double>(samples[2U * frame]));
        active += std::fabs(static_cast<double>(samples[(2U * frame) + 1U]));
    }

    bool ok = true;
    ok &= check(pre_start < 1.0e-6, "nested AudioObject start offsets silence frames before cumulative start");
    ok &= check(active > 0.0, "nested AudioObject start offsets render after cumulative start");
    return ok;
}

bool verify_direct_speakers_label_routing() {
    auto [doc, uid_str] = make_direct_speakers_doc("M+030", 30.0F);
    const auto in_path = write_input_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_ds_label_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+2+0";
    request.options.internal_allow_speaker_stereo = true;
    request.options.renderer = mradm::RendererSelection::saf;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        std::cerr << "FAIL: DirectSpeakers label route render failed: " << result.error.message << "\n";
        return false;
    }

    const auto sums = read_channel_sums(out_path, 2U);
    bool ok = true;
    ok &= check(sums.size() == 2U, "DirectSpeakers label output is stereo");
    if (ok) {
        ok &= check(sums[0] > 0.0, "DirectSpeakers label M+030 routes to ch0 (M+030)");
        ok &= check(sums[1] < 1.0e-6, "DirectSpeakers label M+030 does not leak to ch1 (M-030)");
    }
    return ok;
}

bool verify_wav71_rear_side_order() {
    auto render_ds = [](const char* label, float azimuth, const char* suffix) {
        auto [doc, uid_str] = make_direct_speakers_doc(label, azimuth);
        const auto in_path = write_input_fixture(doc, uid_str);
        FileGuard in_guard{in_path};

        const auto out_path =
            std::filesystem::temp_directory_path() / (std::string{"mr_vbap_wav71_"} + suffix + ".wav");
        FileGuard out_guard{out_path};

        mradm::RenderRequest request;
        request.input_path = in_path;
        request.output_path = out_path;
        request.options.output_layout = "wav71";
        request.options.renderer = mradm::RendererSelection::saf;

        mradm::RenderService service;
        mradm::NullProgressSink progress;
        mradm::NullLogSink logs;
        const mradm::RenderResult result = service.render(request, progress, logs);
        if (!result.success()) {
            std::cerr << "FAIL: wav71 DirectSpeakers render failed: " << result.error.message << "\n";
            return std::vector<double>{};
        }
        return read_channel_sums(out_path, 8U);
    };

    // WAVE_7_1 order is L R C LFE Lrs Rrs Ls Rs.  The old BS.2051 0+7+0 order
    // had side before rear; this locks the public wav71 slot order.
    const auto rear = render_ds("M+135", 135.0F, "rear");
    const auto side = render_ds("M+090", 90.0F, "side");

    bool ok = true;
    ok &= check(rear.size() == 8U, "wav71 rear output has 8 channels");
    ok &= check(side.size() == 8U, "wav71 side output has 8 channels");
    if (ok) {
        ok &= check(rear[4] > 0.0, "wav71 M+135 routes to rear-left slot ch4");
        ok &= check(rear[6] < 1.0e-6, "wav71 M+135 does not route to side-left slot ch6");
        ok &= check(side[6] > 0.0, "wav71 M+090 routes to side-left slot ch6");
        ok &= check(side[4] < 1.0e-6, "wav71 M+090 does not route to rear-left slot ch4");
    }
    return ok;
}

bool verify_direct_speakers_alias_routing() {
    auto [doc, uid_str] = make_direct_speakers_doc("RC_LFE", 0.0F);
    const auto in_path = write_input_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_ds_alias_lfe_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+5+0";
    request.options.renderer = mradm::RendererSelection::saf;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        std::cerr << "FAIL: DirectSpeakers alias route render failed: " << result.error.message << "\n";
        return false;
    }

    constexpr std::size_t k_lfe_ch = 3U;
    const auto sums = read_channel_sums(out_path, 6U);
    bool ok = true;
    ok &= check(sums.size() == 6U, "DirectSpeakers alias output is 5.1");
    if (ok) {
        ok &= check(sums[k_lfe_ch] > 0.0, "DirectSpeakers alias RC_LFE routes to LFE1");
        for (std::size_t ch = 0; ch < sums.size(); ++ch) {
            if (ch == k_lfe_ch) {
                continue;
            }
            ok &= check(sums[ch] < 1.0e-6, "DirectSpeakers alias RC_LFE does not leak to non-LFE channels");
        }
    }
    return ok;
}

bool verify_direct_speakers_position_fallback_wrap() {
    auto [doc, uid_str] = make_direct_speakers_doc("NOT_A_BS2051_LABEL", -179.0F);
    const auto in_path = write_input_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_ds_fallback_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "9+10+3";
    request.options.renderer = mradm::RendererSelection::saf;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    CapturingLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        std::cerr << "FAIL: DirectSpeakers fallback render failed: " << result.error.message << "\n";
        return false;
    }

    // 9+10+3 is now 24ch (22 non-LFE + LFE1[3] + LFE2[9]); M+180 is at index 8.
    constexpr std::size_t k_rear_channel = 8U; // M+180 in 24ch BS.2051 9+10+3 order
    constexpr std::size_t k_lfe1_channel = 3U; // LFE1 — always zero for non-LFE sources
    constexpr std::size_t k_lfe2_channel = 9U; // LFE2 — always zero for non-LFE sources
    const auto sums = read_channel_sums(out_path, 24U);
    bool ok = true;
    ok &= check(sums.size() == 24U, "DirectSpeakers fallback output has 24 channels");
    if (ok) {
        ok &= check(sums[k_rear_channel] > 0.0, "DirectSpeakers fallback wraps -179° to rear M+180");
        for (std::size_t ch = 0; ch < sums.size(); ++ch) {
            if (ch == k_rear_channel || ch == k_lfe1_channel || ch == k_lfe2_channel) {
                continue;
            }
            ok &= check(sums[ch] < 1.0e-6, "DirectSpeakers fallback does not leak to non-nearest channels");
        }
    }
    ok &= check(logs.has_warnings(), "DirectSpeakers fallback emits warning on label miss");
    return ok;
}

bool verify_vbap_unified_extra_layouts() {
    struct Case {
        const char* layout_id;
        const char* label;
        float azimuth;
        float elevation;
        std::size_t channels;
        std::size_t target_channel;
    };
    constexpr std::array<Case, 2> cases{{
        {"2+5+0", "U+030", 30.0F, 30.0F, 8U, 6U},
        {"4+5+4", "U+150", 150.0F, 45.0F, 14U, 12U},
    }};

    bool ok = true;
    for (const auto& c : cases) {
        auto [doc, uid_str] = make_direct_speakers_doc(c.label, c.azimuth, c.elevation);
        const auto in_path = write_input_fixture(doc, uid_str);
        FileGuard in_guard{in_path};
        const auto out_path =
            std::filesystem::temp_directory_path() / (std::string{"mr_vbap_unified_"} + c.layout_id + "_out.wav");
        FileGuard out_guard{out_path};

        mradm::RenderRequest request;
        request.input_path = in_path;
        request.output_path = out_path;
        request.options.output_layout = c.layout_id;
        request.options.renderer = mradm::RendererSelection::saf;

        mradm::RenderService service;
        mradm::NullProgressSink progress;
        mradm::NullLogSink logs;
        const auto res = service.render(request, progress, logs);
        if (!res.success()) {
            std::cerr << "FAIL: SAF unified layout render failed for " << c.layout_id << ": " << res.error.message
                      << "\n";
            return false;
        }

        const auto sums = read_channel_sums(out_path, c.channels);
        ok &= check(sums.size() == c.channels, "SAF unified layout: output channel count");
        if (sums.size() == c.channels) {
            ok &= check(sums[c.target_channel] > 0.0, "SAF unified layout: target top channel has energy");
            ok &= check(sums[3U] < 1.0e-9, "SAF unified layout: LFE silent for non-LFE source");
        }
    }
    return ok;
}

// Verify 9.1.6 layout produces a 16-channel output and that the LFE channel
// (index 3) carries no energy when a non-LFE DirectSpeakers source is rendered.
bool verify_916_output_is_16ch() {
    // Center speaker (M+000, ch2) as a simple non-LFE source.
    auto [doc, uid_str] = make_direct_speakers_doc("M+000", 0.0F);
    const auto in_path = write_input_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_916_16ch_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "9.1.6";
    request.options.renderer = mradm::RendererSelection::saf;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        std::cerr << "FAIL: 9.1.6 render failed: " << result.error.message << "\n";
        return false;
    }

    // 9.1.6: L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr
    constexpr std::size_t k_center_ch = 2U; // M+000 (C)
    constexpr std::size_t k_lfe_ch = 3U;    // LFE1 — always zero for non-LFE source
    const auto sums = read_channel_sums(out_path, 16U);
    bool ok = true;
    ok &= check(sums.size() == 16U, "9.1.6 output has 16 channels");
    if (ok) {
        ok &= check(sums[k_center_ch] > 0.0, "9.1.6 center (ch2) carries energy from M+000 source");
        ok &= check(sums[k_lfe_ch] < 1.0e-9, "9.1.6 LFE (ch3) is silent for non-LFE source");
    }
    return ok;
}

// Verify 9.1.6 top-side channel routing: a DS source with label "U+110" (Ltm)
// must land on channel 12, and a position-fallback at az=110/el=45 must also
// resolve to the same channel (nearest non-LFE speaker).
bool verify_916_top_side_routing() {
    // 9.1.6 channel index map (non-LFE only here):
    //  0  L    1  R    2  C    3  LFE  4  Ls   5  Rs
    //  6  Rls  7  Rrs  8  Lw   9  Rw   10 Vhl  11 Vhr
    // 12  Ltm  13 Rtm  14 Ltr  15 Rtr
    constexpr std::size_t k_ltm_ch = 12U; // U+110 (top side left)
    constexpr std::size_t k_lfe_ch = 3U;

    bool ok = true;

    // ── Sub-test A: label routing via "U+110" ────────────────────────────────
    {
        auto [doc, uid_str] = make_direct_speakers_doc("U+110", 110.0F, 45.0F);
        const auto in_path = write_input_fixture(doc, uid_str);
        FileGuard in_guard{in_path};
        const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_916_ltm_label_out.wav";
        FileGuard out_guard{out_path};

        mradm::RenderRequest request;
        request.input_path = in_path;
        request.output_path = out_path;
        request.options.output_layout = "9.1.6";
        request.options.renderer = mradm::RendererSelection::saf;

        mradm::RenderService service;
        mradm::NullProgressSink progress;
        mradm::NullLogSink logs;
        const mradm::RenderResult res = service.render(request, progress, logs);
        if (!res.success()) {
            std::cerr << "FAIL: 9.1.6 top-side label render failed: " << res.error.message << "\n";
            return false;
        }

        const auto sums = read_channel_sums(out_path, 16U);
        ok &= check(sums.size() == 16U, "9.1.6 top-side label: 16 channels");
        if (ok) {
            ok &= check(sums[k_ltm_ch] > 0.0, "9.1.6 U+110 label routes to ch12 (Ltm)");
            ok &= check(sums[k_lfe_ch] < 1.0e-9, "9.1.6 top-side: LFE ch3 silent");
            for (std::size_t ch = 0; ch < sums.size(); ++ch) {
                if (ch == k_ltm_ch || ch == k_lfe_ch) {
                    continue;
                }
                if (!check(sums[ch] < 1.0e-6, "9.1.6 top-side label: energy leaked to non-target channel")) {
                    ok = false;
                }
            }
        }
    }

    // ── Sub-test B: position fallback az=110/el=45 → nearest = U+110 (ch12) ─
    {
        auto [doc, uid_str] = make_direct_speakers_doc("NOT_A_BS2051_LABEL", 110.0F, 45.0F);
        const auto in_path = write_input_fixture(doc, uid_str);
        FileGuard in_guard{in_path};
        const auto out_path = std::filesystem::temp_directory_path() / "mr_vbap_916_ltm_fallback_out.wav";
        FileGuard out_guard{out_path};

        mradm::RenderRequest request;
        request.input_path = in_path;
        request.output_path = out_path;
        request.options.output_layout = "9.1.6";
        request.options.renderer = mradm::RendererSelection::saf;

        mradm::RenderService service;
        mradm::NullProgressSink progress;
        CapturingLogSink logs;
        const mradm::RenderResult res = service.render(request, progress, logs);
        if (!res.success()) {
            std::cerr << "FAIL: 9.1.6 top-side fallback render failed: " << res.error.message << "\n";
            return false;
        }

        const auto sums = read_channel_sums(out_path, 16U);
        ok &= check(sums.size() == 16U, "9.1.6 top-side fallback: 16 channels");
        if (ok) {
            ok &= check(sums[k_ltm_ch] > 0.0, "9.1.6 position fallback az=110/el=45 routes to ch12 (Ltm)");
            ok &= check(logs.has_warnings(), "9.1.6 position fallback emits warning on label miss");
        }
    }

    return ok;
}

// Verify MDAP spread: Objects with width=0.8 rendered to 4+5+0 (9ch, has
// elevated speakers at ±45°) should distribute energy to elevated channels.
// Compare against the same source with width=0 (pure VBAP) to confirm spread
// actually changes the gain distribution.
bool verify_mdap_spread_fixture() {
    double elevated_no_spread = 0.0;
    double elevated_with_spread = 0.0;
    bool ok = true;
    ok &= render_mdap_and_sum_elevated(0.0F, elevated_no_spread);
    ok &= render_mdap_and_sum_elevated(0.8F, elevated_with_spread);
    if (!ok) {
        return false;
    }
    ok &= check(elevated_with_spread > elevated_no_spread,
                "MDAP spread: elevated channels get more energy with width=0.8 than width=0");
    return ok;
}

// Returns total |sample| energy summed across all channels and frames.
double read_total_energy(const std::filesystem::path& path, std::size_t channels) {
    const auto sums = read_channel_sums(path, channels);
    return std::accumulate(sums.begin(), sums.end(), 0.0);
}

// Returns total |sample| energy for [start_frame, end_frame) across all channels.
double read_segment_energy(const std::filesystem::path& path,
                           std::size_t channels,
                           std::size_t start_frame,
                           std::size_t end_frame) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        return -1.0;
    }
    auto& reader = *reader_res;
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(n_frames * channels);
    reader.read(samples.data(), reader.frame_count());

    double sum = 0.0;
    for (std::size_t f = start_frame; f < std::min(end_frame, n_frames); ++f) {
        for (std::size_t ch = 0; ch < channels; ++ch) {
            sum += std::fabs(static_cast<double>(samples[(f * channels) + ch]));
        }
    }
    return sum;
}

// Build a minimal mono polar-front Objects document and return {doc, obj_ptr, uid_str}.
struct SimpleDoc {
    std::shared_ptr<adm::Document> doc;
    std::shared_ptr<adm::AudioObject> obj;
    std::string uid_str;
};

SimpleDoc make_simple_front_doc(const char* suffix = "") {
    auto doc = adm::Document::create();
    const std::string s{suffix};
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"ObjCF" + s}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"ObjPF" + s}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"ObjSF" + s}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"ObjTF" + s}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"ObjObject" + s});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"ObjContent" + s});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"ObjProg" + s});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, obj, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

bool render_simple(const std::shared_ptr<adm::Document>& doc,
                   const std::string& uid_str,
                   const std::filesystem::path& out_path,
                   uint16_t sample_rate = 48000U,
                   uint32_t frames = 1000U) {
    const auto in_path = write_input_fixture(doc, uid_str, sample_rate, frames);
    FileGuard in_guard{in_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = "0+2+0";
    req.options.internal_allow_speaker_stereo = true;
    req.options.renderer = mradm::RendererSelection::saf;
    req.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: render failed: " << res.error.message << "\n";
        return false;
    }
    return true;
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_object_semantics_doc(
    float azimuth, bool channel_lock, std::optional<float> max_distance, float divergence, float azimuth_range) {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"SemCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{azimuth}, adm::Elevation{0.0F}}};
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        if (channel_lock) {
            adm::ChannelLock lock;
            lock.set(adm::ChannelLockFlag{true});
            if (max_distance.has_value()) {
                lock.set(adm::MaxDistance{*max_distance});
            }
            block.set(lock);
        }
        if (divergence > 0.0F) {
            adm::ObjectDivergence od;
            od.set(adm::Divergence{divergence});
            od.set(adm::AzimuthRange{azimuth_range});
            block.set(od);
        }
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"SemPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"SemSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"SemTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"SemObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"SemContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"SemProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

bool render_to_layout(const std::shared_ptr<adm::Document>& doc,
                      const std::string& uid_str,
                      const std::filesystem::path& out_path,
                      std::string_view layout_id) {
    const auto in_path = write_input_fixture(doc, uid_str);
    FileGuard in_guard{in_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.output_layout = std::string{layout_id};
    req.options.renderer = mradm::RendererSelection::saf;
    req.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: render_to_layout failed: " << res.error.message << "\n";
        return false;
    }
    return true;
}

bool verify_audio_object_gain_scales_output() {
    bool ok = true;
    double energy_full = 0.0;
    double energy_half = 0.0;

    // Render with obj.gain = 1.0 (default)
    {
        auto [doc, obj, uid_str] = make_simple_front_doc("A");
        const auto out = std::filesystem::temp_directory_path() / "mr_vbap_obj_gain_full_out.wav";
        FileGuard out_g{out};
        if (!render_simple(doc, uid_str, out)) {
            return false;
        }
        energy_full = read_total_energy(out, 2U);
    }

    // Render with obj.gain = 0.5
    {
        auto [doc, obj, uid_str] = make_simple_front_doc("B");
        obj->set(adm::Gain{0.5});
        const auto out = std::filesystem::temp_directory_path() / "mr_vbap_obj_gain_half_out.wav";
        FileGuard out_g{out};
        if (!render_simple(doc, uid_str, out)) {
            return false;
        }
        energy_half = read_total_energy(out, 2U);
    }

    ok &= check(energy_full > 0.0, "obj gain: reference render is not silent");
    ok &= check(energy_half > 0.0, "obj gain: gain=0.5 render is not silent");
    const double ratio = (energy_full > 0.0) ? (energy_half / energy_full) : 0.0;
    ok &= check(ratio > 0.48 && ratio < 0.52, "obj gain=0.5 halves total output energy");
    return ok;
}

bool verify_vbap_channel_lock_routes_to_nearest_speaker() {
    auto [doc, uid_str] = make_object_semantics_doc(20.0F, true, std::nullopt, 0.0F, 45.0F);
    const auto out = std::filesystem::temp_directory_path() / "mr_vbap_channel_lock_out.wav";
    FileGuard out_g{out};
    if (!render_to_layout(doc, uid_str, out, "0+5+0")) {
        return false;
    }

    const auto sums = read_channel_sums(out, 6U);
    bool ok = true;
    ok &= check(sums.size() == 6U, "channelLock: output is 5.1");
    if (sums.size() == 6U) {
        ok &= check(sums[0] > 100.0, "channelLock: nearest left speaker carries energy");
        ok &= check(sums[1] < 1.0e-6, "channelLock: right speaker is silent");
        ok &= check(sums[2] < 1.0e-6, "channelLock: center speaker is silent after lock");
        ok &= check(sums[3] < 1.0e-9, "channelLock: LFE is ignored");
    }
    return ok;
}

bool verify_vbap_channel_lock_max_distance_rejects_lock() {
    auto [locked_doc, locked_uid] = make_object_semantics_doc(20.0F, true, std::nullopt, 0.0F, 45.0F);
    auto [free_doc, free_uid] = make_object_semantics_doc(20.0F, true, 0.01F, 0.0F, 45.0F);

    const auto locked_out = std::filesystem::temp_directory_path() / "mr_vbap_channel_lock_ref_out.wav";
    const auto free_out = std::filesystem::temp_directory_path() / "mr_vbap_channel_lock_reject_out.wav";
    FileGuard locked_g{locked_out};
    FileGuard free_g{free_out};
    if (!render_to_layout(locked_doc, locked_uid, locked_out, "0+5+0") ||
        !render_to_layout(free_doc, free_uid, free_out, "0+5+0")) {
        return false;
    }

    const auto locked = read_channel_sums(locked_out, 6U);
    const auto free = read_channel_sums(free_out, 6U);
    bool ok = true;
    ok &= check(locked.size() == 6U && free.size() == 6U, "channelLock maxDistance: outputs are 5.1");
    if (locked.size() == 6U && free.size() == 6U) {
        ok &= check(locked[2] < 1.0e-6, "channelLock maxDistance: reference lock has no center energy");
        ok &= check(free[2] > 1.0, "channelLock maxDistance: rejected lock falls back to VBAP center contribution");
    }
    return ok;
}

bool verify_vbap_object_divergence_spreads_center_source() {
    auto [doc, uid_str] = make_object_semantics_doc(0.0F, false, std::nullopt, 1.0F, 30.0F);
    const auto out = std::filesystem::temp_directory_path() / "mr_vbap_divergence_out.wav";
    FileGuard out_g{out};
    if (!render_to_layout(doc, uid_str, out, "0+5+0")) {
        return false;
    }

    const auto sums = read_channel_sums(out, 6U);
    bool ok = true;
    ok &= check(sums.size() == 6U, "objectDivergence: output is 5.1");
    if (sums.size() == 6U) {
        ok &= check(sums[0] > 1.0, "objectDivergence: left virtual source contributes");
        ok &= check(sums[1] > 1.0, "objectDivergence: right virtual source contributes");
        ok &= check(sums[2] < 1.0e-6, "objectDivergence: divergence=1 removes center contribution");
        const double ratio = sums[0] > 0.0 ? sums[1] / sums[0] : 0.0;
        ok &= check(ratio > 0.95 && ratio < 1.05, "objectDivergence: left/right energy is balanced");
    }
    return ok;
}

bool verify_audio_object_mute_silences_output() {
    auto [doc, obj, uid_str] = make_simple_front_doc("M");
    obj->set(adm::Mute{true});

    const auto out = std::filesystem::temp_directory_path() / "mr_vbap_obj_mute_out.wav";
    FileGuard out_g{out};
    if (!render_simple(doc, uid_str, out)) {
        return false;
    }

    const double energy = read_total_energy(out, 2U);
    return check(energy == 0.0, "muted AudioObject produces silent output");
}

bool verify_audio_object_duration_gates_output() {
    // sample_rate=1000, frames=1000; obj.duration=250ms = 250 samples.
    // Frames 0-249 should have audio; frames 250-999 should be silent.
    auto [doc, obj, uid_str] = make_simple_front_doc("D");
    obj->set(adm::Duration{adm::Time{std::chrono::milliseconds{250}}});

    const auto out = std::filesystem::temp_directory_path() / "mr_vbap_obj_duration_out.wav";
    FileGuard out_g{out};
    if (!render_simple(doc, uid_str, out, 1000U, 1000U)) {
        return false;
    }

    bool ok = true;
    const double active_energy = read_segment_energy(out, 2U, 0U, 250U);
    const double silent_energy = read_segment_energy(out, 2U, 250U, 1000U);
    ok &= check(active_energy > 0.0, "obj duration: frames 0-249 have audio");
    ok &= check(silent_energy == 0.0, "obj duration: frames 250-999 are silent");
    return ok;
}

bool verify_ds_time_window_gates_block() {
    // DS block with rtime=250ms, duration=250ms at 1000Hz / 1000 frames.
    // Only frames 250-499 should be active for M+030 (ch0 in 0+2+0).
    auto doc = adm::Document::create();
    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"DsTimeCF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    {
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}, adm::Distance{1.0F}}};
        block.add(adm::SpeakerLabel{"M+030"});
        block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{250}}});
        block.set(adm::Duration{adm::Time{std::chrono::milliseconds{250}}});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"DsTimePF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"DsTimeSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"DsTimeTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"DsTimeObj"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"DsTimeContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"DsTimeProg"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    const std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());

    const auto out = std::filesystem::temp_directory_path() / "mr_vbap_ds_timewin_out.wav";
    FileGuard out_g{out};
    if (!render_simple(doc, uid_str, out, 1000U, 1000U)) {
        return false;
    }

    bool ok = true;
    // Left channel (ch 0) carries M+030 label routing (0+2+0: ch0=M+030, ch1=M-030).
    // read_segment_energy sums both channels, so this test is channel-order-independent.
    const double before = read_segment_energy(out, 2U, 0U, 250U);
    const double active = read_segment_energy(out, 2U, 250U, 500U);
    const double after = read_segment_energy(out, 2U, 500U, 1000U);
    ok &= check(before == 0.0, "DS time window: frames 0-249 are silent");
    ok &= check(active > 0.0, "DS time window: frames 250-499 have audio");
    ok &= check(after == 0.0, "DS time window: frames 500-999 are silent");
    return ok;
}

} // namespace

// Verifies that AudioObject positionOffset is applied in the VBAP path:
// block at az=30 (L speaker in stereo) + offset azimuth=-30 → effective az=0
// (center) → equal energy in L and R channels.
bool verify_vbap_position_offset() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"OffVbapCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"OffVbapPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"OffVbapSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"OffVbapTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"OffVbapObject"});
    obj->addReference(uid);
    obj->set(adm::SphericalPositionOffset{adm::AzimuthOffset{-30.0F}});
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"OffVbapContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"OffVbapProg"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);

    const std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());
    const auto out = std::filesystem::temp_directory_path() / "mr_vbap_offset_out.wav";
    FileGuard out_g{out};
    if (!render_simple(doc, uid_str, out)) {
        return false;
    }

    const auto sums = read_channel_sums(out, 2U);
    bool ok = true;
    ok &= check(sums[0] > 0.0, "vbap positionOffset: L channel not silent");
    ok &= check(sums[1] > 0.0, "vbap positionOffset: R channel not silent");
    const double ratio = (sums[0] > 0.0) ? (sums[1] / sums[0]) : 0.0;
    ok &= check(ratio > 0.9 && ratio < 1.1, "vbap positionOffset: L≈R for az=30+offset(-30)=center");
    return ok;
}

// Two-block document without jumpPosition: block1 az=-30 (right), block2 az=+30 (left),
// each 500ms.  The renderer will apply the default interpolation ramp at the transition.
std::pair<std::shared_ptr<adm::Document>, std::string> make_two_block_no_jump_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"TBnjCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects b1{adm::SphericalPosition{adm::Azimuth{-30.0F}, adm::Elevation{0.0F}}};
        b1.set(adm::Rtime{adm::Time{std::chrono::milliseconds{0}}});
        b1.set(adm::Duration{adm::Time{std::chrono::milliseconds{500}}});
        cf->add(b1);

        adm::AudioBlockFormatObjects b2{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}}};
        b2.set(adm::Rtime{adm::Time{std::chrono::milliseconds{500}}});
        b2.set(adm::Duration{adm::Time{std::chrono::milliseconds{500}}});
        cf->add(b2);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"TBnjPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"TBnjSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TBnjTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"TBnjObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"TBnjContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"TBnjProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// Render the two-block document with the given interp_ms and return stereo samples.
std::vector<float> render_two_block_no_jump(uint32_t interp_ms) {
    auto [doc, uid_str] = make_two_block_no_jump_doc();
    // 1000Hz × 1000 frames = 1 second; block boundary at frame 500.
    const auto in_path = write_input_fixture(doc, uid_str, 1000U, 1000U);
    FileGuard in_guard{in_path};

    const auto out_path =
        std::filesystem::temp_directory_path() / ("mr_vbap_interp_ms_" + std::to_string(interp_ms) + "_out.wav");
    FileGuard out_guard{out_path};

    mradm::RenderRequest request;
    request.input_path = in_path;
    request.output_path = out_path;
    request.options.output_layout = "0+2+0";
    request.options.internal_allow_speaker_stereo = true;
    request.options.renderer = mradm::RendererSelection::saf;
    request.options.peak_limit = false;
    request.options.default_interp_ms = interp_ms;
    request.options.object_smoothing_frames = 0;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        return {};
    }

    auto reader_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!reader_res) {
        return {};
    }
    auto& reader = *reader_res;
    std::vector<float> samples(static_cast<std::size_t>(reader.frame_count()) * 2U);
    reader.read(samples.data(), reader.frame_count());
    return samples;
}

bool verify_default_interp_ms_controls_ramp() {
    bool ok = true;

    // With interp_ms=0: block transition at frame 500 is instant.
    // Frames 510-540 (well into block 2, az=+30 → left) must have no right-channel
    // energy from the previous block.
    const auto samples0 = render_two_block_no_jump(0);
    if (samples0.empty()) {
        std::cerr << "FAIL: interp_ms=0 render failed\n";
        return false;
    }
    double right_after_0 = 0.0;
    for (std::size_t frame = 510; frame < 540; ++frame) {
        right_after_0 += std::fabs(static_cast<double>(samples0[(2U * frame) + 1U]));
    }
    ok &= check(right_after_0 < 1.0e-6, "interp_ms=0: no right energy at frames 510-539 (instant switch, block2=left)");

    // With interp_ms=200 (200 samples at 1000Hz): ramp spans frames 500-699.
    // Frames 510-540 are inside the ramp → must still have measurable right-channel
    // energy from the previous block.
    const auto samples200 = render_two_block_no_jump(200);
    if (samples200.empty()) {
        std::cerr << "FAIL: interp_ms=200 render failed\n";
        return false;
    }
    double right_after_200 = 0.0;
    for (std::size_t frame = 510; frame < 540; ++frame) {
        right_after_200 += std::fabs(static_cast<double>(samples200[(2U * frame) + 1U]));
    }
    ok &= check(right_after_200 > 1.0e-3, "interp_ms=200: right energy present at frames 510-539 (ramp not complete)");

    if (ok) {
        std::cout << "PASS: verify_default_interp_ms_controls_ramp\n";
    }
    return ok;
}

bool verify_register_vbap_layout() {
    bool ok = true;

    // ── Valid registration ─────────────────────────────────────────────────────
    const bool registered = mradm::register_vbap_layout(
        "test+custom",
        "Test Custom 4ch",
        {{30.0F, 0.0F, "L"}, {-30.0F, 0.0F, "R"}, {110.0F, 0.0F, "Ls"}, {-110.0F, 0.0F, "Rs"}});
    ok &= check(registered, "register_vbap_layout: valid layout should succeed");

    // ── Duplicate rejection (same id, different speakers) ──────────────────────
    const bool dup = mradm::register_vbap_layout("test+custom", "Dup", {{30.0F, 0.0F, "L"}, {-30.0F, 0.0F, "R"}});
    ok &= check(!dup, "register_vbap_layout: duplicate id must return false");

    // ── Built-in id is immutable ───────────────────────────────────────────────
    const bool builtin_dup =
        mradm::register_vbap_layout("0+2+0", "Override", {{30.0F, 0.0F, "M+030"}, {-30.0F, 0.0F, "M-030"}});
    ok &= check(!builtin_dup, "register_vbap_layout: built-in id must not be overridden");

    // ── Registered layout visible in capabilities with correct metadata ─────────
    const auto caps = mradm::vbap_capabilities();
    const auto it = std::ranges::find_if(caps.supported_layouts, [](const auto& l) { return l.id == "test+custom"; });
    ok &= check(it != caps.supported_layouts.end(), "register_vbap_layout: registered layout appears in capabilities");
    if (it != caps.supported_layouts.end()) {
        ok &= check(it->channel_count == 4, "register_vbap_layout: channel_count == 4");
        ok &= check(!it->is_3d, "register_vbap_layout: 2D custom layout: is_3d == false");
        ok &= check(it->lfe_count == 0, "register_vbap_layout: lfe_count == 0");
    }

    // ── Invalid input rejection ────────────────────────────────────────────────
    ok &= check(!mradm::register_vbap_layout("", "Empty id", {{30.0F, 0.0F, "L"}}),
                "register_vbap_layout: empty id rejected");
    ok &= check(!mradm::register_vbap_layout("test+empty-speakers", "Empty speakers", {}),
                "register_vbap_layout: empty speakers rejected");
    ok &= check(!mradm::register_vbap_layout(
                    "test+all-lfe", "All LFE", {{30.0F, -30.0F, "LFE1", true}, {-30.0F, -30.0F, "LFE2", true}}),
                "register_vbap_layout: all-LFE layout rejected");
    ok &= check(!mradm::register_vbap_layout(
                    "test+nan-az", "NaN azimuth", {{std::numeric_limits<float>::quiet_NaN(), 0.0F, "L"}}),
                "register_vbap_layout: NaN azimuth rejected");
    ok &= check(!mradm::register_vbap_layout(
                    "test+inf-el", "Inf elevation", {{30.0F, std::numeric_limits<float>::infinity(), "L"}}),
                "register_vbap_layout: infinite elevation rejected");

    return ok;
}

// Creates a minimal Objects document with a source at the given elevation.
std::pair<std::shared_ptr<adm::Document>, std::string> make_elevated_objects_doc(float elevation_deg) {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"ElevCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{elevation_deg}}};
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"ElevPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"ElevSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"ElevTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"ElevObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"ElevContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"ElevProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// Verify the 2D-layout-with-elevated-sources warning:
//   - 2D layout (0+2+0) + elevated source (el=30°)  → warning containing "2D"
//   - 3D layout (4+5+0) + elevated source (el=30°)  → no warning
//   - 2D layout (0+2+0) + floor source   (el=0°)    → no warning
bool verify_2d_layout_elevated_source_warning() {
    bool ok = true;

    struct RenderCapture {
        bool success{false};
        CapturingLogSink logs;
    };

    auto render_and_capture = [](std::string_view layout_id, float el_deg) -> RenderCapture {
        auto [doc, uid_str] = make_elevated_objects_doc(el_deg);
        const auto path_in = write_input_fixture(doc, uid_str);
        FileGuard in_guard{path_in};

        auto path_out = std::filesystem::temp_directory_path() / "mr_vbap_2d_warn_out.wav";
        FileGuard out_guard{path_out};

        mradm::RenderRequest req;
        req.input_path = path_in;
        req.output_path = path_out;
        req.options.output_layout = std::string{layout_id};
        if (std::string_view{layout_id} == "0+2+0") {
            req.options.internal_allow_speaker_stereo = true;
        }
        req.options.renderer = mradm::RendererSelection::saf;
        req.options.peak_limit = false;

        mradm::RenderService service;
        mradm::NullProgressSink progress;
        CapturingLogSink logs;
        const auto result = service.render(req, progress, logs);
        return {result.success(), logs};
    };

    // 2D layout + elevated source → must warn with "2D" in the message.
    {
        const auto capture = render_and_capture("0+2+0", 30.0F);
        ok &= check(capture.success, "2D layout + elevated source: render succeeds");
        ok &= check(capture.logs.has_warning_containing("2D"), "2D layout + elevated source: warning mentions '2D'");
        ok &=
            check(capture.logs.has_warning_containing("elevation") || capture.logs.has_warning_containing("horizontal"),
                  "2D layout + elevated source: warning mentions height loss");
    }

    // 3D layout + elevated source → no warning expected.
    {
        const auto capture = render_and_capture("4+5+0", 30.0F);
        ok &= check(capture.success, "3D layout + elevated source: render succeeds");
        ok &= check(!capture.logs.has_warning_containing("2D"), "3D layout + elevated source: no 2D warning");
    }

    // 2D layout + floor-level source → no warning.
    {
        const auto capture = render_and_capture("0+2+0", 0.0F);
        ok &= check(capture.success, "2D layout + floor source: render succeeds");
        ok &= check(!capture.logs.has_warning_containing("2D"), "2D layout + floor source: no warning");
    }

    if (ok) {
        std::cout << "PASS: verify_2d_layout_elevated_source_warning\n";
    }
    return ok;
}

// VbapStream (realtime) reproduces the offline render_window render and is independent of
// the caller's pull chunk size (canonical block + FIFO). Drives the renderer directly so
// no semantic policy / post-processing sits between stream and reference.
bool verify_vbap_stream_matches_window() {
    auto [doc, uid_str] = make_objects_doc(ObjectPositionMode::polar_front);
    const auto in_path = write_input_fixture(doc, uid_str, 48000U, 4096U);
    FileGuard in_guard{in_path};

    auto scene = mradm::io::import_scene(in_path.string());
    if (!check(scene.has_value(), "vbap stream: import scene")) {
        return false;
    }
    mradm::RenderPlan plan;
    plan.input_path = in_path.string();
    plan.output_layout = "0+5+0";
    plan.scene = *scene;

    auto renderer = mradm::create_vbap_renderer();
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "vbap stream: prepare")) {
        return false;
    }

    const auto ref_path = std::filesystem::temp_directory_path() / "mr_vbap_stream_ref.wav";
    FileGuard ref_guard{ref_path};
    mradm::RenderPlan window_plan = plan;
    window_plan.output_path = ref_path.string();
    if (!check(renderer->render_window(**prepared, window_plan, progress, logs).has_value(),
               "vbap stream: reference render_window")) {
        return false;
    }
    auto reader = mradm::audio::FloatWavReader::open(ref_path.string());
    if (!check(reader.has_value(), "vbap stream: reference WAV opens")) {
        return false;
    }
    const auto ch = static_cast<std::size_t>(reader->channels());
    std::vector<float> ref(ch * reader->frame_count());
    reader->read(ref.data(), reader->frame_count());

    auto pull_stream = [&](const std::vector<std::size_t>& chunk_pattern) -> std::optional<std::vector<float>> {
        auto stream = renderer->open_stream(**prepared, plan, logs);
        if (!check(stream.has_value(), "vbap stream: open_stream")) {
            return std::nullopt;
        }
        const uint32_t oc = (*stream)->out_channels();
        std::vector<float> out;
        std::vector<float> buf;
        std::size_t pi = 0;
        while (true) {
            const std::size_t frames = chunk_pattern[pi % chunk_pattern.size()];
            ++pi;
            buf.assign(frames * oc, 0.0F);
            auto produced = (*stream)->process(std::span<float>(buf), frames);
            if (!produced) {
                return std::nullopt;
            }
            if (*produced == 0) {
                break;
            }
            out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*produced * oc));
        }
        return out;
    };

    const auto uniform = pull_stream({1024});
    const auto varied = pull_stream({333, 1000, 512, 7});
    if (!uniform || !varied) {
        return false;
    }

    bool ok = check(uniform->size() == ref.size(), "vbap stream output frame count matches render_window");
    ok &= check(*uniform == *varied, "vbap stream output is identical regardless of pull chunk size");
    double max_diff = 0.0;
    const std::size_t n = std::min(uniform->size(), ref.size());
    for (std::size_t i = 0; i < n; ++i) {
        max_diff = std::max(max_diff, std::fabs(static_cast<double>((*uniform)[i]) - static_cast<double>(ref[i])));
    }
    ok &= check(max_diff < 1.0e-4, "vbap stream output matches the offline render_window render");
    return ok;
}

// A live per-object gain override pre-scales the object's input channel, so the VBAP stream
// output scales by the override factor (linear, commutes with the pan).
bool verify_vbap_stream_gain_override() {
    auto [doc, uid_str] = make_objects_doc(ObjectPositionMode::polar_front);
    const auto in_path = write_input_fixture(doc, uid_str, 48000U, 4096U);
    FileGuard in_guard{in_path};

    auto scene = mradm::io::import_scene(in_path.string());
    if (!check(scene.has_value() && !scene->objects.empty(), "vbap override: import scene")) {
        return false;
    }
    const std::string object_id = scene->objects.front().id;

    mradm::RenderPlan plan;
    plan.input_path = in_path.string();
    plan.output_layout = "0+5+0";
    plan.scene = *scene;

    auto renderer = mradm::create_vbap_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "vbap override: prepare")) {
        return false;
    }

    auto pull_rms = [&](bool with_override) -> double {
        auto stream = renderer->open_stream(**prepared, plan, logs);
        if (!stream.has_value()) {
            return -1.0;
        }
        if (with_override) {
            mradm::LiveOverrides ov;
            ov.revision = 1;
            ov.objects.push_back({object_id, -20.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, ""}); // 0.1 linear
            (*stream)->set_overrides(ov);
        }
        const uint32_t oc = (*stream)->out_channels();
        const std::size_t block_floats = std::size_t{1024U} * oc;
        std::vector<float> buf(block_floats, 0.0F);
        double total = 0.0;
        std::size_t n = 0;
        while (true) {
            buf.assign(block_floats, 0.0F);
            auto produced = (*stream)->process(std::span<float>(buf), 1024U);
            if (!produced || *produced == 0) {
                break;
            }
            const auto used = static_cast<std::vector<float>::difference_type>(*produced * oc);
            total += std::accumulate(buf.begin(), buf.begin() + used, 0.0, [](double acc, float s) {
                return acc + (static_cast<double>(s) * static_cast<double>(s));
            });
            n += *produced * oc;
        }
        return n > 0 ? std::sqrt(total / static_cast<double>(n)) : 0.0;
    };

    const double base = pull_rms(false);
    const double over = pull_rms(true);
    bool ok = check(base > 1.0e-3, "vbap override: baseline has energy");
    ok &= check(over < base * 0.2 && over > base * 0.05, "vbap override: -20 dB scales output by ~0.1");
    return ok;
}

// NOLINTNEXTLINE(readability-function-size): linear smoke-test checklist.
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
    const auto wav71 = std::ranges::find_if(caps.supported_layouts, [](const auto& layout) {
        return layout.id == "wav71" && layout.channel_count == 8U && layout.lfe_count == 1U;
    });
    if (wav71 == caps.supported_layouts.end()) {
        std::cerr << "FAIL: saf-vbap must expose wav71 as the public 7.1 layout\n";
        ok = false;
    }
    const auto old_070 =
        std::ranges::find_if(caps.supported_layouts, [](const auto& layout) { return layout.id == "0+7+0"; });
    if (old_070 != caps.supported_layouts.end()) {
        std::cerr << "FAIL: saf-vbap must not expose old 0+7+0 layout id\n";
        ok = false;
    }
    constexpr std::array<const char*, 9> k_expected_layouts{
        "0+2+0", "0+5+0", "2+5+0", "4+5+0", "wav71", "4+7+0", "4+5+4", "9.1.6", "9+10+3"};
    for (const auto* expected : k_expected_layouts) {
        const auto it = std::ranges::find_if(caps.supported_layouts,
                                             [expected](const auto& layout) { return layout.id == expected; });
        if (it == caps.supported_layouts.end()) {
            std::cerr << "FAIL: saf-vbap capabilities missing layout " << expected << "\n";
            ok = false;
        }
    }
    if (!caps.supports_objects) {
        std::cerr << "FAIL: saf-vbap must declare supports_objects\n";
        ok = false;
    }
    if (!caps.supports_direct_speakers) {
        std::cerr << "FAIL: saf-vbap must declare supports_direct_speakers\n";
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
    ok &= verify_time_varying_objects_blocks();
    ok &= verify_overlong_interpolation_is_clamped();
    ok &= verify_nested_audio_object_start_offsets();
    ok &= verify_direct_speakers_label_routing();
    ok &= verify_wav71_rear_side_order();
    ok &= verify_direct_speakers_alias_routing();
    ok &= verify_direct_speakers_position_fallback_wrap();
    ok &= verify_vbap_unified_extra_layouts();
    ok &= verify_916_output_is_16ch();
    ok &= verify_916_top_side_routing();
    ok &= verify_mdap_spread_fixture();
    ok &= verify_audio_object_gain_scales_output();
    ok &= verify_audio_object_mute_silences_output();
    ok &= verify_audio_object_duration_gates_output();
    ok &= verify_ds_time_window_gates_block();
    ok &= verify_vbap_position_offset();
    ok &= verify_vbap_channel_lock_routes_to_nearest_speaker();
    ok &= verify_vbap_channel_lock_max_distance_rejects_lock();
    ok &= verify_vbap_object_divergence_spreads_center_source();
    ok &= verify_default_interp_ms_controls_ramp();
    ok &= verify_register_vbap_layout();
    ok &= verify_2d_layout_elevated_source_warning();
    ok &= verify_vbap_stream_matches_window();
    ok &= verify_vbap_stream_gain_override();

    if (ok) {
        std::cout << "vbap smoke test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
