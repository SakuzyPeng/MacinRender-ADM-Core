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

std::pair<std::shared_ptr<adm::Document>, std::string>
make_objects_doc(float azimuth, std::chrono::milliseconds rtime, std::chrono::milliseconds duration) {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"BinauralCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{azimuth}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        block.set(adm::Rtime{adm::Time{rtime}});
        block.set(adm::Duration{adm::Time{duration}});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
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

std::filesystem::path
write_fixture(float azimuth, std::chrono::milliseconds rtime, std::chrono::milliseconds duration, uint32_t frames) {
    constexpr uint32_t k_ch = 1U;
    constexpr uint32_t k_sr = 48000U;

    const auto [doc, uid_str] = make_objects_doc(azimuth, rtime, duration);
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

double
channel_energy(const std::vector<float>& samples, uint32_t channels, uint32_t ch, std::size_t begin, std::size_t end) {
    double e = 0.0;
    for (std::size_t f = begin; f < end; ++f) {
        const double v = samples[(f * channels) + ch];
        e += v * v;
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

} // namespace

int main() {
    bool ok = true;
    ok &= verify_binaural_render_is_stereo_and_directional();
    ok &= verify_binaural_time_gate_respects_block_start();
    ok &= verify_binaural_caf_ignores_requested_layout();
    ok &= verify_binaural_missing_sofa_fails_cleanly();
    ok &= verify_external_sofa_when_available();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
