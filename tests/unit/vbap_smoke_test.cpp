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
#include <utility>
#include <vector>

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm/audio_io.h"
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

class CapturingLogSink final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel level, std::string_view module, std::string_view message) override {
        if (level == mradm::LogLevel::warning && module == "saf-vbap") {
            warnings_.emplace_back(message);
        }
    }

    [[nodiscard]] bool has_warnings() const { return !warnings_.empty(); }

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

std::pair<std::shared_ptr<adm::Document>, std::string> make_direct_speakers_doc(const char* label, float azimuth) {
    auto doc = adm::Document::create();

    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"VbapDsCF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    {
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{azimuth}, adm::Elevation{0.0F}, adm::Distance{1.0F}}};
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
    auto path = std::filesystem::temp_directory_path() / "mr_vbap_fixture_in.wav";

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    auto writer = bw64::writeFile(path.string(), 1U, sample_rate, 24U, chna, axml);
    std::vector<float> samples(frames, 0.5F);
    writer->write(samples.data(), frames);

    return path;
}

std::vector<double> read_channel_sums(const std::filesystem::path& path, std::size_t channels) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        return {};
    }
    auto& reader = *reader_res;
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(n_frames * channels);
    reader.read(samples.data(), reader.frame_count());

    std::vector<double> sums(channels, 0.0);
    for (std::size_t frame = 0; frame < n_frames; ++frame) {
        for (std::size_t ch = 0; ch < channels; ++ch) {
            sums[ch] += std::fabs(static_cast<double>(samples[(frame * channels) + ch]));
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
    constexpr std::size_t k_num_ch = 9U;
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(n_frames * k_num_ch);
    reader.read(samples.data(), reader.frame_count());

    // Channels 5-8 are the elevated speakers (elevation +/-45 degrees).
    double elevated_sum = 0.0;
    for (std::size_t frame = 0; frame < n_frames; frame++) {
        for (std::size_t ch = 5U; ch < k_num_ch; ch++) {
            elevated_sum += std::fabs(static_cast<double>(samples[(frame * k_num_ch) + ch]));
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
    request.options.renderer = mradm::RendererSelection::saf;
    request.options.peak_limit = false;

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
    ok &= check(first_left > 0.0, "first timed block renders to left speaker");
    ok &= check(first_right < k_leak_tolerance, "first timed block does not leak into right speaker");
    ok &= check(second_right > 0.0, "second timed block renders to right speaker");
    ok &= check(second_left < k_leak_tolerance, "second timed block does not leak into left speaker");
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
    request.options.renderer = mradm::RendererSelection::saf;
    request.options.peak_limit = false;

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

    ok &= check(middle_right > middle_left * 0.8, "overlong interpolation clamps to current block duration");
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
        ok &= check(sums[0] < 1.0e-6, "DirectSpeakers label M+030 does not leak to M-030 channel");
        ok &= check(sums[1] > 0.0, "DirectSpeakers label M+030 routes to M+030 channel");
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

    constexpr std::size_t k_rear_channel = 7U; // M+180 in layout_spec("9+10+3")
    const auto sums = read_channel_sums(out_path, 22U);
    bool ok = true;
    ok &= check(sums.size() == 22U, "DirectSpeakers fallback output has 22 channels");
    if (ok) {
        ok &= check(sums[k_rear_channel] > 0.0, "DirectSpeakers fallback wraps -179° to rear M+180");
        for (std::size_t ch = 0; ch < sums.size(); ++ch) {
            if (ch != k_rear_channel) {
                ok &= check(sums[ch] < 1.0e-6, "DirectSpeakers fallback does not leak to non-nearest channels");
            }
        }
    }
    ok &= check(logs.has_warnings(), "DirectSpeakers fallback emits warning on label miss");
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
    ok &= verify_direct_speakers_position_fallback_wrap();
    ok &= verify_mdap_spread_fixture();

    if (ok) {
        std::cout << "vbap smoke test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
