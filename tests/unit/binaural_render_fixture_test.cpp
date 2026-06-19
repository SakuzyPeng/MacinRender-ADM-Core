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
#include "adm/io.h"
#include "adm/render.h"
#include "adm/render_binaural.h"

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
    const auto name = std::string(stem) + "_" + std::to_string(static_cast<int>(::getpid())) + "_" +
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
    req.options.renderer = mradm::RendererSelection::binaural;
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

bool verify_binaural_render_is_stereo_and_directional() {
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
    ok &= check(reader->channels() == 2U, "binaural output is stereo");

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
    ok &= check(reader->channels() == 2U, "LFE bypass output is stereo");
    const double l_energy = channel_energy(samples, reader->channels(), 0U, 0U, reader->frame_count());
    const double r_energy = channel_energy(samples, reader->channels(), 1U, 0U, reader->frame_count());
    ok &= check(l_energy > 1e-5, "LFE bypass left output is not silent");
    ok &= check(std::abs(l_energy - r_energy) < (l_energy * 1.0e-6), "LFE bypass feeds both ears equally");
    return ok;
}

bool verify_binaural_caf_ignores_requested_layout() {
    const auto in = write_fixture(0.0F, std::chrono::milliseconds{0}, std::chrono::milliseconds{80}, 4096U);
    const auto out = temp_path("mr_binaural_layout", ".caf");
    FileGuard in_guard(in);
    FileGuard out_guard(out);

    auto options = make_request(in, out).options;
    options.output_layout = "wav71";
    if (!render_to_path(in, out, options)) {
        return false;
    }

    auto reader = mradm::audio::FloatCafReader::open(out.string());
    if (!check(reader.has_value(), "binaural CAF output opens")) {
        return false;
    }
    bool ok = true;
    ok &= check(reader->channels() == 2U, "binaural CAF output is stereo despite requested wav71");
    ok &= check(reader->sample_rate() == 48000U, "binaural CAF sample rate is preserved");
    ok &= check(read_caf_layout_tag(out) == ((106U << 16U) | 2U), "binaural CAF uses CoreAudio Binaural tag");
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

// Drive BinauralStream over the whole timeline, pulling in the given (repeating) chunk
// pattern, optionally applying live overrides before the first pull. Returns interleaved
// stereo float PCM, or nullopt on error.
std::optional<std::vector<float>> render_binaural_stream(const std::filesystem::path& input,
                                                         const std::vector<std::size_t>& chunk_pattern,
                                                         const mradm::LiveOverrides* overrides) {
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
    ov.objects.push_back({object_id, -12.041F, 1.0F, 1.0F, 1.0F}); // ≈ 0.25 linear
    const auto attenuated = render_binaural_stream(in, {1024}, &ov);
    if (!baseline || !attenuated) {
        return false;
    }

    auto buffer_rms = [](const std::vector<float>& v) {
        double total = 0.0;
        for (const float s : v) {
            total += static_cast<double>(s) * static_cast<double>(s);
        }
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

} // namespace

int main() {
    bool ok = true;
    ok &= verify_binaural_render_is_stereo_and_directional();
    ok &= verify_binaural_time_gate_respects_block_start();
    ok &= verify_binaural_lfe_bypasses_hrtf();
    ok &= verify_binaural_caf_ignores_requested_layout();
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
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
