#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>
#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm/audio_io.h"
#include "adm/io.h"
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

constexpr UInt32 k_probe_frames = 4096U;
constexpr UInt32 k_probe_sample_rate = 48000U;
constexpr UInt32 k_probe_render_block = 512U;
// macOS 13/14 SpatialMixer property IDs; numeric IDs keep the diagnostic buildable
// with SDKs that do not expose the personalized HRTF enum names.
constexpr AudioUnitPropertyID k_spatial_mixer_personalized_hrtf_mode = 3113U;
constexpr AudioUnitPropertyID k_spatial_mixer_any_input_is_using_personalized_hrtf = 3116U;
constexpr UInt32 k_spatial_mixer_personalized_hrtf_off = 0U;
constexpr UInt32 k_spatial_mixer_personalized_hrtf_on = 1U;

std::string os_status_text(OSStatus status) {
    return std::to_string(static_cast<int>(status));
}

class AudioUnitGuard {
  public:
    explicit AudioUnitGuard(AudioUnit unit) : unit_(unit) {}
    AudioUnitGuard(const AudioUnitGuard&) = delete;
    AudioUnitGuard& operator=(const AudioUnitGuard&) = delete;
    AudioUnitGuard(AudioUnitGuard&& other) noexcept : unit_(other.unit_) { other.unit_ = nullptr; }
    AudioUnitGuard& operator=(AudioUnitGuard&& other) noexcept {
        if (this != &other) {
            dispose();
            unit_ = other.unit_;
            other.unit_ = nullptr;
        }
        return *this;
    }
    ~AudioUnitGuard() { dispose(); }

    [[nodiscard]] AudioUnit get() const noexcept { return unit_; }

  private:
    void dispose() noexcept {
        if (unit_ != nullptr) {
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
    }

    AudioUnit unit_{nullptr};
};

AudioStreamBasicDescription probe_pcm_float_format(UInt32 channels) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = static_cast<double>(k_probe_sample_rate);
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved |
                       kAudioFormatFlagsNativeEndian;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    return fmt;
}

bool set_probe_uint32(AudioUnit unit,
                      AudioUnitPropertyID property,
                      AudioUnitScope scope,
                      AudioUnitElement element,
                      UInt32 value,
                      const char* label) {
    const OSStatus status = AudioUnitSetProperty(unit, property, scope, element, &value, sizeof(value));
    if (status != noErr) {
        std::cerr << "FAIL: AUSpatialMixer set " << label << " failed: " << os_status_text(status) << "\n";
        return false;
    }
    return true;
}

bool set_probe_stream_format(AudioUnit unit, AudioUnitScope scope, AudioUnitElement element, UInt32 channels) {
    auto fmt = probe_pcm_float_format(channels);
    const OSStatus status =
        AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, element, &fmt, sizeof(fmt));
    if (status != noErr) {
        std::cerr << "FAIL: AUSpatialMixer set stream format failed: " << os_status_text(status) << "\n";
        return false;
    }
    return true;
}

std::optional<UInt32>
get_probe_uint32(AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope, AudioUnitElement element) {
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    const OSStatus status = AudioUnitGetProperty(unit, property, scope, element, &value, &size);
    if (status != noErr) {
        return std::nullopt;
    }
    return value;
}

struct ProbeInputContext {
    const std::vector<float>* samples{nullptr};
};

// cppcheck-suppress constParameterCallback ; AURenderCallback mandates void* (non-const).
OSStatus probe_input_render_callback(void* ref_con,
                                     AudioUnitRenderActionFlags* /*flags*/,
                                     const AudioTimeStamp* time_stamp,
                                     UInt32 /*bus_number*/,
                                     UInt32 frames,
                                     AudioBufferList* io_data) {
    const auto* ctx = static_cast<const ProbeInputContext*>(ref_con);
    const auto start = (time_stamp != nullptr && (time_stamp->mFlags & kAudioTimeStampSampleTimeValid) != 0U)
                           ? static_cast<std::size_t>(std::max<Float64>(0.0, time_stamp->mSampleTime))
                           : std::size_t{0};
    for (UInt32 b = 0; b < io_data->mNumberBuffers; ++b) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) - CoreAudio flexible array.
        auto* out = static_cast<float*>(io_data->mBuffers[b].mData);
        for (UInt32 f = 0; f < frames; ++f) {
            const auto idx = start + f;
            out[f] = idx < ctx->samples->size() ? ctx->samples->at(idx) : 0.0F;
        }
    }
    return noErr;
}

struct HrtfProbeCase {
    const char* name{""};
    UInt32 algorithm{0};
    std::optional<UInt32> personalized_mode;
};

struct HrtfProbeResult {
    const char* name{""};
    UInt32 algorithm{0};
    std::optional<UInt32> algorithm_readback;
    std::optional<UInt32> personalized_mode_readback;
    std::optional<bool> any_personalized;
    bool personalized_set_ok{false};
    bool personalized_set_unsupported{false};
    double left_energy{0.0};
    double right_energy{0.0};
    std::vector<float> samples;
};

std::optional<AudioUnitGuard> create_probe_spatial_mixer() {
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Mixer;
    desc.componentSubType = kAudioUnitSubType_SpatialMixer;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr) {
        std::cerr << "FAIL: AUSpatialMixer component unavailable\n";
        return std::nullopt;
    }
    AudioUnit unit = nullptr;
    const OSStatus status = AudioComponentInstanceNew(comp, &unit);
    if (status != noErr || unit == nullptr) {
        std::cerr << "FAIL: create AUSpatialMixer failed: " << os_status_text(status) << "\n";
        return std::nullopt;
    }
    return AudioUnitGuard{unit};
}

bool hrtf_probe_enabled() {
    const char* value = std::getenv("MR_ADM_APPLE_HRTF_PROBE");
    return value != nullptr && std::string_view{value} == "1";
}

bool configure_hrtf_probe_unit(AudioUnit unit, const HrtfProbeCase& probe) {
    return set_probe_uint32(unit, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, 1U, "input count") &&
           set_probe_uint32(unit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Global,
                            0,
                            k_probe_render_block,
                            "maximum frames per slice") &&
           set_probe_uint32(unit,
                            kAudioUnitProperty_SpatialMixerOutputType,
                            kAudioUnitScope_Global,
                            0,
                            kSpatialMixerOutputType_Headphones,
                            "headphone output type") &&
           set_probe_stream_format(unit, kAudioUnitScope_Output, 0, 2U) &&
           set_probe_stream_format(unit, kAudioUnitScope_Input, 0, 1U) &&
           set_probe_uint32(unit,
                            kAudioUnitProperty_SpatializationAlgorithm,
                            kAudioUnitScope_Input,
                            0,
                            probe.algorithm,
                            "spatialization algorithm") &&
           set_probe_uint32(unit,
                            kAudioUnitProperty_SpatialMixerSourceMode,
                            kAudioUnitScope_Input,
                            0,
                            kSpatialMixerSourceMode_PointSource,
                            "point source mode");
}

bool apply_probe_personalized_mode(AudioUnit unit, const HrtfProbeCase& probe, HrtfProbeResult& result) {
    if (probe.personalized_mode.has_value()) {
        const UInt32 personalized_mode = probe.personalized_mode.value();
        const OSStatus status = AudioUnitSetProperty(unit,
                                                     k_spatial_mixer_personalized_hrtf_mode,
                                                     kAudioUnitScope_Global,
                                                     0,
                                                     &personalized_mode,
                                                     sizeof(personalized_mode));
        result.personalized_set_ok = status == noErr;
        result.personalized_set_unsupported = status != noErr;
        if (status != noErr) {
            std::cerr << "WARN: " << probe.name << " personalized HRTF property rejected: " << os_status_text(status)
                      << "\n";
            return false;
        }
    }
    return true;
}

std::vector<float> make_probe_input() {
    std::vector<float> input(k_probe_frames);
    for (UInt32 i = 0; i < k_probe_frames; ++i) {
        input[i] = 0.2F * std::sin(2.0F * std::numbers::pi_v<float> * 997.0F * static_cast<float>(i) /
                                   static_cast<float>(k_probe_sample_rate));
    }
    return input;
}

bool attach_probe_input(AudioUnit unit, ProbeInputContext& input_context) {
    AURenderCallbackStruct callback{};
    callback.inputProc = &probe_input_render_callback;
    callback.inputProcRefCon = &input_context;
    const OSStatus callback_status = AudioUnitSetProperty(
        unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    if (callback_status != noErr) {
        std::cerr << "FAIL: set probe render callback failed: " << os_status_text(callback_status) << "\n";
        return false;
    }
    return true;
}

bool initialize_probe_unit(AudioUnit unit, const char* name) {
    const OSStatus init_status = AudioUnitInitialize(unit);
    if (init_status != noErr) {
        std::cerr << "FAIL: initialize " << name << " failed: " << os_status_text(init_status) << "\n";
        return false;
    }

    AudioUnitSetParameter(unit, kSpatialMixerParam_Azimuth, kAudioUnitScope_Input, 0, -45.0F, 0);
    AudioUnitSetParameter(unit, kSpatialMixerParam_Elevation, kAudioUnitScope_Input, 0, 0.0F, 0);
    AudioUnitSetParameter(unit, kSpatialMixerParam_Distance, kAudioUnitScope_Input, 0, 1.0F, 0);
    AudioUnitSetParameter(unit, kSpatialMixerParam_Gain, kAudioUnitScope_Input, 0, 0.0F, 0);
    return true;
}

void read_probe_properties(AudioUnit unit, HrtfProbeResult& result) {
    result.algorithm_readback =
        get_probe_uint32(unit, kAudioUnitProperty_SpatializationAlgorithm, kAudioUnitScope_Input, 0);
    result.personalized_mode_readback =
        get_probe_uint32(unit, k_spatial_mixer_personalized_hrtf_mode, kAudioUnitScope_Global, 0);
    if (auto any =
            get_probe_uint32(unit, k_spatial_mixer_any_input_is_using_personalized_hrtf, kAudioUnitScope_Global, 0)) {
        result.any_personalized = any.value() != 0U;
    }
}

bool render_probe_output(AudioUnit unit, HrtfProbeResult& result) {
    std::array<std::vector<float>, 2> planar{std::vector<float>(k_probe_render_block),
                                             std::vector<float>(k_probe_render_block)};
    std::vector<std::uint8_t> abl_storage(sizeof(AudioBufferList) + sizeof(AudioBuffer));
    auto* abl = reinterpret_cast<AudioBufferList*>(abl_storage.data());
    abl->mNumberBuffers = 2;
    result.samples.resize(static_cast<std::size_t>(k_probe_frames) * 2U);
    AudioTimeStamp time_stamp{};
    time_stamp.mFlags = kAudioTimeStampSampleTimeValid;
    for (UInt32 frames_done = 0; frames_done < k_probe_frames;) {
        const UInt32 frames_now = std::min(k_probe_render_block, k_probe_frames - frames_done);
        for (UInt32 ch = 0; ch < 2U; ++ch) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) - CoreAudio flexible array.
            abl->mBuffers[ch].mNumberChannels = 1;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) - CoreAudio flexible array.
            abl->mBuffers[ch].mDataByteSize = frames_now * sizeof(float);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) - CoreAudio flexible array.
            abl->mBuffers[ch].mData = planar[ch].data();
        }

        time_stamp.mSampleTime = static_cast<Float64>(frames_done);
        AudioUnitRenderActionFlags flags = 0;
        const OSStatus render_status = AudioUnitRender(unit, &flags, &time_stamp, 0, frames_now, abl);
        if (render_status != noErr) {
            std::cerr << "FAIL: AudioUnitRender " << result.name << " failed: " << os_status_text(render_status)
                      << "\n";
            return false;
        }

        for (UInt32 f = 0; f < frames_now; ++f) {
            for (UInt32 ch = 0; ch < 2U; ++ch) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) - fixed 2-buffer ABL.
                const float sample = planar[ch][f];
                result.samples[(static_cast<std::size_t>(frames_done + f) * 2U) + ch] = sample;
                if (ch == 0U) {
                    result.left_energy += static_cast<double>(sample) * static_cast<double>(sample);
                } else {
                    result.right_energy += static_cast<double>(sample) * static_cast<double>(sample);
                }
            }
        }
        frames_done += frames_now;
    }
    return true;
}

std::optional<HrtfProbeResult> run_hrtf_probe(const HrtfProbeCase& probe) {
    auto unit_guard = create_probe_spatial_mixer();
    if (!unit_guard) {
        return std::nullopt;
    }
    AudioUnit unit = unit_guard->get();
    HrtfProbeResult result;
    result.name = probe.name;
    result.algorithm = probe.algorithm;

    if (!configure_hrtf_probe_unit(unit, probe)) {
        return std::nullopt;
    }
    if (!apply_probe_personalized_mode(unit, probe, result)) {
        return result;
    }

    auto input = make_probe_input();
    ProbeInputContext input_context{&input};
    if (!attach_probe_input(unit, input_context) || !initialize_probe_unit(unit, probe.name)) {
        return std::nullopt;
    }

    read_probe_properties(unit, result);
    if (!render_probe_output(unit, result)) {
        return std::nullopt;
    }
    return result;
}

double rms_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
    if (lhs.size() != rhs.size() || lhs.empty()) {
        return 0.0;
    }
    double total = 0.0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const double diff = static_cast<double>(lhs[i]) - static_cast<double>(rhs[i]);
        total += diff * diff;
    }
    return std::sqrt(total / static_cast<double>(lhs.size()));
}

// Single OBJECTS object at a fixed azimuth (ADM convention: +ve = left), linear gain,
// optional ADM width (0..1) for extent spreading, and optional channelLock.
std::pair<std::shared_ptr<adm::Document>, std::string>
make_object_doc(float azimuth, float gain, float width, bool channel_lock, bool mute = false) {
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
    if (mute) {
        obj->set(adm::Mute{true});
    }
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

std::filesystem::path
write_fixture(float azimuth, uint32_t frames, float gain, float width, bool channel_lock, bool mute = false) {
    constexpr uint32_t k_ch = 1U;
    constexpr uint32_t k_sr = 48000U;
    const auto [doc, uid_str] = make_object_doc(azimuth, gain, width, channel_lock, mute);
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

std::optional<std::vector<float>>
render_apple(float azimuth,
             std::string_view stem,
             const std::string& layout,
             uint16_t expected_channels,
             float gain = 1.0F,
             float width = 0.0F,
             bool channel_lock = false,
             mradm::AppleSpatialPreset apple_spatial_preset = mradm::AppleSpatialPreset::off,
             const mradm::ListenerOrientation& listener = {}) {
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
    req.options.apple_spatial_preset = apple_spatial_preset;
    req.options.listener_orientation = listener;

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

bool verify_spatial_presets_render_binaural() {
    bool ok = true;
    for (const auto preset :
         {mradm::AppleSpatialPreset::headphone_default, mradm::AppleSpatialPreset::headphone_movie}) {
        const auto rendered = render_apple(0.0F, "mr_apple_spatial_preset", "binaural", 2U, 1.0F, 0.0F, false, preset);
        if (!rendered.has_value()) {
            check(false, "apple binaural factory preset renders");
            ok = false;
            continue;
        }
        ok &= check(total_energy(rendered.value(), 2U) > 1.0e-5, "apple binaural factory preset output is not silent");
    }
    return ok;
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

// Single DirectSpeakers (bed) channel at a fixed speaker position and label.
std::pair<std::shared_ptr<adm::Document>, std::string>
make_ds_doc(float azimuth, float elevation, const std::string& label) {
    auto doc = adm::Document::create();
    auto cf =
        adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"AppleDsCF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    {
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{azimuth}, adm::Elevation{elevation}, adm::Distance{1.0F}}};
        block.add(adm::SpeakerLabel{label});
        block.set(adm::Gain{1.0F});
        cf->add(block);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"AppleDsPF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"AppleDsSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"AppleDsTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"AppleDsObject"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"AppleDsContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"AppleDsProgramme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// Render a single DirectSpeakers bed channel through the apple backend to `layout`.
std::optional<std::vector<float>> render_ds(float azimuth,
                                            float elevation,
                                            const std::string& label,
                                            const std::string& layout,
                                            uint16_t channels,
                                            std::string_view stem) {
    constexpr uint32_t k_sr = 48000U;
    constexpr uint32_t k_frames = 8192U;
    const auto [doc, uid_str] = make_ds_doc(azimuth, elevation, label);
    const auto in = temp_path("mr_apple_ds_input", ".wav");
    FileGuard in_guard(in);
    {
        std::ostringstream xml_buf;
        adm::writeXml(xml_buf, doc);
        auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
        auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
        auto writer = bw64::writeFile(in.string(), 1U, k_sr, 24U, chna, axml);
        std::vector<float> samples(k_frames);
        for (uint32_t i = 0; i < k_frames; ++i) {
            samples[i] = 0.25F * std::sin(2.0F * std::numbers::pi_v<float> * 440.0F * static_cast<float>(i) /
                                          static_cast<float>(k_sr));
        }
        writer->write(samples.data(), k_frames);
    }
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
    if (!check(service.render(req, progress, logs).success(), "apple DirectSpeakers render succeeds")) {
        return std::nullopt;
    }
    auto reader = mradm::audio::FloatWavReader::open(out.string());
    if (!reader || reader->channels() != channels) {
        return std::nullopt;
    }
    std::vector<float> samples(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(samples.data(), reader->frame_count());
    return samples;
}

// Speaker routing: a DirectSpeakers bed channel (AmbienceBed) must land on its output
// speaker, and an LFE bed channel (Bypass) on the output LFE channel. 7.1.4 channel order:
// ch0=L(M+030) ch1=R(M-030) ch2=C ch3=LFE.
bool verify_bed_and_lfe_routing() {
    const auto bed_l = render_ds(30.0F, 0.0F, "M+030", "4+7+0", 12U, "mr_apple_bed_l");
    const auto bed_c = render_ds(0.0F, 0.0F, "M+000", "4+7+0", 12U, "mr_apple_bed_c");
    const auto lfe = render_ds(45.0F, -30.0F, "RC_LFE", "4+7+0", 12U, "mr_apple_lfe");
    if (!bed_l || !bed_c || !lfe) {
        return false;
    }
    bool ok = true;
    ok &= check(loudest_channel(*bed_l, 12U) == 0U, "bed M+030 routes to 7.1.4 front-left (ch0)");
    ok &= check(loudest_channel(*bed_c, 12U) == 2U, "bed M+000 routes to 7.1.4 centre (ch2)");
    ok &= check(loudest_channel(*lfe, 12U) == 3U, "LFE bed routes to 7.1.4 LFE channel (ch3)");
    return ok;
}

// 22.2 (CICP_13) labels its two LFE channels LFE2/LFE3, not LFEScreen. AUSpatialMixer's bypass LFE
// path only ever targets a channel labeled LFEScreen, so without the output-layout relabel in
// set_output_layout_tag the bed LFE is dropped — and the mixer folds it into ch0 (Lw) at unity gain.
// This guards both: the LFE lands on the LFE channel (ch3), and does NOT leak into ch0.
bool verify_22_2_lfe_routing() {
    const auto lfe = render_ds(45.0F, -30.0F, "RC_LFE", "9+10+3", 24U, "mr_apple_lfe_222");
    if (!lfe) {
        return false;
    }
    bool ok = true;
    ok &= check(loudest_channel(*lfe, 24U) == 3U, "22.2 LFE bed routes to CICP_13 LFE channel (ch3)");
    const double lfe_energy = channel_energy(*lfe, 24U, 3U);
    const double lw_energy = channel_energy(*lfe, 24U, 0U);
    ok &= check(lfe_energy > 1.0e-6, "22.2 LFE channel (ch3) is not silent");
    ok &= check(lw_energy < lfe_energy * 1.0e-3, "22.2 LFE does not leak into ch0 (Lw)");
    return ok;
}

bool verify_spatial_mixer_hrtf_modes_probe() {
    if (!hrtf_probe_enabled()) {
        return true;
    }

    constexpr std::array<HrtfProbeCase, 4> k_hrtf_cases{{
        {"HRTF", kSpatializationAlgorithm_HRTF, k_spatial_mixer_personalized_hrtf_off},
        {"HRTFHQ", kSpatializationAlgorithm_HRTFHQ, k_spatial_mixer_personalized_hrtf_off},
        {"UseOutputType", kSpatializationAlgorithm_UseOutputType, k_spatial_mixer_personalized_hrtf_off},
        {"UseOutputType+pHRTF", kSpatializationAlgorithm_UseOutputType, k_spatial_mixer_personalized_hrtf_on},
    }};

    std::vector<HrtfProbeResult> results;
    results.reserve(k_hrtf_cases.size());
    bool ok = true;
    for (const auto& probe : k_hrtf_cases) {
        auto result = run_hrtf_probe(probe);
        if (!result) {
            ok = false;
            continue;
        }
        const double total = result->left_energy + result->right_energy;
        if (result->personalized_set_unsupported && probe.personalized_mode == k_spatial_mixer_personalized_hrtf_on) {
            std::cerr << "WARN: skipping pHRTF actual-use assertion because the property is unavailable\n";
            results.push_back(std::move(*result));
            continue;
        }
        ok &= check(total > 1.0e-5, "AUSpatialMixer HRTF probe output is not silent");
        ok &= check(result->algorithm_readback.value_or(probe.algorithm) == probe.algorithm,
                    "AUSpatialMixer HRTF probe readback algorithm matches request");
        if (probe.personalized_mode.has_value() && !result->personalized_set_unsupported) {
            ok &= check(result->personalized_set_ok, "AUSpatialMixer personalized HRTF mode property can be set");
            ok &= check(result->personalized_mode_readback.value_or(probe.personalized_mode.value()) ==
                            probe.personalized_mode.value(),
                        "AUSpatialMixer personalized HRTF mode readback matches request");
        }
        results.push_back(std::move(*result));
    }

    const auto use_output = std::ranges::find_if(
        results, [](const HrtfProbeResult& r) { return std::string_view{r.name} == "UseOutputType"; });

    std::cout << "AUSpatialMixer HRTF mode probe\n";
    std::cout << std::fixed << std::setprecision(8);
    for (const auto& result : results) {
        const double total = result.left_energy + result.right_energy;
        const double balance = total > 0.0 ? (result.left_energy - result.right_energy) / total : 0.0;
        std::cout << "  " << result.name << ": requested_algorithm=" << result.algorithm << " readback_algorithm=";
        if (result.algorithm_readback.has_value()) {
            std::cout << result.algorithm_readback.value();
        } else {
            std::cout << "unavailable";
        }
        std::cout << " personalized_mode=";
        if (result.personalized_mode_readback.has_value()) {
            std::cout << result.personalized_mode_readback.value();
        } else {
            std::cout << "unavailable";
        }
        std::cout << " actual_personalized=";
        if (result.any_personalized.has_value()) {
            std::cout << (result.any_personalized.value() ? "yes" : "no");
        } else {
            std::cout << "unavailable";
        }
        std::cout << " total_energy=" << total << " lr_balance=" << balance;
        if (use_output != results.end() && !result.samples.empty() && !use_output->samples.empty()) {
            std::cout << " rms_diff_vs_use_output=" << rms_difference(result.samples, use_output->samples);
        }
        std::cout << "\n";
    }

    const auto phrtf = std::ranges::find_if(
        results, [](const HrtfProbeResult& r) { return std::string_view{r.name} == "UseOutputType+pHRTF"; });
    const auto phrtf_actual = phrtf != results.end() ? phrtf->any_personalized : std::optional<bool>{};
    if (phrtf_actual.has_value()) {
        std::cout << "  pHRTF actual use: " << (phrtf_actual.value() ? "enabled" : "not active") << "\n";
    }
    return ok;
}

// Listener head orientation: a +90° yaw (head turned left) must move a front point source
// toward the right ear, and the binaural output must differ from the identity (head-forward)
// render. Locks the Apple backend's HeadYaw sign convention.
bool verify_listener_orientation() {
    const auto forward = render_apple(0.0F, "mr_apple_yaw_identity", "binaural", 2U);
    mradm::ListenerOrientation yaw_left{};
    yaw_left.yaw_deg = 90.0F;
    const auto turned = render_apple(
        0.0F, "mr_apple_yaw_left", "binaural", 2U, 1.0F, 0.0F, false, mradm::AppleSpatialPreset::off, yaw_left);
    if (!forward || !turned) {
        return check(false, "listener-orientation renders succeed");
    }
    bool ok = true;
    const double id_l = channel_energy(*forward, 2U, 0U);
    const double id_r = channel_energy(*forward, 2U, 1U);
    const double turn_l = channel_energy(*turned, 2U, 0U);
    const double turn_r = channel_energy(*turned, 2U, 1U);
    ok &= check(rms_difference(*forward, *turned) > 1.0e-4, "listener yaw changes the binaural output");
    ok &= check(std::abs(id_l - id_r) < 0.25 * (id_l + id_r), "identity orientation keeps a front source L/R balanced");
    ok &= check(turn_r > turn_l, "yaw=+90 (head turned left) moves a front source toward the right ear");
    return ok;
}

// Render the whole timeline through AppleStream, pulling in the given (repeating) chunk
// pattern. Returns interleaved float PCM, or nullopt on error.
std::optional<std::vector<float>> render_apple_stream_full(mradm::IRenderer& renderer,
                                                           const mradm::IPreparedRender& prepared,
                                                           const mradm::RenderPlan& plan,
                                                           mradm::LogSink& logs,
                                                           const std::vector<std::size_t>& chunk_pattern) {
    auto stream = renderer.open_stream(prepared, plan, logs);
    if (!check(stream.has_value(), "apple open_stream succeeds")) {
        return std::nullopt;
    }
    const uint32_t ch = (*stream)->out_channels();
    std::vector<float> out;
    std::vector<float> buf;
    std::size_t pi = 0;
    while (true) {
        const std::size_t frames = chunk_pattern[pi % chunk_pattern.size()];
        ++pi;
        buf.assign(frames * ch, 0.0F);
        auto produced = (*stream)->process(std::span<float>(buf), frames);
        if (!check(produced.has_value(), "apple stream process succeeds")) {
            return std::nullopt;
        }
        const std::size_t got = *produced;
        if (got == 0) {
            break;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got * ch));
    }
    return out;
}

// Output-stage rendering (方案 B): produce_intermediate (worker, reads source PCM) + render_output
// (audio callback, applies the orientation + runs the AU) with a STATIC identity orientation must be
// bit-identical to the single-stage process() path — same AU, same per-block params, just split into
// two calls so the head pose can enter at playout. This is the core correctness gate for the split.
bool verify_apple_output_stage_matches_process() {
    const auto in = write_fixture(45.0F, 8192U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value(), "output-stage: import scene")) {
        return false;
    }
    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_apple_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "output-stage: apple prepare")) {
        return false;
    }

    // Reference: single-stage process() in fixed 512-frame chunks.
    const auto reference = render_apple_stream_full(*renderer, **prepared, plan, logs, {512});
    if (!reference) {
        return false;
    }

    // Output-stage: a fresh stream, driven via produce_intermediate + render_output(identity).
    auto stream = renderer->open_stream(**prepared, plan, logs);
    if (!check(stream.has_value(), "output-stage: open stream")) {
        return false;
    }
    if (!check((*stream)->renders_orientation_at_output(), "output-stage: binaural opts into output-stage")) {
        return false;
    }
    const uint32_t in_ch = (*stream)->intermediate_channels();
    const uint32_t out_ch = (*stream)->out_channels();
    const mradm::ListenerOrientation identity{};
    std::vector<float> out;
    std::vector<float> interm;
    std::vector<float> buf;
    constexpr std::size_t k = 512;
    while (true) {
        interm.assign(k * in_ch, 0.0F);
        auto got = (*stream)->produce_intermediate(std::span<float>(interm), k);
        if (!check(got.has_value(), "output-stage: produce_intermediate succeeds")) {
            return false;
        }
        if (*got == 0) {
            break;
        }
        buf.assign(*got * out_ch, 0.0F);
        const std::size_t rendered = (*stream)->render_output(
            std::span<const float>(interm.data(), *got * in_ch), std::span<float>(buf), *got, identity);
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(rendered * out_ch));
    }

    bool ok = check(out.size() == reference->size(), "output-stage: same total samples as process()");
    if (ok) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (out[i] != (*reference)[i]) {
                ok = check(false, "output-stage: bit-identical to single-stage process()");
                break;
            }
        }
    }
    return ok;
}

// Output-stage loop (方案 B): the Stage A primitive reposition_source() must re-read the source from
// the wrap point and must NOT disturb the render cursors (the consumer wraps separately). Here we
// verify the source reposition: produce two distinct blocks, reposition to 0, and confirm the next
// block re-reads the first block's source exactly.
bool verify_apple_reposition_source() {
    const auto in = write_fixture(30.0F, 8192U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value(), "reposition-source: import scene")) {
        return false;
    }
    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_apple_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "reposition-source: apple prepare")) {
        return false;
    }
    auto stream = renderer->open_stream(**prepared, plan, logs);
    if (!check(stream.has_value(), "reposition-source: open stream")) {
        return false;
    }
    const uint32_t in_ch = (*stream)->intermediate_channels();
    constexpr std::size_t k = 1024;
    std::vector<float> block_a(k * in_ch, 0.0F);
    std::vector<float> block_b(k * in_ch, 0.0F);
    std::vector<float> block_a2(k * in_ch, 0.0F);

    auto got_a = (*stream)->produce_intermediate(std::span<float>(block_a), k);   // source [0, k)
    auto got_b = (*stream)->produce_intermediate(std::span<float>(block_b), k);   // source [k, 2k)
    auto rep = (*stream)->reposition_source(0);                                   // Stage A wrap → 0
    auto got_a2 = (*stream)->produce_intermediate(std::span<float>(block_a2), k); // source [0, k) again
    bool ok = check(got_a.has_value() && got_b.has_value() && rep.has_value() && got_a2.has_value() && *got_a == k &&
                        *got_b == k && *got_a2 == k,
                    "reposition-source: produce + reposition succeed");
    ok &= check(block_a2 == block_a, "reposition-source: re-reads the source from the wrap point");
    ok &= check(block_a != block_b, "reposition-source: distinct source blocks (sanity)");
    return ok;
}

// AppleStream (realtime) must reproduce the offline render_window render, and its output
// must be independent of the caller's pull chunk size (FIFO + canonical k_render_block).
bool verify_apple_stream_matches_window() {
    const auto in = write_fixture(45.0F, 8192U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value(), "stream: import scene")) {
        return false;
    }

    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_apple_renderer();
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "stream: apple prepare")) {
        return false;
    }

    // Reference: the offline render_window path to a float WAV.
    const auto out_ref = temp_path("mr_apple_stream_ref", ".wav");
    FileGuard out_guard(out_ref);
    mradm::RenderPlan window_plan = plan;
    window_plan.output_path = out_ref.string();
    if (!check(renderer->render_window(**prepared, window_plan, progress, logs).has_value(),
               "stream: reference render_window")) {
        return false;
    }
    auto reader = mradm::audio::FloatWavReader::open(out_ref.string());
    if (!check(reader.has_value() && reader->channels() == 2U, "stream: reference WAV opens (2ch)")) {
        return false;
    }
    std::vector<float> ref(static_cast<std::size_t>(reader->channels()) * reader->frame_count());
    reader->read(ref.data(), reader->frame_count());

    // AppleStream, uniform vs. varied chunking (two fresh AUs, identical input/params/time).
    const auto uniform = render_apple_stream_full(*renderer, **prepared, plan, logs, {1024});
    const auto varied = render_apple_stream_full(*renderer, **prepared, plan, logs, {333, 1000, 512, 7});
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
    ok &= check(max_diff < 1.0e-4, "stream output matches the offline render_window render");
    return ok;
}

// AppleStream must honor a live listener orientation pushed through set_listener_orientation()
// (the realtime head-tracking / free-look path): turning the head left moves a front source
// toward the right ear, exactly like the offline render_window orientation (verify above).
bool verify_apple_stream_listener_orientation() {
    const auto in = write_fixture(0.0F, 8192U, 1.0F, 0.0F, false); // front source
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value(), "stream-orient: import scene")) {
        return false;
    }
    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_apple_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "stream-orient: apple prepare")) {
        return false;
    }

    // Reference: stream rendered at identity orientation.
    const auto forward = render_apple_stream_full(*renderer, **prepared, plan, logs, {1024});

    // Turned: open a fresh stream, push yaw=+90° (head turned left) live BEFORE the first
    // process(), then render. The first slice applies the pending orientation to the AU.
    auto turned_stream = renderer->open_stream(**prepared, plan, logs);
    if (!forward || !check(turned_stream.has_value(), "stream-orient: open turned stream")) {
        return false;
    }
    mradm::ListenerOrientation yaw_left{};
    yaw_left.yaw_deg = 90.0F;
    (*turned_stream)->set_listener_orientation(yaw_left);
    const uint32_t ch = (*turned_stream)->out_channels();
    std::vector<float> turned;
    std::vector<float> buf;
    while (true) {
        buf.assign(static_cast<std::size_t>(1024) * ch, 0.0F);
        auto produced = (*turned_stream)->process(std::span<float>(buf), 1024);
        if (!check(produced.has_value(), "stream-orient: turned process succeeds")) {
            return false;
        }
        if (*produced == 0) {
            break;
        }
        turned.insert(turned.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*produced * ch));
    }

    bool ok = true;
    ok &= check(rms_difference(*forward, turned) > 1.0e-4, "stream live yaw changes the binaural output");
    ok &= check(channel_energy(turned, 2U, 1U) > channel_energy(turned, 2U, 0U),
                "stream yaw=+90 (head turned left) moves a front source toward the right ear");
    return ok;
}

// If the stream is created with an initial non-identity listener orientation, head-locked live
// overrides must use that same initial pose for per-bus compensation before the first process().
bool verify_apple_stream_initial_head_locked_orientation() {
    const auto in = write_fixture(0.0F, 8192U, 1.0F, 0.0F, false); // front source
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value() && !scene->objects.empty(), "stream-headlock: import scene with an object")) {
        return false;
    }
    const std::string object_id = scene->objects.front().id;

    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;
    plan.listener_orientation.yaw_deg = 90.0F;

    auto renderer = mradm::create_apple_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "stream-headlock: apple prepare")) {
        return false;
    }

    const auto world_locked = render_apple_stream_full(*renderer, **prepared, plan, logs, {1024});
    if (!world_locked) {
        return false;
    }

    auto stream = renderer->open_stream(**prepared, plan, logs);
    if (!check(stream.has_value(), "stream-headlock: open stream")) {
        return false;
    }
    mradm::LiveOverrides ov;
    ov.revision = 1;
    ov.objects.push_back({object_id, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, "", true});
    (*stream)->set_overrides(ov);

    const uint32_t ch = (*stream)->out_channels();
    std::vector<float> head_locked;
    std::vector<float> buf;
    while (true) {
        buf.assign(static_cast<std::size_t>(1024) * ch, 0.0F);
        auto produced = (*stream)->process(std::span<float>(buf), 1024);
        if (!check(produced.has_value(), "stream-headlock: process succeeds")) {
            return false;
        }
        if (*produced == 0) {
            break;
        }
        head_locked.insert(head_locked.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*produced * ch));
    }

    const double world_l = channel_energy(*world_locked, 2U, 0U);
    const double world_r = channel_energy(*world_locked, 2U, 1U);
    const double locked_l = channel_energy(head_locked, 2U, 0U);
    const double locked_r = channel_energy(head_locked, 2U, 1U);
    const double world_imbalance = std::abs(world_r - world_l);
    const double locked_imbalance = std::abs(locked_r - locked_l);

    bool ok = true;
    ok &= check(world_r > world_l, "stream-headlock: initial yaw moves world-locked front source right");
    ok &= check(locked_imbalance < world_imbalance * 0.5,
                "stream-headlock: head-locked override compensates the initial listener yaw");
    return ok;
}

// A scene with no renderable buses (here: a muted object) must open a silent stream — not
// fail trying to drive a 0-input AUSpatialMixer — and produce full-length silence.
bool verify_apple_stream_silent() {
    constexpr uint32_t k_frames = 4096U;
    const auto in = write_fixture(0.0F, k_frames, 1.0F, 0.0F, false, /*mute=*/true);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value(), "silent: import scene")) {
        return false;
    }
    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_apple_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "silent: apple prepare")) {
        return false;
    }

    const auto out = render_apple_stream_full(*renderer, **prepared, plan, logs, {512, 100});
    if (!out) {
        return check(false, "silent: open_stream + process succeeds for empty buses");
    }
    bool ok = true;
    ok &= check(out->size() == static_cast<std::size_t>(k_frames) * 2U, "silent: full-length silence produced");
    ok &= check(std::ranges::all_of(*out, [](float v) { return v == 0.0F; }), "silent: output is all zero");

    // seek() on a silent stream must not dereference the (absent) AU / reader.
    auto stream = renderer->open_stream(**prepared, plan, logs);
    if (!stream) {
        return check(false, "silent: open_stream for seek");
    }
    ok &= check((*stream)->seek(1000).has_value(), "silent: seek succeeds without AU/reader");
    std::array<float, std::size_t{64} * 2U> buf{};
    const auto produced = (*stream)->process(std::span<float>(buf), 64);
    ok &= check(produced.has_value() && *produced == 64U, "silent: process after seek produces frames");
    ok &= check(std::ranges::all_of(buf, [](float v) { return v == 0.0F; }), "silent: post-seek output is silence");
    return ok;
}

// A live gain override (set_overrides) must scale the object's bus gain on the next block,
// so the stream output is quieter by the override factor — validating BusPlan.object_id →
// live gain map end to end through the real AUSpatialMixer.
bool verify_apple_stream_gain_override() {
    const auto in = write_fixture(45.0F, 8192U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);

    auto scene = mradm::io::import_scene(in.string());
    if (!check(scene.has_value() && !scene->objects.empty(), "override: import scene with an object")) {
        return false;
    }
    const std::string object_id = scene->objects.front().id;

    mradm::RenderPlan plan;
    plan.input_path = in.string();
    plan.output_layout = "binaural";
    plan.scene = *scene;

    auto renderer = mradm::create_apple_renderer();
    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "override: apple prepare")) {
        return false;
    }

    const auto baseline = render_apple_stream_full(*renderer, **prepared, plan, logs, {1024});
    if (!baseline) {
        return false;
    }

    // Fresh stream; apply -12.04 dB (≈ 0.25 linear) to the object before pulling.
    auto stream = renderer->open_stream(**prepared, plan, logs);
    if (!check(stream.has_value(), "override: open_stream")) {
        return false;
    }
    mradm::LiveOverrides ov;
    ov.revision = 1;
    ov.objects.push_back({object_id, -12.041F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, ""});
    (*stream)->set_overrides(ov);

    const uint32_t ch = (*stream)->out_channels();
    const std::size_t block_floats = std::size_t{1024U} * ch;
    std::vector<float> overridden;
    std::vector<float> buf(block_floats, 0.0F);
    while (true) {
        buf.assign(block_floats, 0.0F);
        auto produced = (*stream)->process(std::span<float>(buf), 1024U);
        if (!check(produced.has_value(), "override: process succeeds")) {
            return false;
        }
        if (*produced == 0) {
            break;
        }
        overridden.insert(overridden.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*produced * ch));
    }

    bool ok = check(overridden.size() == baseline->size(), "override: same frame count as baseline");

    auto buffer_rms = [](const std::vector<float>& v) {
        const double total = std::accumulate(v.begin(), v.end(), 0.0, [](double acc, float s) {
            return acc + (static_cast<double>(s) * static_cast<double>(s));
        });
        return v.empty() ? 0.0 : std::sqrt(total / static_cast<double>(v.size()));
    };
    const double b = buffer_rms(*baseline);
    const double o = buffer_rms(overridden);
    ok &= check(b > 1.0e-3, "override: baseline has signal energy");
    // 0.25 linear; allow generous tolerance for HRTF spectral coloration around the scalar.
    ok &= check(o < b * 0.5, "override: -12 dB override clearly attenuates the output");
    ok &= check(o > b * 0.1, "override: attenuation is the gain scalar, not silence");
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
    ok &= verify_spatial_presets_render_binaural();
    ok &= verify_extent_spread();
    ok &= verify_spread_mode_none_disables_extent();
    ok &= verify_channel_lock_snaps();
    ok &= verify_bed_and_lfe_routing();
    ok &= verify_22_2_lfe_routing();
    ok &= verify_apple_stream_matches_window();
    ok &= verify_apple_output_stage_matches_process();
    ok &= verify_apple_reposition_source();
    ok &= verify_apple_stream_listener_orientation();
    ok &= verify_apple_stream_initial_head_locked_orientation();
    ok &= verify_apple_stream_silent();
    ok &= verify_apple_stream_gain_override();
    ok &= verify_spatial_mixer_hrtf_modes_probe();
    ok &= verify_listener_orientation();
    return ok ? 0 : 1;
}
