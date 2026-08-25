#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
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
#include "adm/render_binaural.h"

#include "binaural_test_probe.h"

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

[[nodiscard]] uint32_t read_be32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24U) | (static_cast<uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 2U]) << 8U) | static_cast<uint32_t>(bytes[offset + 3U]);
}

[[nodiscard]] uint64_t read_be64(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint64_t>(read_be32(bytes, offset)) << 32U) | read_be32(bytes, offset + 4U);
}

[[nodiscard]] std::size_t find_caf_chunk(const std::vector<unsigned char>& bytes, std::string_view id) {
    std::size_t offset = 8U;
    while (offset + 12U <= bytes.size()) {
        const auto chunk_id = std::string_view{reinterpret_cast<const char*>(bytes.data() + offset), 4U};
        const uint64_t size = read_be64(bytes, offset + 4U);
        if (chunk_id == id) {
            return offset;
        }
        if (size > bytes.size() || offset + 12U + static_cast<std::size_t>(size) > bytes.size()) {
            break;
        }
        offset += 12U + static_cast<std::size_t>(size);
    }
    return std::string::npos;
}

[[nodiscard]] uint32_t read_caf_layout_tag(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    const auto chan = find_caf_chunk(bytes, "chan");
    return chan == std::string::npos ? 0U : read_be32(bytes, chan + 12U);
}

std::filesystem::path temp_path(std::string_view stem, std::string_view ext) {
    static std::atomic<int> s_seq{0};
    const auto name = std::string(stem) + "_" + std::to_string(current_process_id()) + "_" +
                      std::to_string(s_seq.fetch_add(1)) + std::string(ext);
    return std::filesystem::temp_directory_path() / name;
}

struct ObjectFixtureOptions {
    float azimuth{0.0F};
    std::chrono::milliseconds rtime{0};
    std::chrono::milliseconds duration{80};
    bool channel_lock{false};
    float divergence{0.0F};
    float divergence_range{45.0F};
    float diffuse{0.0F};
    float width{0.0F};
    float height{0.0F};
    float depth{0.0F};
    std::optional<bool> object_head_locked;
    std::optional<bool> block_head_locked;
};

std::pair<std::shared_ptr<adm::Document>, std::string> make_objects_doc(const ObjectFixtureOptions& opts) {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"BinauralCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{opts.azimuth}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        block.set(adm::Rtime{adm::Time{opts.rtime}});
        block.set(adm::Duration{adm::Time{opts.duration}});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        if (opts.diffuse > 0.0F) {
            block.set(adm::Diffuse{opts.diffuse});
        }
        if (opts.width > 0.0F) {
            block.set(adm::Width{opts.width});
        }
        if (opts.height > 0.0F) {
            block.set(adm::Height{opts.height});
        }
        if (opts.depth > 0.0F) {
            block.set(adm::Depth{opts.depth});
        }
        if (opts.block_head_locked.has_value()) {
            block.set(adm::HeadLocked{*opts.block_head_locked});
        }
        if (opts.channel_lock) {
            adm::ChannelLock lock;
            lock.set(adm::ChannelLockFlag{true});
            block.set(lock);
        }
        if (opts.divergence > 0.0F) {
            adm::ObjectDivergence od;
            od.set(adm::Divergence{opts.divergence});
            od.set(adm::AzimuthRange{opts.divergence_range});
            block.set(od);
        }
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"BinauralPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"BinauralSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"BinauralTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"BinauralObject"});
    if (opts.object_head_locked.has_value()) {
        obj->set(adm::HeadLocked{*opts.object_head_locked});
    }
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"BinauralContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"BinauralProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_head_locked_timeline_doc() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"BinauralHeadTimelineCF"},
                                              adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{0}}});
        block.set(adm::Duration{adm::Time{std::chrono::milliseconds{200}}});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        block.set(adm::HeadLocked{false});
        cf->add(block);
    }
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{200}}});
        block.set(adm::Duration{adm::Time{std::chrono::milliseconds{200}}});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        block.set(adm::HeadLocked{true});
        cf->add(block);
    }
    doc->add(cf);
    auto pf =
        adm::AudioPackFormat::create(adm::AudioPackFormatName{"BinauralHeadTimelinePF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"BinauralHeadTimelineSF"},
                                             adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf =
        adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"BinauralHeadTimelineTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"BinauralHeadTimelineObject"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"BinauralHeadTimelineContent"});
    content->addReference(obj);
    doc->add(content);
    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"BinauralHeadTimelineProgramme"});
    programme->addReference(content);
    doc->add(programme);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_mixed_divergence_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"BinauralMixedDivCF"},
                                              adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{0}}});
        block.set(adm::Duration{adm::Time{std::chrono::milliseconds{40}}});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        cf->add(block);
    }
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        block.set(adm::Rtime{adm::Time{std::chrono::milliseconds{40}}});
        block.set(adm::Duration{adm::Time{std::chrono::milliseconds{40}}});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{false}});
        adm::ObjectDivergence od;
        od.set(adm::Divergence{1.0F});
        od.set(adm::AzimuthRange{60.0F});
        block.set(od);
        cf->add(block);
    }
    doc->add(cf);

    auto pf =
        adm::AudioPackFormat::create(adm::AudioPackFormatName{"BinauralMixedDivPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf =
        adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"BinauralMixedDivSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf =
        adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"BinauralMixedDivTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"BinauralMixedDivObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"BinauralMixedDivContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"BinauralMixedDivProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<adm::Document>, std::string>
make_lfe_direct_speakers_doc(std::chrono::milliseconds rtime, std::chrono::milliseconds duration) {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"BinauralLfeCF"},
                                              adm::TypeDefinition::DIRECT_SPEAKERS);
    {
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{45.0F}, adm::Elevation{-35.0F}, adm::Distance{1.0F}}};
        block.add(adm::SpeakerLabel{"RC_LFE"});
        block.set(adm::Gain{1.0F});
        block.set(adm::Rtime{adm::Time{rtime}});
        block.set(adm::Duration{adm::Time{duration}});
        cf->add(block);
    }
    doc->add(cf);

    auto pf =
        adm::AudioPackFormat::create(adm::AudioPackFormatName{"BinauralLfePF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"BinauralLfeSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"BinauralLfeTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"BinauralLfeObject"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"BinauralLfeContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"BinauralLfeProgramme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::filesystem::path write_fixture(const ObjectFixtureOptions& opts, uint32_t frames) {
    constexpr uint32_t k_ch = 1U;
    constexpr uint32_t k_sr = 48000U;

    const auto [doc, uid_str] = make_objects_doc(opts);
    auto path = temp_path("mr_binaural_input", ".wav");

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

std::filesystem::path write_head_locked_timeline_fixture(uint32_t frames) {
    constexpr uint32_t k_sr = 48000U;
    const auto [doc, uid] = make_head_locked_timeline_doc();
    auto path = temp_path("mr_binaural_head_timeline", ".wav");
    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
    auto writer = bw64::writeFile(path.string(), 1U, k_sr, 24U, chna, axml);
    std::vector<float> samples(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        samples[i] = 0.25F * std::sin(2.0F * std::numbers::pi_v<float> * 440.0F * static_cast<float>(i) /
                                      static_cast<float>(k_sr));
    }
    writer->write(samples.data(), frames);
    return path;
}

std::filesystem::path
write_fixture(float azimuth, std::chrono::milliseconds rtime, std::chrono::milliseconds duration, uint32_t frames) {
    return write_fixture(ObjectFixtureOptions{.azimuth = azimuth, .rtime = rtime, .duration = duration}, frames);
}

std::filesystem::path write_mixed_divergence_fixture(uint32_t frames) {
    constexpr uint32_t k_ch = 1U;
    constexpr uint32_t k_sr = 48000U;

    const auto [doc, uid_str] = make_mixed_divergence_doc();
    auto path = temp_path("mr_binaural_mixed_divergence_input", ".wav");

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

std::filesystem::path
write_lfe_fixture(std::chrono::milliseconds rtime, std::chrono::milliseconds duration, uint32_t frames) {
    constexpr uint32_t k_ch = 1U;
    constexpr uint32_t k_sr = 48000U;

    const auto [doc, uid_str] = make_lfe_direct_speakers_doc(rtime, duration);
    auto path = temp_path("mr_binaural_lfe_input", ".wav");

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);

    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    auto writer = bw64::writeFile(path.string(), k_ch, k_sr, 24U, chna, axml);
    std::vector<float> samples(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        samples[i] = 0.25F * std::sin(2.0F * std::numbers::pi_v<float> * 80.0F * static_cast<float>(i) /
                                      static_cast<float>(k_sr));
    }
    writer->write(samples.data(), frames);
    return path;
}

double
channel_energy(const std::vector<float>& samples, uint32_t channels, uint32_t ch, std::size_t begin, std::size_t end) {
    double e = 0.0;
    for (std::size_t f = begin; f < end; ++f) {
        const double v = samples[(f * channels) + ch];
        e += v * v;
    }
    return e;
}

bool render_to_path(const std::filesystem::path& input,
                    const std::filesystem::path& output,
                    mradm::RenderOptions options);

std::optional<std::vector<float>> render_stereo_samples(const std::filesystem::path& input,
                                                        std::string_view output_stem) {
    const auto out = temp_path(output_stem, ".wav");
    FileGuard out_guard(out);

    mradm::RenderOptions options;
    options.renderer = mradm::RendererSelection::binaural;
    options.peak_limit = false;
    options.measure_loudness = false;
    if (!render_to_path(input, out, options)) {
        return std::nullopt;
    }

    auto reader = mradm::audio::FloatWavReader::open(out.string());
    if (!check(reader.has_value(), "binaural semantic WAV output opens")) {
        return std::nullopt;
    }
    if (!check(reader->channels() == 2U, "binaural semantic output is stereo")) {
        return std::nullopt;
    }
    std::vector<float> samples(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());
    return samples;
}

double sample_difference_energy(const std::vector<float>& lhs, const std::vector<float>& rhs) {
    const auto n = std::min(lhs.size(), rhs.size());
    double e = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double diff = static_cast<double>(lhs[i]) - static_cast<double>(rhs[i]);
        e += diff * diff;
    }
    return e;
}

mradm::RenderRequest make_request(const std::filesystem::path& input, const std::filesystem::path& output) {
    mradm::RenderRequest req;
    req.input_path = input;
    req.output_path = output;
    req.options.renderer = mradm::RendererSelection::saf_binaural;
    req.options.peak_limit = false;
    req.options.measure_loudness = false;
    return req;
}

bool render_to_path(const std::filesystem::path& input,
                    const std::filesystem::path& output,
                    mradm::RenderOptions options) {
    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;

    mradm::RenderRequest req;
    req.input_path = input;
    req.output_path = output;
    req.options = std::move(options);

    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: render failed: " << res.error.message << "\n";
        return false;
    }
    return true;
}

mradm::RenderResult render_result_for(const std::filesystem::path& input,
                                      const std::filesystem::path& output,
                                      mradm::RenderOptions options) {
    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;

    mradm::RenderRequest req;
    req.input_path = input;
    req.output_path = output;
    req.options = std::move(options);
    return service.render(req, progress, logs);
}

bool verify_binaural_render_is_two_channel_and_directional() {
    const auto in = write_fixture(90.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{80}, 4096U);
    const auto out = temp_path("mr_binaural_directional", ".wav");
    FileGuard in_guard(in);
    FileGuard out_guard(out);

    auto options = make_request(in, out).options;
    if (!render_to_path(in, out, options)) {
        return false;
    }

    auto reader = mradm::audio::FloatWavReader::open(out.string());
    if (!check(reader.has_value(), "binaural WAV output opens")) {
        return false;
    }
    bool ok = true;
    ok &= check(reader->channels() == 2U, "binaural output has two channels");

    std::vector<float> samples(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());
    const double l_energy = channel_energy(samples, reader->channels(), 0U, 0U, reader->frame_count());
    const double r_energy = channel_energy(samples, reader->channels(), 1U, 0U, reader->frame_count());
    ok &= check((l_energy + r_energy) > 1e-5, "binaural output is not silent");
    ok &= check(std::abs(l_energy - r_energy) > 1e-7, "off-centre source produces L/R HRTF difference");
    return ok;
}

bool verify_binaural_time_gate_respects_block_start() {
    const auto in = write_fixture(0.0F, std::chrono::milliseconds{10}, std::chrono::milliseconds{30}, 4096U);
    const auto out = temp_path("mr_binaural_time_gate", ".wav");
    FileGuard in_guard(in);
    FileGuard out_guard(out);

    auto options = make_request(in, out).options;
    if (!render_to_path(in, out, options)) {
        return false;
    }

    auto reader = mradm::audio::FloatWavReader::open(out.string());
    if (!check(reader.has_value(), "time-gated binaural WAV output opens")) {
        return false;
    }
    std::vector<float> samples(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());

    bool ok = true;
    const double preroll = channel_energy(samples, reader->channels(), 0U, 0U, 400U) +
                           channel_energy(samples, reader->channels(), 1U, 0U, 400U);
    const double active = channel_energy(samples, reader->channels(), 0U, 800U, 1400U) +
                          channel_energy(samples, reader->channels(), 1U, 800U, 1400U);
    ok &= check(preroll < 1e-10, "binaural renderer does not leak before block start");
    ok &= check(active > 1e-5, "binaural renderer emits inside active block");
    return ok;
}

bool verify_binaural_lfe_bypasses_hrtf() {
    const auto in = write_lfe_fixture(std::chrono::milliseconds{0}, std::chrono::milliseconds{80}, 4096U);
    const auto out = temp_path("mr_binaural_lfe_bypass", ".wav");
    FileGuard in_guard(in);
    FileGuard out_guard(out);

    auto options = make_request(in, out).options;
    if (!render_to_path(in, out, options)) {
        return false;
    }

    auto reader = mradm::audio::FloatWavReader::open(out.string());
    if (!check(reader.has_value(), "LFE bypass binaural WAV output opens")) {
        return false;
    }
    std::vector<float> samples(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());

    bool ok = true;
    ok &= check(reader->channels() == 2U, "LFE bypass output has two channels");
    const double l_energy = channel_energy(samples, reader->channels(), 0U, 0U, reader->frame_count());
    const double r_energy = channel_energy(samples, reader->channels(), 1U, 0U, reader->frame_count());
    ok &= check(l_energy > 1e-5, "LFE bypass left output is not silent");
    ok &= check(std::abs(l_energy - r_energy) < (l_energy * 1.0e-6), "LFE bypass feeds both ears equally");
    return ok;
}

bool verify_binaural_caf_layout_is_strict() {
    const auto in = write_fixture(0.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{80}, 4096U);
    const auto out = temp_path("mr_binaural_layout", ".caf");
    const auto invalid_out = temp_path("mr_binaural_invalid_layout", ".caf");
    FileGuard in_guard(in);
    FileGuard out_guard(out);
    FileGuard invalid_out_guard(invalid_out);

    auto options = make_request(in, out).options;
    if (!render_to_path(in, out, options)) {
        return false;
    }

    auto reader = mradm::audio::FloatCafReader::open(out.string());
    if (!check(reader.has_value(), "binaural CAF output opens")) {
        return false;
    }
    bool ok = true;
    ok &= check(reader->channels() == 2U, "binaural CAF output has two channels");
    ok &= check(reader->sample_rate() == 48000U, "binaural CAF sample rate is preserved");
    ok &= check(read_caf_layout_tag(out) == ((106U << 16U) | 2U), "binaural CAF uses CoreAudio Binaural tag");

    options.output_layout = "wav71";
    const auto invalid = render_result_for(in, invalid_out, options);
    ok &= check(!invalid.success() && invalid.error.code == mradm::ErrorCode::unsupported,
                "binaural backend rejects a multichannel output layout");
    ok &= check(invalid.error.message.find("does not support output layout") != std::string::npos,
                "binaural layout mismatch reports a clear error");
    return ok;
}

bool verify_binaural_missing_sofa_fails_cleanly() {
    const auto in = write_fixture(0.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{80}, 4096U);
    const auto out = temp_path("mr_binaural_missing_sofa", ".wav");
    const auto missing_sofa = temp_path("mr_binaural_missing", ".sofa");
    FileGuard in_guard(in);
    FileGuard out_guard(out);

    auto options = make_request(in, out).options;
    options.sofa_path = missing_sofa;
    const auto res = render_result_for(in, out, options);

    bool ok = true;
    ok &= check(!res.success(), "missing SOFA path fails");
    ok &= check(res.error.code == mradm::ErrorCode::io_error || res.error.code == mradm::ErrorCode::unsupported,
                "missing SOFA error is explicit");
    return ok;
}

bool verify_binaural_channel_lock_changes_direction() {
    const auto free_in = write_fixture(0.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{80}, 4096U);
    const auto locked_in = write_fixture(ObjectFixtureOptions{.channel_lock = true}, 4096U);
    FileGuard free_guard(free_in);
    FileGuard locked_guard(locked_in);

    const auto free = render_stereo_samples(free_in, "mr_binaural_channel_lock_free");
    const auto locked = render_stereo_samples(locked_in, "mr_binaural_channel_lock_locked");
    if (!free || !locked) {
        return false;
    }

    const double free_l = channel_energy(*free, 2U, 0U, 0U, free->size() / 2U);
    const double free_r = channel_energy(*free, 2U, 1U, 0U, free->size() / 2U);
    const double locked_l = channel_energy(*locked, 2U, 0U, 0U, locked->size() / 2U);
    const double locked_r = channel_energy(*locked, 2U, 1U, 0U, locked->size() / 2U);

    bool ok = true;
    ok &= check((locked_l + locked_r) > 1e-5, "channelLock: locked binaural output is not silent");
    ok &= check(std::abs(locked_l - locked_r) > std::abs(free_l - free_r) * 2.0,
                "channelLock: front source locks to off-centre binaural reference");
    return ok;
}

bool verify_binaural_object_divergence_changes_output() {
    const auto point_in = write_fixture(0.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{80}, 4096U);
    const auto div_in = write_fixture(ObjectFixtureOptions{.divergence = 1.0F, .divergence_range = 60.0F}, 4096U);
    FileGuard point_guard(point_in);
    FileGuard div_guard(div_in);

    const auto point = render_stereo_samples(point_in, "mr_binaural_divergence_point");
    const auto div = render_stereo_samples(div_in, "mr_binaural_divergence_spread");
    if (!point || !div) {
        return false;
    }

    const double point_energy =
        channel_energy(*point, 2U, 0U, 0U, point->size() / 2U) + channel_energy(*point, 2U, 1U, 0U, point->size() / 2U);
    const double div_energy =
        channel_energy(*div, 2U, 0U, 0U, div->size() / 2U) + channel_energy(*div, 2U, 1U, 0U, div->size() / 2U);

    bool ok = true;
    ok &= check(div_energy > 1e-5, "objectDivergence: divergent binaural output is not silent");
    ok &= check(sample_difference_energy(*point, *div) > point_energy * 1.0e-3,
                "objectDivergence: divergent output differs from point source");
    return ok;
}

bool verify_binaural_extent_changes_output() {
    const auto point_in = write_fixture(0.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{80}, 4096U);
    const auto width_in = write_fixture(ObjectFixtureOptions{.width = 1.0F}, 4096U);
    const auto height_in = write_fixture(ObjectFixtureOptions{.height = 1.0F}, 4096U);
    const auto depth_in = write_fixture(ObjectFixtureOptions{.depth = 1.0F}, 4096U);
    FileGuard point_guard(point_in);
    FileGuard width_guard(width_in);
    FileGuard height_guard(height_in);
    FileGuard depth_guard(depth_in);

    const auto point = render_stereo_samples(point_in, "mr_binaural_extent_point");
    const auto width = render_stereo_samples(width_in, "mr_binaural_extent_width");
    const auto height = render_stereo_samples(height_in, "mr_binaural_extent_height");
    const auto depth = render_stereo_samples(depth_in, "mr_binaural_extent_depth");
    if (!point || !width || !height || !depth) {
        return false;
    }

    const double point_energy =
        channel_energy(*point, 2U, 0U, 0U, point->size() / 2U) + channel_energy(*point, 2U, 1U, 0U, point->size() / 2U);

    bool ok = true;
    ok &= check(sample_difference_energy(*point, *width) > point_energy * 1.0e-3,
                "binaural width: extent output differs from point source");
    ok &= check(sample_difference_energy(*point, *height) > point_energy * 1.0e-4,
                "binaural height: extent output differs from point source");
    ok &= check(sample_difference_energy(*point, *depth) > point_energy * 1.0e-4,
                "binaural depth: extent output differs from point source");
    return ok;
}

bool verify_binaural_saf_spreader_output() {
    constexpr uint32_t frames = 4096U;
    const auto width_in = write_fixture(ObjectFixtureOptions{.width = 1.0F}, frames);
    const auto point_in = write_fixture(ObjectFixtureOptions{}, frames);
    FileGuard width_guard(width_in);
    FileGuard point_guard(point_in);

    bool ok = true;

    // Width=1.0 rendered with the experimental saf_spreader mode.
    const auto spr_out = temp_path("mr_binaural_saf_spreader_width", ".wav");
    FileGuard spr_guard(spr_out);
    mradm::RenderOptions spr_opts;
    spr_opts.renderer = mradm::RendererSelection::binaural;
    spr_opts.peak_limit = false;
    spr_opts.measure_loudness = false;
    spr_opts.binaural_spread_mode = mradm::BinauralSpreadMode::saf_spreader;
    if (!render_to_path(width_in, spr_out, spr_opts)) {
        return false;
    }
    auto spr_reader = mradm::audio::FloatWavReader::open(spr_out.string());
    if (!check(spr_reader.has_value(), "saf_spreader output opens")) {
        return false;
    }
    // STFT delay compensation: the rendered file must be exactly as long as the
    // input ADM timeline (head warm-up skipped, tail drained, no extra frames).
    ok &= check(spr_reader->frame_count() == frames,
                "saf_spreader output length equals input length (delay compensated)");
    ok &= check(spr_reader->channels() == 2U, "saf_spreader output is stereo");
    std::vector<float> spr_samples(static_cast<std::size_t>(spr_reader->channels()) * spr_reader->frame_count());
    spr_reader->read(spr_samples.data(), spr_reader->frame_count());
    const double spr_energy = channel_energy(spr_samples, 2U, 0U, 0U, spr_reader->frame_count()) +
                              channel_energy(spr_samples, 2U, 1U, 0U, spr_reader->frame_count());
    ok &= check(spr_energy > 1e-5, "saf_spreader width output is not silent");

    // saf_spreader should produce a different rendering than the default 17-point cloud.
    const auto cloud = render_stereo_samples(width_in, "mr_binaural_saf_spreader_cloud_ref");
    if (!cloud) {
        return false;
    }
    ok &= check(sample_difference_energy(spr_samples, *cloud) > spr_energy * 1.0e-3,
                "saf_spreader output differs from cloud extent output");

    // A point source (no extent) carries no spreader track, so saf_spreader mode
    // falls back to the OLA path: output stays length-exact and non-silent.
    const auto pt_out = temp_path("mr_binaural_saf_spreader_point", ".wav");
    FileGuard pt_guard(pt_out);
    if (!render_to_path(point_in, pt_out, spr_opts)) {
        return false;
    }
    auto pt_reader = mradm::audio::FloatWavReader::open(pt_out.string());
    if (!check(pt_reader.has_value(), "saf_spreader point output opens")) {
        return false;
    }
    ok &= check(pt_reader->frame_count() == frames, "saf_spreader point-source output length equals input length");
    std::vector<float> pt_samples(static_cast<std::size_t>(pt_reader->channels()) * pt_reader->frame_count());
    pt_reader->read(pt_samples.data(), pt_reader->frame_count());
    const double pt_energy = channel_energy(pt_samples, 2U, 0U, 0U, pt_reader->frame_count()) +
                             channel_energy(pt_samples, 2U, 1U, 0U, pt_reader->frame_count());
    ok &= check(pt_energy > 1e-5, "saf_spreader point-source output is not silent");

    // Non-512-aligned length: the partial last STFT batch must still be flushed by
    // the tail, so the output stays exactly as long as the input (no truncation).
    constexpr uint32_t odd_frames = 4096U + 123U;
    const auto odd_in = write_fixture(ObjectFixtureOptions{.width = 1.0F}, odd_frames);
    FileGuard odd_guard(odd_in);
    const auto odd_out = temp_path("mr_binaural_saf_spreader_odd", ".wav");
    FileGuard odd_out_guard(odd_out);
    if (!render_to_path(odd_in, odd_out, spr_opts)) {
        return false;
    }
    auto odd_reader = mradm::audio::FloatWavReader::open(odd_out.string());
    if (!check(odd_reader.has_value(), "saf_spreader odd-length output opens")) {
        return false;
    }
    ok &= check(odd_reader->frame_count() == odd_frames,
                "saf_spreader non-512-aligned output length equals input length");
    std::vector<float> odd_samples(static_cast<std::size_t>(odd_reader->channels()) * odd_reader->frame_count());
    odd_reader->read(odd_samples.data(), odd_reader->frame_count());
    const double odd_energy = channel_energy(odd_samples, 2U, 0U, 0U, odd_reader->frame_count()) +
                              channel_energy(odd_samples, 2U, 1U, 0U, odd_reader->frame_count());
    ok &= check(odd_energy > 1e-5, "saf_spreader non-512-aligned output is not silent");
    return ok;
}

bool verify_binaural_diffuse_bus() {
    const auto direct_in = write_fixture(ObjectFixtureOptions{}, 4096U);
    const auto diffuse_in = write_fixture(ObjectFixtureOptions{.diffuse = 1.0F}, 4096U);
    const auto mixed_in = write_fixture(ObjectFixtureOptions{.diffuse = 0.5F}, 4096U);
    FileGuard direct_guard(direct_in);
    FileGuard diffuse_guard(diffuse_in);
    FileGuard mixed_guard(mixed_in);

    const auto direct = render_stereo_samples(direct_in, "mr_binaural_diffuse_direct");
    const auto diffuse = render_stereo_samples(diffuse_in, "mr_binaural_diffuse_full");
    const auto mixed = render_stereo_samples(mixed_in, "mr_binaural_diffuse_half");
    if (!direct || !diffuse || !mixed) {
        return false;
    }

    const double direct_energy = channel_energy(*direct, 2U, 0U, 0U, direct->size() / 2U) +
                                 channel_energy(*direct, 2U, 1U, 0U, direct->size() / 2U);
    const double diffuse_energy = channel_energy(*diffuse, 2U, 0U, 0U, diffuse->size() / 2U) +
                                  channel_energy(*diffuse, 2U, 1U, 0U, diffuse->size() / 2U);
    const double mixed_energy =
        channel_energy(*mixed, 2U, 0U, 0U, mixed->size() / 2U) + channel_energy(*mixed, 2U, 1U, 0U, mixed->size() / 2U);

    bool ok = true;
    ok &= check(diffuse_energy > direct_energy * 0.01, "binaural diffuse=1: diffuse bus is not silent");
    ok &= check(mixed_energy > direct_energy * 0.01, "binaural diffuse=0.5: mixed bus is not silent");
    ok &= check(sample_difference_energy(*direct, *diffuse) > direct_energy * 1.0e-3,
                "binaural diffuse=1: output differs from direct bus");
    return ok;
}

bool verify_binaural_mixed_divergence_keeps_center_slot() {
    constexpr std::size_t k_divergent_begin = 1920U;
    constexpr std::size_t k_divergent_end = 3840U;

    const auto reference_in = write_fixture(ObjectFixtureOptions{.divergence = 1.0F, .divergence_range = 60.0F}, 4096U);
    const auto mixed_in = write_mixed_divergence_fixture(4096U);
    FileGuard reference_guard(reference_in);
    FileGuard mixed_guard(mixed_in);

    const auto reference = render_stereo_samples(reference_in, "mr_binaural_mixed_divergence_reference");
    const auto mixed = render_stereo_samples(mixed_in, "mr_binaural_mixed_divergence_mixed");
    if (!reference || !mixed) {
        return false;
    }

    const double reference_l = channel_energy(*reference, 2U, 0U, k_divergent_begin, k_divergent_end);
    const double reference_r = channel_energy(*reference, 2U, 1U, k_divergent_begin, k_divergent_end);
    const double mixed_l = channel_energy(*mixed, 2U, 0U, k_divergent_begin, k_divergent_end);
    const double mixed_r = channel_energy(*mixed, 2U, 1U, k_divergent_begin, k_divergent_end);

    const double reference_delta = std::abs(reference_l - reference_r);
    const double mixed_delta = std::abs(mixed_l - mixed_r);
    const double mixed_energy = mixed_l + mixed_r;

    bool ok = true;
    ok &= check(mixed_energy > 1e-5, "mixed objectDivergence: divergent segment is not silent");
    ok &= check(mixed_delta < (reference_delta + (mixed_energy * 0.05)),
                "mixed objectDivergence: divergent segment keeps balanced ±60° slots");
    return ok;
}

bool verify_external_sofa_when_available() {
    const char* sofa_env = std::getenv("MR_ADM_TEST_SOFA_PATH");
    if (sofa_env == nullptr || std::string_view{sofa_env}.empty()) {
        return true;
    }

    const auto in = write_fixture(90.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{80}, 4096U);
    const auto out = temp_path("mr_binaural_external_sofa", ".wav");
    FileGuard in_guard(in);
    FileGuard out_guard(out);

    auto options = make_request(in, out).options;
    options.sofa_path = std::filesystem::path{sofa_env};
    if (!render_to_path(in, out, options)) {
        return false;
    }

    auto reader = mradm::audio::FloatWavReader::open(out.string());
    if (!check(reader.has_value(), "external SOFA binaural WAV output opens")) {
        return false;
    }
    std::vector<float> samples(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());
    const double l_energy = channel_energy(samples, reader->channels(), 0U, 0U, reader->frame_count());
    const double r_energy = channel_energy(samples, reader->channels(), 1U, 0U, reader->frame_count());

    bool ok = true;
    ok &= check(reader->channels() == 2U, "external SOFA output is stereo");
    ok &= check((l_energy + r_energy) > 1e-5, "external SOFA output is not silent");
    ok &= check(l_energy > r_energy, "left source favours left ear with external SOFA");
    return ok;
}

// Invariant 1: at a measurement grid point the magnitude/phase-split interpolation must reproduce
// the raw measured HRTF (a single VBAP gain of 1 → no interpolation). Guards against insertion of
// interpolation artefacts at the exact measured directions.
bool verify_binaural_grid_point_identity() {
    bool ok = true;
    bool tested_any_grid_point = false;
    // KEMAR's built-in grid includes the cardinal horizontal directions; sweep azimuth and pick the
    // ones that resolve to a single grid point.
    for (float az : {0.0F, 30.0F, 90.0F, 180.0F, -90.0F}) {
        for (int ear = 0; ear < 2; ++ear) {
            const auto p = mradm::binaural_detail::probe_hrtf_interpolation(az, 0.0F, ear);
            if (!check(p.valid, "grid-point probe builds KEMAR state")) {
                return false;
            }
            if (!p.grid_point) {
                continue;
            }
            tested_any_grid_point = true;
            double max_abs_db = 0.0;
            for (std::size_t b = 0; b < p.interp_mag.size(); ++b) {
                if (p.freqs[b] < 200.0F || p.freqs[b] >= 18000.0F) {
                    continue;
                }
                const double a = 20.0 * std::log10(static_cast<double>(p.interp_mag[b]) + 1e-9);
                const double r = 20.0 * std::log10(static_cast<double>(p.grid_raw_mag[b]) + 1e-9);
                max_abs_db = std::max(max_abs_db, std::abs(a - r));
            }
            ok &= check(max_abs_db < 0.01, "grid-point interpolation reproduces raw measured HRTF (<0.01 dB)");
        }
    }
    ok &= check(tested_any_grid_point, "at least one azimuth resolved to a KEMAR grid point");
    return ok;
}

// Invariant 2: off-grid the split interpolation must keep the magnitude inside the convex hull of
// the contributing measured directions (min_k|H| ≤ |interp| ≤ max_k|H|). This is the mathematical
// guarantee that it cannot fabricate a comb notch deeper than any neighbour, nor fill a notch that
// every neighbour shares — the core safety property versus naive complex weighting (which combs).
bool verify_binaural_offgrid_convex_magnitude() {
    bool ok = true;
    bool tested_any_offgrid = false;
    // Deliberately off-grid azimuth/elevation pairs, including lateral and elevated directions.
    const std::array<std::pair<float, float>, 5> dirs{
        {{37.0F, 10.0F}, {70.0F, 25.0F}, {-55.0F, 15.0F}, {120.0F, 0.0F}, {25.0F, 50.0F}}};
    for (const auto& [az, el] : dirs) {
        for (int ear = 0; ear < 2; ++ear) {
            const auto p = mradm::binaural_detail::probe_hrtf_interpolation(az, el, ear);
            if (!check(p.valid, "off-grid probe builds KEMAR state")) {
                return false;
            }
            if (p.grid_point) {
                continue;
            }
            tested_any_offgrid = true;
            // Allow a small numerical slack on the convex bound (float FFT / round-off).
            constexpr float k_slack = 1.0e-4F;
            bool within = true;
            for (std::size_t b = 0; b < p.interp_mag.size(); ++b) {
                if (p.freqs[b] < 200.0F || p.freqs[b] >= 18000.0F) {
                    continue;
                }
                const float lo = p.nbr_min_mag[b] - (k_slack * p.nbr_max_mag[b]) - k_slack;
                const float hi = p.nbr_max_mag[b] + (k_slack * p.nbr_max_mag[b]) + k_slack;
                if (p.interp_mag[b] < lo || p.interp_mag[b] > hi) {
                    within = false;
                }
            }
            ok &= check(within, "off-grid magnitude stays within the neighbour convex hull (no fabricated comb)");
        }
    }
    ok &= check(tested_any_offgrid, "at least one direction resolved to an off-grid query");
    return ok;
}

double probe_band_mean_db(const mradm::binaural_detail::HrtfInterpProbe& p, float lo, float hi) {
    double sum = 0.0;
    int n = 0;
    for (std::size_t b = 0; b < p.interp_mag.size(); ++b) {
        if (p.freqs[b] < lo || p.freqs[b] >= hi) {
            continue;
        }
        sum += 20.0 * std::log10(static_cast<double>(p.interp_mag[b]) + 1e-9);
        ++n;
    }
    return n > 0 ? sum / n : 0.0;
}

// Invariant 3: a concrete lateral regression guard (not a tautology). At lateral off-grid
// directions the far (contralateral) ear must stay head-shadowed in the high band — the split
// interpolation must NOT lift the far-ear magnitude up toward the near ear. We assert a minimum
// interaural level difference in 4–10 kHz; complex weighting that combed, or an over-aggressive
// notch-fill, would shrink this ILD below the floor.
bool verify_binaural_lateral_head_shadow() {
    bool ok = true;
    struct Lateral {
        float az;
        int near_ear;
        int far_ear;
    };
    // KEMAR convention (measured): az>0 → ear 0 is the near ear, ear 1 the shadowed far ear; az<0
    // mirrors. Measured 4–10 kHz ILD at these directions is ~19–25 dB; the 12 dB floor catches a
    // far-ear notch-fill / comb regression while leaving headroom for dataset/interpolation jitter.
    const std::array<Lateral, 3> dirs{{{100.0F, 0, 1}, {120.0F, 0, 1}, {-100.0F, 1, 0}}};
    for (const auto& d : dirs) {
        const auto near_p = mradm::binaural_detail::probe_hrtf_interpolation(d.az, 0.0F, d.near_ear);
        const auto far_p = mradm::binaural_detail::probe_hrtf_interpolation(d.az, 0.0F, d.far_ear);
        if (!check(near_p.valid && far_p.valid, "lateral probe builds KEMAR state")) {
            return false;
        }
        const double ild = probe_band_mean_db(near_p, 4000.0F, 10000.0F) - probe_band_mean_db(far_p, 4000.0F, 10000.0F);
        ok &= check(ild > 12.0, "lateral far ear stays head-shadowed (4-10kHz ILD > 12 dB)");
    }
    return ok;
}

// Drive BinauralStream over the whole timeline, pulling in the given (repeating) chunk
// pattern, optionally applying live overrides before the first pull. Returns interleaved
// stereo float PCM, or nullopt on error.
std::optional<std::vector<float>> render_binaural_stream(const std::filesystem::path& input,
                                                         const std::vector<std::size_t>& chunk_pattern,
                                                         const mradm::LiveOverrides* overrides,
                                                         const mradm::ListenerOrientation* orient = nullptr) {
    auto scene = mradm::io::import_scene(input.string());
    if (!check(scene.has_value(), "stream: import scene")) {
        return std::nullopt;
    }
    mradm::RenderPlan plan;
    plan.input_path = input.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_binaural_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "stream: binaural prepare")) {
        return std::nullopt;
    }
    auto stream = renderer->open_stream(**prepared, plan, logs);
    if (!check(stream.has_value(), "stream: open_stream succeeds")) {
        return std::nullopt;
    }
    if (overrides != nullptr) {
        (*stream)->set_overrides(*overrides);
    }
    if (orient != nullptr) {
        (*stream)->set_listener_orientation(*orient);
    }

    std::vector<float> out;
    std::vector<float> buf;
    std::size_t pi = 0;
    while (true) {
        const std::size_t frames = chunk_pattern[pi % chunk_pattern.size()];
        ++pi;
        buf.assign(frames * 2U, 0.0F);
        auto produced = (*stream)->process(std::span<float>(buf), frames);
        if (!check(produced.has_value(), "stream: process succeeds")) {
            return std::nullopt;
        }
        if (*produced == 0) {
            break;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*produced * 2U));
    }
    return out;
}

std::optional<std::vector<float>>
render_binaural_window(const std::filesystem::path& input,
                       const mradm::ListenerOrientation& orientation,
                       mradm::BinauralSpreadMode spread_mode = mradm::BinauralSpreadMode::automatic) {
    auto scene = mradm::io::import_scene(input.string());
    if (!check(scene.has_value(), "headtrack-window: import scene")) {
        return std::nullopt;
    }

    const auto output = temp_path("mr_binaural_headtrack_window", ".wav");
    FileGuard output_guard(output);
    mradm::RenderPlan plan;
    plan.input_path = input.string();
    plan.output_path = output.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;
    plan.listener_orientation = orientation;
    plan.binaural_spread_mode = spread_mode;

    auto renderer = mradm::create_binaural_renderer();
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "headtrack-window: prepare")) {
        return std::nullopt;
    }
    if (!check(renderer->render_window(**prepared, plan, progress, logs).has_value(),
               "headtrack-window: render_window")) {
        return std::nullopt;
    }

    auto reader = mradm::audio::FloatWavReader::open(output.string());
    if (!check(reader.has_value() && reader->channels() == 2U, "headtrack-window: stereo WAV opens")) {
        return std::nullopt;
    }
    std::vector<float> samples(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());
    return samples;
}

std::optional<std::vector<float>> render_binaural_stream_with_mid_override(const std::filesystem::path& input,
                                                                           std::size_t boundary_frames,
                                                                           const mradm::LiveOverrides& overrides) {
    auto scene = mradm::io::import_scene(input.string());
    if (!check(scene.has_value(), "mid-override: import scene")) {
        return std::nullopt;
    }
    mradm::RenderPlan plan;
    plan.input_path = input.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_binaural_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    auto stream = prepared.has_value() ? renderer->open_stream(**prepared, plan, logs)
                                       : decltype(renderer->open_stream(**prepared, plan, logs)){};
    if (!check(prepared.has_value() && stream.has_value(), "mid-override: prepare + open_stream")) {
        return std::nullopt;
    }

    std::vector<float> out;
    std::vector<float> buf(boundary_frames * 2U, 0.0F);
    auto first = (*stream)->process(std::span<float>(buf), boundary_frames);
    if (!check(first.has_value() && *first == boundary_frames, "mid-override: render pre-edit prefix")) {
        return std::nullopt;
    }
    out.insert(out.end(), buf.begin(), buf.end());
    (*stream)->set_overrides(overrides);

    while (true) {
        buf.assign(std::size_t{1024U} * 2U, 0.0F);
        auto produced = (*stream)->process(std::span<float>(buf), 1024U);
        if (!check(produced.has_value(), "mid-override: process succeeds")) {
            return std::nullopt;
        }
        if (*produced == 0U) {
            break;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*produced * 2U));
    }
    return out;
}

// BinauralStream (realtime) reproduces the offline render_window render and is independent
// of the caller's pull chunk size (canonical block + FIFO).
bool verify_binaural_stream_matches_window() {
    const auto in = write_fixture(30.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{120}, 8192U);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value(), "stream: import scene for reference")) {
        return false;
    }
    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_binaural_renderer();
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "stream: reference prepare")) {
        return false;
    }

    // Reference: the offline render_window path to a float WAV.
    const auto ref_path = temp_path("mr_binaural_stream_ref", ".wav");
    FileGuard ref_guard(ref_path);
    mradm::RenderPlan window_plan = plan;
    window_plan.output_path = ref_path.string();
    if (!check(renderer->render_window(**prepared, window_plan, progress, logs).has_value(),
               "stream: reference render_window")) {
        return false;
    }
    auto reader = mradm::audio::FloatWavReader::open(ref_path.string());
    if (!check(reader.has_value() && reader->channels() == 2U, "stream: reference WAV opens (2ch)")) {
        return false;
    }
    std::vector<float> ref(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(ref.data(), reader->frame_count());

    const auto uniform = render_binaural_stream(in, {1024}, nullptr);
    const auto varied = render_binaural_stream(in, {333, 1000, 512, 7}, nullptr);
    if (!uniform || !varied) {
        return false;
    }

    bool ok = true;
    ok &= check(uniform->size() == ref.size(), "stream output frame count matches render_window");
    ok &= check(*uniform == *varied, "stream output is identical regardless of pull chunk size (FIFO correct)");

    double max_diff = 0.0;
    const std::size_t n = std::min(uniform->size(), ref.size());
    for (std::size_t i = 0; i < n; ++i) {
        max_diff = std::max(max_diff, std::fabs(static_cast<double>((*uniform)[i]) - static_cast<double>(ref[i])));
    }
    // Tolerance accounts only for the reference WAV's integer-PCM round-trip; the stream
    // shares render_window's exact float math (chunk invariance above is the hard guarantee).
    ok &= check(max_diff < 1.0e-4, "stream output matches the offline render_window render");
    return ok;
}

// A live gain override scales the object's output by the override factor (linear gain
// commutes with the HRTF convolution).
bool verify_binaural_stream_gain_override() {
    const auto in = write_fixture(30.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{120}, 8192U);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value() && !scene->objects.empty(), "override: import scene with an object")) {
        return false;
    }
    const std::string object_id = scene->objects.front().id;

    const auto baseline = render_binaural_stream(in, {1024}, nullptr);
    mradm::LiveOverrides ov;
    ov.revision = 1;
    ov.objects.push_back(
        {object_id, -12.041F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, "", std::nullopt, false}); // ≈ 0.25 linear
    const auto attenuated = render_binaural_stream(in, {1024}, &ov);
    if (!baseline || !attenuated) {
        return false;
    }

    auto buffer_rms = [](const std::vector<float>& v) {
        const double total = std::accumulate(v.begin(), v.end(), 0.0, [](double acc, float s) {
            return acc + (static_cast<double>(s) * static_cast<double>(s));
        });
        return v.empty() ? 0.0 : std::sqrt(total / static_cast<double>(v.size()));
    };
    const double b = buffer_rms(*baseline);
    const double a = buffer_rms(*attenuated);

    bool ok = check(attenuated->size() == baseline->size(), "override: same frame count as baseline");
    ok &= check(b > 1.0e-3, "override: baseline has signal energy");
    // Pure linear gain: ratio is exactly the scalar (0.25). Tight bounds either side.
    ok &= check(a < b * 0.30 && a > b * 0.20, "override: -12 dB override scales output by ~0.25");
    return ok;
}

bool verify_binaural_stream_live_gain_ramp() {
    constexpr std::size_t k_boundary = 1024U;
    constexpr std::size_t k_ramp_frames = 960U; // LiveGainRamp default: 20 ms at 48 kHz
    const auto in = write_fixture(ObjectFixtureOptions{.duration = std::chrono::milliseconds{400}}, 16384U);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value() && !scene->objects.empty(), "gain-ramp: import scene")) {
        return false;
    }
    const auto baseline = render_binaural_stream(in, {1024}, nullptr);
    mradm::LiveOverrides ov;
    ov.revision = 1;
    ov.objects.push_back(
        {scene->objects.front().id, -20.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, "", std::nullopt, false});
    const auto changed = render_binaural_stream_with_mid_override(in, k_boundary, ov);
    if (!baseline || !changed || !check(changed->size() == baseline->size(), "gain-ramp: frame count unchanged")) {
        return false;
    }

    bool ok = true;
    double ramp_error = 0.0;
    constexpr float k_target = 0.1F;
    for (std::size_t offset = 0; offset < k_ramp_frames; ++offset) {
        const float mix = static_cast<float>(offset) / static_cast<float>(k_ramp_frames - 1U);
        const float gain = 1.0F + ((k_target - 1.0F) * mix);
        for (std::size_t channel = 0; channel < 2U; ++channel) {
            const std::size_t index = ((k_boundary + offset) * 2U) + channel;
            ramp_error = std::max(
                ramp_error,
                std::fabs(static_cast<double>((*changed)[index]) - (static_cast<double>((*baseline)[index]) * gain)));
        }
    }
    ok &= check(ramp_error < 2.0e-5, "gain-ramp: live gain follows a sample-continuous 20 ms ramp");

    double settled_error = 0.0;
    for (std::size_t frame = k_boundary + k_ramp_frames; frame < k_boundary + k_ramp_frames + 1024U; ++frame) {
        for (std::size_t channel = 0; channel < 2U; ++channel) {
            const std::size_t index = (frame * 2U) + channel;
            settled_error = std::max(settled_error,
                                     std::fabs(static_cast<double>((*changed)[index]) -
                                               (static_cast<double>((*baseline)[index]) * k_target)));
        }
    }
    ok &= check(settled_error < 2.0e-5, "gain-ramp: reaches the exact target after the ramp");
    return ok;
}

bool verify_binaural_stream_topology_crossfade() {
    constexpr std::size_t k_boundary = 1024U;
    const auto in =
        write_fixture(ObjectFixtureOptions{.duration = std::chrono::milliseconds{400}, .width = 1.0F}, 16384U);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value() && !scene->objects.empty(), "topology-xfade: import scene")) {
        return false;
    }
    const auto baseline = render_binaural_stream(in, {1024}, nullptr);
    mradm::LiveOverrides ov;
    ov.revision = 1;
    ov.objects.push_back(
        {scene->objects.front().id, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F, "", std::nullopt, false});
    const auto changed = render_binaural_stream_with_mid_override(in, k_boundary, ov);
    if (!baseline || !changed || !check(changed->size() == baseline->size(), "topology-xfade: frame count unchanged")) {
        return false;
    }

    bool ok = true;
    for (std::size_t channel = 0; channel < 2U; ++channel) {
        const std::size_t index = (k_boundary * 2U) + channel;
        ok &= check(std::fabs((*changed)[index] - (*baseline)[index]) < 1.0e-6F,
                    "topology-xfade: transition begins on the intact old DSP state");
    }
    const std::size_t tail_begin = (k_boundary + 2048U) * 2U;
    double changed_energy = 0.0;
    for (std::size_t index = tail_begin; index < changed->size(); ++index) {
        const double delta = static_cast<double>((*changed)[index]) - static_cast<double>((*baseline)[index]);
        changed_energy += delta * delta;
    }
    ok &= check(changed_energy > 1.0e-5, "topology-xfade: extent target takes effect after the crossfade");
    return ok;
}

// A topology override (extent / diffuse / divergence scale) rebuilds the stream's source
// list (cheap re-prepare, HRTF reused); reverting the scales to 1.0 reproduces the
// unscaled render bit-for-bit.
bool verify_binaural_stream_topology_reprepare() {
    const auto in = write_fixture(ObjectFixtureOptions{.width = 1.0F}, 8192U);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value() && !scene->objects.empty(), "reprepare: import scene with an object")) {
        return false;
    }
    const std::string object_id = scene->objects.front().id;

    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_binaural_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "reprepare: prepare")) {
        return false;
    }

    auto run = [&](const std::vector<mradm::LiveOverrides>& seq) -> std::optional<std::vector<float>> {
        auto stream = renderer->open_stream(**prepared, plan, logs);
        if (!check(stream.has_value(), "reprepare: open_stream")) {
            return std::nullopt;
        }
        for (const auto& ov : seq) {
            (*stream)->set_overrides(ov);
        }
        std::vector<float> out;
        std::vector<float> buf;
        while (true) {
            buf.assign(std::size_t{1024U} * 2U, 0.0F);
            auto produced = (*stream)->process(std::span<float>(buf), 1024U);
            if (!produced) {
                return std::nullopt;
            }
            if (*produced == 0) {
                break;
            }
            out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*produced * 2U));
        }
        return out;
    };

    auto extent_override = [&](float scale) {
        mradm::LiveOverrides o;
        o.revision = 1;
        o.objects.push_back(
            {object_id, 0.0F, 1.0F, scale, 1.0F, 1.0F, 1.0F, 1.0F, "", std::nullopt, false}); // extent_scale
        return o;
    };
    auto extent_width_override = [&](float scale) {
        mradm::LiveOverrides o;
        o.revision = 1;
        o.objects.push_back({object_id, 0.0F, 1.0F, 1.0F, 1.0F, scale, 1.0F, 1.0F, "", std::nullopt, false});
        return o;
    };

    const auto baseline = run({});                                             // width = 1.0 (wide cloud)
    const auto pointed = run({extent_override(0.0F)});                         // extent_scale 0 → collapses to a point
    const auto width_pointed = run({extent_width_override(0.0F)});             // width_scale 0 also collapses width
    const auto reverted = run({extent_override(0.0F), extent_override(1.0F)}); // back to unscaled
    if (!baseline || !pointed || !width_pointed || !reverted) {
        return false;
    }

    bool ok = check(baseline->size() == pointed->size() && baseline->size() == width_pointed->size() &&
                        baseline->size() == reverted->size(),
                    "reprepare: all runs produce the same frame count");
    ok &= check(sample_difference_energy(*baseline, *pointed) > 1.0e-6, "reprepare: extent scale changes the output");
    ok &= check(sample_difference_energy(*baseline, *width_pointed) > 1.0e-6,
                "reprepare: extent width scale changes the output");
    ok &= check(*reverted == *baseline, "reprepare: reverting scales to 1.0 reproduces the unscaled render bit-exact");
    return ok;
}

// A listener head orientation rotates scene-relative sources into the head frame before HRTF lookup:
// turning left (yaw +90°) swings a front source onto the right ear. ADM headLocked and explicit live
// overrides exempt head-relative sources, while a gain-only live override must continue to inherit ADM.
bool verify_binaural_head_tracking() {
    const auto in = write_fixture(0.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{120}, 8192U);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value() && !scene->objects.empty(), "headtrack: import scene with an object")) {
        return false;
    }
    const std::string object_id = scene->objects.front().id;

    const auto lr = [](const std::vector<float>& s) {
        return std::pair<double, double>{channel_energy(s, 2U, 0U, 0U, s.size() / 2U),
                                         channel_energy(s, 2U, 1U, 0U, s.size() / 2U)};
    };

    const auto baseline = render_binaural_stream(in, {1024}, nullptr); // identity pose
    const mradm::ListenerOrientation yaw_left{90.0F, 0.0F, 0.0F};
    const auto turned = render_binaural_stream(in, {1024}, nullptr, &yaw_left);
    if (!baseline || !turned) {
        return false;
    }

    bool ok = true;
    const auto [b_l, b_r] = lr(*baseline);
    const auto [t_l, t_r] = lr(*turned);
    // A front (az=0) source is ~symmetric at rest; turning the head left swings it to the listener's
    // right, so the right ear must clearly dominate once tracking is applied.
    ok &= check(b_l > 1.0e-3 && std::fabs(b_l - b_r) < b_l * 0.5, "headtrack: front source ~balanced at rest");
    ok &= check(t_r > t_l * 2.0, "headtrack: yaw-left moves a front source onto the right ear");

    // An explicit live true remains supported for source material with no ADM headLocked value.
    mradm::LiveOverrides live_locked;
    live_locked.revision = 1;
    mradm::LiveObjectOverride live_locked_object;
    live_locked_object.object_id = object_id;
    live_locked_object.head_locked = true;
    live_locked.objects.push_back(live_locked_object);
    const auto live_locked_turned = render_binaural_stream(in, {1024}, &live_locked, &yaw_left);
    if (!live_locked_turned) {
        return false;
    }
    const auto [live_k_l, live_k_r] = lr(*live_locked_turned);
    ok &=
        check(std::fabs(live_k_l - live_k_r) < live_k_l * 0.5, "headtrack: explicit live true is exempt from tracking");

    ObjectFixtureOptions adm_locked_options;
    adm_locked_options.duration = std::chrono::milliseconds{120};
    adm_locked_options.object_head_locked = true;
    const auto adm_locked_in = write_fixture(adm_locked_options, 8192U);
    FileGuard adm_locked_guard(adm_locked_in);
    auto adm_locked_scene = mradm::io::import_scene(adm_locked_in.string());
    if (!check(adm_locked_scene.has_value() && !adm_locked_scene->objects.empty(),
               "headtrack: import ADM head-locked object")) {
        return false;
    }
    const std::string adm_locked_id = adm_locked_scene->objects.front().id;

    const auto adm_locked_turned = render_binaural_stream(adm_locked_in, {1024}, nullptr, &yaw_left);
    mradm::LiveOverrides gain_only;
    gain_only.revision = 2;
    mradm::LiveObjectOverride gain_only_object;
    gain_only_object.object_id = adm_locked_id;
    gain_only_object.gain_db = -3.0F;
    gain_only.objects.push_back(gain_only_object);
    const auto gain_only_turned = render_binaural_stream(adm_locked_in, {1024}, &gain_only, &yaw_left);

    mradm::LiveOverrides live_unlocked;
    live_unlocked.revision = 3;
    mradm::LiveObjectOverride live_unlocked_object;
    live_unlocked_object.object_id = adm_locked_id;
    live_unlocked_object.head_locked = false;
    live_unlocked.objects.push_back(live_unlocked_object);
    const auto live_unlocked_turned = render_binaural_stream(adm_locked_in, {1024}, &live_unlocked, &yaw_left);
    const auto offline_locked = render_binaural_window(adm_locked_in, yaw_left);
    if (!adm_locked_turned || !gain_only_turned || !live_unlocked_turned || !offline_locked) {
        return false;
    }
    const auto [adm_k_l, adm_k_r] = lr(*adm_locked_turned);
    const auto [gain_k_l, gain_k_r] = lr(*gain_only_turned);
    const auto [live_u_l, live_u_r] = lr(*live_unlocked_turned);
    const auto [offline_k_l, offline_k_r] = lr(*offline_locked);
    ok &= check(std::fabs(adm_k_l - adm_k_r) < adm_k_l * 0.5,
                "headtrack: source ADM headLocked is used by realtime rendering");
    ok &= check(std::fabs(gain_k_l - gain_k_r) < gain_k_l * 0.5,
                "headtrack: gain-only live override inherits ADM headLocked");
    ok &= check(live_u_r > live_u_l * 2.0, "headtrack: explicit live false overrides ADM headLocked");
    ok &= check(std::fabs(offline_k_l - offline_k_r) < offline_k_l * 0.5,
                "headtrack: source ADM headLocked is used by offline rendering");

    // A block-level explicit false must override an AudioObject-level true all the way through rendering.
    ObjectFixtureOptions block_unlocked_options = adm_locked_options;
    block_unlocked_options.block_head_locked = false;
    const auto block_unlocked_in = write_fixture(block_unlocked_options, 8192U);
    FileGuard block_unlocked_guard(block_unlocked_in);
    const auto block_unlocked = render_binaural_stream(block_unlocked_in, {1024}, nullptr, &yaw_left);
    if (!block_unlocked) {
        return false;
    }
    const auto [block_u_l, block_u_r] = lr(*block_unlocked);
    ok &= check(block_u_r > block_u_l * 2.0,
                "headtrack: explicit block false overrides AudioObject true during rendering");

    // Exercise the cloud and diffuse OLA branches with inherited ADM headLocked.
    ObjectFixtureOptions cloud_options = adm_locked_options;
    cloud_options.width = 0.35F;
    cloud_options.diffuse = 0.35F;
    const auto cloud_in = write_fixture(cloud_options, 8192U);
    FileGuard cloud_guard(cloud_in);
    const auto cloud_locked = render_binaural_stream(cloud_in, {1024}, nullptr, &yaw_left);
    if (!cloud_locked) {
        return false;
    }
    const auto [cloud_l, cloud_r] = lr(*cloud_locked);
    ok &= check(std::fabs(cloud_l - cloud_r) < cloud_l * 0.7,
                "headtrack: cloud and diffuse branches inherit ADM headLocked");

    // The SAF spreader is offline-only, but must use the same active-block state.
    ObjectFixtureOptions spreader_options = adm_locked_options;
    spreader_options.width = 0.35F;
    const auto spreader_in = write_fixture(spreader_options, 8192U);
    FileGuard spreader_guard(spreader_in);
    const auto spreader_locked = render_binaural_window(spreader_in, yaw_left, mradm::BinauralSpreadMode::saf_spreader);
    if (!spreader_locked) {
        return false;
    }
    const auto [spreader_l, spreader_r] = lr(*spreader_locked);
    ok &= check(std::fabs(spreader_l - spreader_r) < spreader_l * 0.7,
                "headtrack: SAF spreader inherits active-block ADM headLocked");
    return ok;
}

bool verify_binaural_head_locked_timeline() {
    constexpr std::size_t k_frames = 19200U;
    const auto in = write_head_locked_timeline_fixture(k_frames);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value() && scene->objects.size() == 1U && scene->objects.front().tracks.size() == 1U &&
                   scene->objects.front().tracks.front().blocks.size() == 2U,
               "headtrack-timeline: import two Objects blocks")) {
        return false;
    }
    const auto& blocks = scene->objects.front().tracks.front().blocks;
    bool ok = true;
    ok &= check(!blocks[0].head_locked && blocks[1].head_locked,
                "headtrack-timeline: imported blocks keep explicit false/true");

    const mradm::ListenerOrientation yaw_left{90.0F, 0.0F, 0.0F};
    const auto realtime = render_binaural_stream(in, {333, 1024, 511}, nullptr, &yaw_left);
    const auto offline = render_binaural_window(in, yaw_left);
    if (!realtime || !offline || realtime->size() < k_frames * 2U || offline->size() < k_frames * 2U) {
        return false;
    }

    const auto verify_windows =
        [&](const std::vector<float>& samples, const char* realtime_msg, const char* locked_msg) {
            constexpr std::size_t first_begin = 3072U;
            constexpr std::size_t first_end = 8192U;
            constexpr std::size_t second_begin = 11264U;
            constexpr std::size_t second_end = 17408U;
            const double first_l = channel_energy(samples, 2U, 0U, first_begin, first_end);
            const double first_r = channel_energy(samples, 2U, 1U, first_begin, first_end);
            const double second_l = channel_energy(samples, 2U, 0U, second_begin, second_end);
            const double second_r = channel_energy(samples, 2U, 1U, second_begin, second_end);
            bool windows_ok = true;
            windows_ok &= check(first_r > first_l * 2.0, realtime_msg);
            windows_ok &= check(std::fabs(second_l - second_r) < second_l * 0.5, locked_msg);
            return windows_ok;
        };
    ok &= verify_windows(*realtime,
                         "headtrack-timeline: realtime explicit-false block follows listener rotation",
                         "headtrack-timeline: realtime explicit-true block stays head-relative");
    ok &= verify_windows(*offline,
                         "headtrack-timeline: offline explicit-false block follows listener rotation",
                         "headtrack-timeline: offline explicit-true block stays head-relative");
    return ok;
}

// Sweeping the listener head while monitoring (orientation changed every pull) must track
// continuously without blowing up: a front source's balance migrates from centred to the right ear
// as yaw turns left, and the kernel-crossfade keeps the output finite and bounded (no zipper spike).
bool verify_binaural_head_tracking_dynamic() {
    const auto in = write_fixture(0.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{400}, 19200U);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value() && !scene->objects.empty(), "headtrack-dyn: import scene")) {
        return false;
    }
    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_binaural_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    auto stream = prepared.has_value() ? renderer->open_stream(**prepared, plan, logs)
                                       : decltype(renderer->open_stream(**prepared, plan, logs)){};
    if (!check(prepared.has_value() && stream.has_value(), "headtrack-dyn: prepare + open_stream")) {
        return false;
    }

    constexpr std::size_t chunk = 512U;
    constexpr float total = 19200.0F;
    std::vector<float> out;
    std::vector<float> buf;
    std::uint64_t produced_total = 0;
    while (true) {
        // Yaw sweeps 0 → +90° (turning the head left) across the stream, updated every pull.
        const float yaw = std::min(90.0F, 90.0F * (static_cast<float>(produced_total) / total));
        const mradm::ListenerOrientation o{yaw, 0.0F, 0.0F};
        (*stream)->set_listener_orientation(o);
        buf.assign(chunk * 2U, 0.0F);
        auto produced = (*stream)->process(std::span<float>(buf), chunk);
        if (!check(produced.has_value(), "headtrack-dyn: process")) {
            return false;
        }
        if (*produced == 0) {
            break;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*produced * 2U));
        produced_total += *produced;
    }

    bool ok = check(!out.empty(), "headtrack-dyn: produced output");
    float peak = 0.0F;
    bool finite = true;
    for (const float s : out) {
        finite = finite && std::isfinite(s);
        peak = std::max(peak, std::fabs(s));
    }
    ok &= check(finite, "headtrack-dyn: output stays finite while sweeping");
    ok &= check(peak < 8.0F, "headtrack-dyn: output stays bounded (no zipper blow-up)");

    const std::size_t frames = out.size() / 2U;
    const auto win = frames / 5U; // first/last 20%
    const double early_l = channel_energy(out, 2U, 0U, 0U, win);
    const double early_r = channel_energy(out, 2U, 1U, 0U, win);
    const double late_l = channel_energy(out, 2U, 0U, frames - win, frames);
    const double late_r = channel_energy(out, 2U, 1U, frames - win, frames);
    ok &= check(std::fabs(early_l - early_r) < early_l * 0.6, "headtrack-dyn: starts ~centred (yaw≈0)");
    ok &= check(late_r > late_l * 1.5, "headtrack-dyn: ends biased to the right ear (yaw→left)");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_binaural_grid_point_identity();
    ok &= verify_binaural_offgrid_convex_magnitude();
    ok &= verify_binaural_lateral_head_shadow();
    ok &= verify_binaural_render_is_two_channel_and_directional();
    ok &= verify_binaural_time_gate_respects_block_start();
    ok &= verify_binaural_lfe_bypasses_hrtf();
    ok &= verify_binaural_caf_layout_is_strict();
    ok &= verify_binaural_missing_sofa_fails_cleanly();
    ok &= verify_binaural_channel_lock_changes_direction();
    ok &= verify_binaural_object_divergence_changes_output();
    ok &= verify_binaural_extent_changes_output();
    ok &= verify_binaural_saf_spreader_output();
    ok &= verify_binaural_diffuse_bus();
    ok &= verify_binaural_mixed_divergence_keeps_center_slot();
    ok &= verify_external_sofa_when_available();
    ok &= verify_binaural_stream_matches_window();
    ok &= verify_binaural_stream_gain_override();
    ok &= verify_binaural_stream_live_gain_ramp();
    ok &= verify_binaural_stream_topology_crossfade();
    ok &= verify_binaural_stream_topology_reprepare();
    ok &= verify_binaural_head_tracking();
    ok &= verify_binaural_head_locked_timeline();
    ok &= verify_binaural_head_tracking_dynamic();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
