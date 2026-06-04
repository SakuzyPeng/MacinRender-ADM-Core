#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

#include "render_common.h"

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

// Single OBJECTS object at a fixed azimuth (ADM convention: +ve = left), linear gain,
// optional ADM width (0..1) for extent spreading, and optional channelLock.
std::pair<std::shared_ptr<adm::Document>, std::string>
make_object_doc(float azimuth, float gain, float width, bool channel_lock) {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"AppleCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{azimuth}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{gain});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        if (width > 0.0F) {
            block.set(adm::Width{width});
        }
        if (channel_lock) {
            adm::ChannelLock lock;
            lock.set(adm::ChannelLockFlag{true});
            block.set(lock);
        }
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

std::filesystem::path write_fixture(float azimuth, uint32_t frames, float gain, float width, bool channel_lock) {
    constexpr uint32_t k_ch = 1U;
    constexpr uint32_t k_sr = 48000U;
    const auto [doc, uid_str] = make_object_doc(azimuth, gain, width, channel_lock);
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

std::optional<std::vector<float>> render_apple(float azimuth,
                                               std::string_view stem,
                                               const std::string& layout,
                                               uint16_t expected_channels,
                                               float gain = 1.0F,
                                               float width = 0.0F,
                                               bool channel_lock = false) {
    const auto in = write_fixture(azimuth, 8192U, gain, width, channel_lock);
    FileGuard in_guard(in);
    const auto out = temp_path(stem, ".wav");
    FileGuard out_guard(out);

    mradm::RenderRequest req;
    req.input_path = in;
    req.output_path = out;
    req.options.renderer = mradm::RendererSelection::apple;
    req.options.output_layout = layout;
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
    if (!check(reader->channels() == expected_channels, "apple output channel count")) {
        return std::nullopt;
    }
    std::vector<float> samples(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());
    return samples;
}

std::optional<std::vector<float>> render_apple_stereo(float azimuth, std::string_view stem, float gain = 1.0F) {
    return render_apple(azimuth, stem, "binaural", 2U, gain);
}

uint32_t read_be32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24U) | (static_cast<uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 2U]) << 8U) | static_cast<uint32_t>(bytes[offset + 3U]);
}

uint64_t read_be64(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return (static_cast<uint64_t>(read_be32(bytes, offset)) << 32U) | read_be32(bytes, offset + 4U);
}

// Read the mChannelLayoutTag from a CAF file's "chan" chunk (first UInt32 of its payload).
std::optional<uint32_t> caf_channel_layout_tag(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t offset = 8U; // skip "caff" header
    while (offset + 12U <= bytes.size()) {
        const auto id = std::string_view{reinterpret_cast<const char*>(bytes.data() + offset), 4U};
        const uint64_t size = read_be64(bytes, offset + 4U);
        const std::size_t payload = offset + 12U;
        if (id == "chan") {
            if (payload + 4U > bytes.size()) {
                return std::nullopt;
            }
            return read_be32(bytes, payload);
        }
        if (size > bytes.size()) { // unknown/streaming size (e.g. data) — chan precedes it
            break;
        }
        offset = payload + size;
    }
    return std::nullopt;
}

// Index of the highest-energy output channel.
std::size_t loudest_channel(const std::vector<float>& samples, uint16_t channels) {
    std::size_t best = 0;
    double best_e = -1.0;
    for (uint16_t c = 0; c < channels; ++c) {
        const double e = channel_energy(samples, channels, c);
        if (e > best_e) {
            best_e = e;
            best = c;
        }
    }
    return best;
}

struct ExpectedLayout {
    const char* id;
    const char* display_name;
    uint16_t channels;
    bool binaural;
};

// clang-format off
constexpr std::array<ExpectedLayout, 8> k_expected_layouts{{
    {"binaural", "binaural", 2U, true},
    {"0+5+0",  "5.1",   6U,  false},
    {"wav71",  "7.1",   8U,  false},
    {"2+5+0",  "5.1.2", 8U,  false},
    {"4+5+0",  "5.1.4", 10U, false},
    {"4+7+0",  "7.1.4", 12U, false},
    {"9.1.6",  "9.1.6", 16U, false},
    {"9+10+3", "22.2",  24U, false},
}};
// clang-format on

bool verify_capabilities() {
    const auto caps = mradm::apple_capabilities();
    bool ok = true;
    ok &= check(caps.backend_name == "apple", "backend name");
    ok &= check(caps.supports_objects, "objects supported");
    ok &= check(caps.supports_direct_speakers, "direct speakers supported");
    ok &= check(!caps.supports_hoa, "hoa unsupported");
    ok &= check(caps.supports_object_divergence, "object divergence advertised");
    ok &= check(!caps.supports_diffuse, "diffuse unsupported");
    ok &= check(caps.supports_render_window, "render window supported");
    const auto has_layout = [&](const std::string& id, uint16_t channels, bool binaural) {
        const auto it = std::ranges::find_if(caps.supported_layouts,
                                             [&](const mradm::CapabilityReport::Layout& l) { return l.id == id; });
        return it != caps.supported_layouts.end() && it->channel_count == channels && it->is_binaural == binaural;
    };
    for (const auto& layout : k_expected_layouts) {
        ok &= check(has_layout(layout.id, layout.channels, layout.binaural), layout.display_name);
    }
    return ok;
}

bool verify_lfe_label_detection() {
    mradm::SceneDirectSpeakersBlock lfe;
    lfe.speaker_labels = {"RC_LFE"};
    mradm::SceneDirectSpeakersBlock non_lfe;
    non_lfe.speaker_labels = {"M+030"};

    bool ok = true;
    ok &= check(mradm::render_common::is_lfe_label("RC_LFE"), "RC_LFE label identifies LFE");
    ok &= check(mradm::render_common::is_lfe_label("R-LFE"), "R-LFE label identifies LFE");
    ok &= check(mradm::render_common::is_lfe_label("low_frequency"), "low_frequency label identifies LFE");
    ok &= check(mradm::render_common::is_lfe_label("Subwoofer"), "Subwoofer label identifies LFE");
    ok &= check(mradm::render_common::direct_speakers_block_is_lfe(lfe), "RC_LFE label identifies LFE");
    ok &= check(!mradm::render_common::direct_speakers_block_is_lfe(non_lfe), "front-left label is not LFE");
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

// Speaker (VBAP) path: a 7.1.4 render must produce 12 channels and pan an ADM-left
// object to the front-left speaker (channel 0 = M+030), an ADM-right object to the
// front-right speaker (channel 1 = M-030). Guards VBAP + output layout order + sign flip.
bool verify_speaker_panning() {
    const auto left = render_apple(30.0F, "mr_apple_714_left", "4+7+0", 12U);
    const auto right = render_apple(-30.0F, "mr_apple_714_right", "4+7+0", 12U);
    if (!left || !right) {
        return false;
    }
    bool ok = true;
    ok &= check(loudest_channel(*left, 12U) == 0U, "ADM azimuth +30 pans to 7.1.4 front-left (ch0)");
    ok &= check(loudest_channel(*right, 12U) == 1U, "ADM azimuth -30 pans to 7.1.4 front-right (ch1)");
    return ok;
}

bool verify_speaker_layouts_render() {
    bool ok = true;
    for (const auto& layout : k_expected_layouts) {
        if (layout.binaural) {
            continue;
        }
        const auto rendered = render_apple(0.0F, "mr_apple_layout", layout.id, layout.channels);
        ok &= check(rendered.has_value(), layout.display_name);
    }
    return ok;
}

// A binaural HRTF render requested via the default "0+2+0" alias must tag the CAF
// container as Binaural (106), not plain Stereo (101) — the output is a binaural signal
// and must not be mistaken for a stereo mix (which players may re-virtualize).
bool verify_binaural_container_tag() {
    const auto in = write_fixture(0.0F, 4096U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    const auto out = temp_path("mr_apple_tag", ".caf");
    FileGuard out_guard(out);

    mradm::RenderRequest req;
    req.input_path = in;
    req.output_path = out;
    req.options.renderer = mradm::RendererSelection::apple;
    req.options.output_layout = "0+2+0"; // default alias → must normalize to binaural tag
    req.options.peak_limit = false;
    req.options.measure_loudness = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!check(res.success(), "apple 0+2+0 → CAF render succeeds")) {
        std::cerr << "context: " << res.error.message << " " << res.error.context << "\n";
        return false;
    }
    const auto tag = caf_channel_layout_tag(out);
    constexpr uint32_t k_binaural = (106U << 16) | 2U;
    constexpr uint32_t k_stereo = (101U << 16) | 2U;
    if (!tag.has_value()) {
        check(false, "CAF has a chan chunk");
        return false;
    }
    const uint32_t tag_value = tag.value();
    bool ok = check(tag_value == k_binaural, "CAF tagged Binaural (106), not Stereo");
    ok &= check(tag_value != k_stereo, "CAF not tagged plain Stereo for a binaural render");
    return ok;
}

bool verify_render_window_frame_count() {
    constexpr uint32_t k_sample_rate = 48000U;
    const auto in = write_fixture(0.0F, 4096U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    const auto out = temp_path("mr_apple_window", ".wav");
    FileGuard out_guard(out);

    mradm::RenderRequest req;
    req.input_path = in;
    req.output_path = out;
    req.options.renderer = mradm::RendererSelection::apple;
    req.options.output_layout = "binaural";
    req.options.peak_limit = false;
    req.options.measure_loudness = false;
    req.options.render_start_sec = 1024.0 / static_cast<double>(k_sample_rate);
    req.options.render_end_sec = 2048.0 / static_cast<double>(k_sample_rate);

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!check(res.success(), "apple render window succeeds")) {
        std::cerr << "context: " << res.error.message << " " << res.error.context << "\n";
        return false;
    }

    auto reader = mradm::audio::FloatWavReader::open(out.string());
    if (!check(reader.has_value(), "apple render window output opens")) {
        return false;
    }

    bool ok = true;
    ok &= check(reader->channels() == 2U, "apple render window output is binaural");
    ok &= check(reader->frame_count() == 1024U, "apple render window writes only requested frames");
    return ok;
}

// Number of output channels carrying more than `frac` of the total energy.
int active_channels(const std::vector<float>& samples, uint16_t channels, double frac) {
    double total = 0.0;
    for (uint16_t c = 0; c < channels; ++c) {
        total += channel_energy(samples, channels, c);
    }
    int active = 0;
    for (uint16_t c = 0; c < channels; ++c) {
        if (channel_energy(samples, channels, c) > frac * total) {
            ++active;
        }
    }
    return active;
}

double total_energy(const std::vector<float>& samples, uint16_t channels) {
    double total = 0.0;
    for (uint16_t c = 0; c < channels; ++c) {
        total += channel_energy(samples, channels, c);
    }
    return total;
}

// Extent: a wide front object (ADM width) must spread energy across more 7.1.4 speakers
// than a point object at the same position, and the 17-point cloud's partition-of-unity
// weights must keep the total energy in the same ballpark (not silent, not exploded).
bool verify_extent_spread() {
    const auto point = render_apple(0.0F, "mr_apple_point", "4+7+0", 12U, 1.0F, 0.0F);
    const auto wide = render_apple(0.0F, "mr_apple_wide", "4+7+0", 12U, 1.0F, 0.8F);
    if (!point || !wide) {
        return false;
    }
    const int point_active = active_channels(*point, 12U, 0.05);
    const int wide_active = active_channels(*wide, 12U, 0.05);
    const double point_e = total_energy(*point, 12U);
    const double wide_e = total_energy(*wide, 12U);

    bool ok = true;
    ok &= check(wide_active > point_active, "width spreads energy across more speakers than a point");
    ok &= check(wide_active >= 3, "wide front object lights up >= 3 speakers");
    ok &= check(wide_e > 0.1 * point_e && wide_e < 10.0 * point_e, "extent keeps total energy in the same ballpark");
    return ok;
}

// Render a wide front object to 7.1.4 with an explicit speaker spread mode.
std::optional<std::vector<float>> render_714_spread(float width, mradm::SpeakerSpreadMode mode, std::string_view stem) {
    const auto in = write_fixture(0.0F, 8192U, 1.0F, width, false);
    FileGuard in_guard(in);
    const auto out = temp_path(stem, ".wav");
    FileGuard out_guard(out);

    mradm::RenderRequest req;
    req.input_path = in;
    req.output_path = out;
    req.options.renderer = mradm::RendererSelection::apple;
    req.options.output_layout = "4+7+0";
    req.options.speaker_spread_mode = mode;
    req.options.peak_limit = false;
    req.options.measure_loudness = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    if (!check(service.render(req, progress, logs).success(), "apple spread-mode render succeeds")) {
        return std::nullopt;
    }
    auto reader = mradm::audio::FloatWavReader::open(out.string());
    if (!reader || reader->channels() != 12U) {
        return std::nullopt;
    }
    std::vector<float> samples(12U * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());
    return samples;
}

// --speaker-spread-mode none must disable the extent cloud: a wide object renders as a
// point (localized), aligning Apple with the VBAP/binaural spread-mode capability gating.
bool verify_spread_mode_none_disables_extent() {
    const auto spread = render_714_spread(0.8F, mradm::SpeakerSpreadMode::automatic, "mr_apple_spread_auto");
    const auto none = render_714_spread(0.8F, mradm::SpeakerSpreadMode::none, "mr_apple_spread_none");
    if (!spread || !none) {
        return false;
    }
    const int spread_active = active_channels(*spread, 12U, 0.05);
    const int none_active = active_channels(*none, 12U, 0.05);
    bool ok = true;
    ok &= check(none_active < spread_active, "speaker-spread-mode none renders a wide object as a point");
    ok &= check(none_active <= 2, "spread-mode none keeps the wide object localized");
    return ok;
}

// channelLock (speaker output): an object between two speakers snaps to the nearest one.
// At az=20 (between M+000=ch2 and M+030=ch0) a locked object collapses onto a single
// speaker, where a free object pans across both. Guards the resolved-speaker-set path.
bool verify_channel_lock_snaps() {
    const auto locked = render_apple(20.0F, "mr_apple_cl_on", "4+7+0", 12U, 1.0F, 0.0F, true);
    const auto free = render_apple(20.0F, "mr_apple_cl_off", "4+7+0", 12U, 1.0F, 0.0F, false);
    if (!locked || !free) {
        return false;
    }
    const int locked_active = active_channels(*locked, 12U, 0.05);
    const int free_active = active_channels(*free, 12U, 0.05);
    bool ok = true;
    ok &= check(locked_active < free_active, "channelLock concentrates onto fewer speakers than free VBAP");
    ok &= check(locked_active == 1, "channelLock snaps the object to a single speaker");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_capabilities();
    ok &= verify_lfe_label_detection();
    ok &= verify_directional_sign();
    ok &= verify_zero_gain_is_silent();
    ok &= verify_speaker_panning();
    ok &= verify_speaker_layouts_render();
    ok &= verify_binaural_container_tag();
    ok &= verify_render_window_frame_count();
    ok &= verify_extent_spread();
    ok &= verify_spread_mode_none_disables_extent();
    ok &= verify_channel_lock_snaps();
    return ok ? 0 : 1;
}
