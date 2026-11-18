// The macOS AUSpatialMixer backend (src/adm_apple) without the C++ SDK (R3 F3). Whole renders whose
// options the C ABI carries run through adm_render_file_ex with the Apple renderer; the factory
// presets, the offline listener orientation and the monitor streams run on the Apple island table
// (mradm_apple_islands) over a scene imported through the test FFI, as the engine drives them. The
// AUSpatialMixer HRTF mode probe calls AudioToolbox directly. macOS only.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
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
#include <nlohmann/json.hpp>

#include "adm/c_api.h"

#include "../support/adm_fixture.h"
#include "../support/apple_island_driver.h"
#include "../support/wav_fixture.h"

namespace {

using apple_test::Caller;
using apple_test::Override;
using apple_test::Prepared;
using apple_test::Settings;
using apple_test::Stream;

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

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

// BEGIN AUSpatialMixer HRTF probe (direct AudioToolbox, unchanged by R3 F3)
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

// END AUSpatialMixer HRTF probe (first part)

// The fixtures author one object (or one bed object) first, so after reassign_ids it is AO_1001.
constexpr const char* k_object_id = "AO_1001";

// Single OBJECTS object at a fixed azimuth (ADM convention: +ve = left), linear gain,
// optional ADM width (0..1) for extent spreading, and optional channelLock.
std::pair<std::shared_ptr<fixture::Document>, std::string>
make_object_doc(float azimuth,
                float gain,
                float width,
                bool channel_lock,
                bool mute = false,
                std::optional<bool> object_head_locked = std::nullopt,
                std::optional<bool> block_head_locked = std::nullopt) {
    auto doc = fixture::Document::create();
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"AppleCF"},
                                                  fixture::TypeDefinition::objects);
    {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{azimuth}, fixture::Elevation{0.0F}}};
        block.set(fixture::Gain{gain});
        block.set(fixture::JumpPosition{fixture::JumpPositionFlag{true}});
        if (width > 0.0F) {
            block.set(fixture::Width{width});
        }
        if (channel_lock) {
            fixture::ChannelLock lock;
            lock.set(fixture::ChannelLockFlag{true});
            block.set(lock);
        }
        if (block_head_locked.has_value()) {
            block.set(fixture::HeadLocked{*block_head_locked});
        }
        cf->add(block);
    }
    doc->add(cf);
    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"ApplePF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);
    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"AppleSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);
    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"AppleTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);
    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{"AppleObject"});
    obj->add_reference(uid);
    if (mute) {
        obj->set(fixture::Mute{true});
    }
    if (object_head_locked.has_value()) {
        obj->set(fixture::HeadLocked{*object_head_locked});
    }
    doc->add(obj);
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"AppleContent"});
    content->add_reference(obj);
    doc->add(content);
    auto prog = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"AppleProgramme"});
    prog->add_reference(content);
    doc->add(prog);
    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

// Two Objects blocks at the front: 0-200 ms headLocked false, 200-400 ms true.
std::pair<std::shared_ptr<fixture::Document>, std::string> make_head_locked_timeline_doc() {
    auto doc = fixture::Document::create();
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"AppleHeadTimelineCF"},
                                                  fixture::TypeDefinition::objects);
    for (const bool locked : {false, true}) {
        fixture::AudioBlockFormatObjects block{
            fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
        block.set(fixture::Rtime{fixture::Time{std::chrono::milliseconds{locked ? 200 : 0}}});
        block.set(fixture::Duration{fixture::Time{std::chrono::milliseconds{200}}});
        block.set(fixture::JumpPosition{fixture::JumpPositionFlag{true}});
        block.set(fixture::HeadLocked{locked});
        cf->add(block);
    }
    doc->add(cf);
    auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"AppleHeadTimelinePF"},
                                               fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);
    auto sf = fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"AppleHeadTimelineSF"},
                                                 fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);
    auto tf = fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"AppleHeadTimelineTF"},
                                                fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);
    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{"AppleHeadTimelineObject"});
    obj->add_reference(uid);
    doc->add(obj);
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"AppleHeadTimelineContent"});
    content->add_reference(obj);
    doc->add(content);
    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"AppleHeadTimelineProgramme"});
    programme->add_reference(content);
    doc->add(programme);
    fixture::reassign_ids(doc);
    return {doc, fixture::format_id(uid->get<fixture::AudioTrackUidId>())};
}

// A mono 48 kHz 440 Hz input whose track UIDs all play file channel 1.
std::filesystem::path write_mono(const std::shared_ptr<fixture::Document>& doc,
                                 const std::vector<std::string>& uids,
                                 std::string_view stem,
                                 uint32_t frames) {
    constexpr uint32_t k_sr = 48000U;
    std::vector<fixture::WaveAudioId> ids;
    ids.reserve(uids.size());
    std::ranges::transform(
        uids, std::back_inserter(ids), [](const std::string& uid) { return fixture::WaveAudioId{1U, uid, "", ""}; });
    const auto path = temp_path(stem, ".wav");
    std::ostringstream xml_buf;
    fixture::write_xml(xml_buf, doc);
    auto chna = std::make_shared<fixture::WaveChna>(ids);
    auto axml = std::make_shared<fixture::WaveAxml>(xml_buf.str());
    auto writer = fixture::write_wave(path.string(), 1U, k_sr, 24U, chna, axml);
    std::vector<float> samples(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        samples[i] = 0.25F * std::sin(2.0F * std::numbers::pi_v<float> * 440.0F * static_cast<float>(i) /
                                      static_cast<float>(k_sr));
    }
    writer->write(samples.data(), frames);
    return path;
}

std::filesystem::path write_fixture(float azimuth,
                                    uint32_t frames,
                                    float gain,
                                    float width,
                                    bool channel_lock,
                                    bool mute = false,
                                    std::optional<bool> object_head_locked = std::nullopt,
                                    std::optional<bool> block_head_locked = std::nullopt) {
    const auto [doc, uid] =
        make_object_doc(azimuth, gain, width, channel_lock, mute, object_head_locked, block_head_locked);
    return write_mono(doc, {uid}, "mr_apple_input", frames);
}

std::filesystem::path write_head_locked_timeline_fixture(uint32_t frames) {
    const auto [doc, uid] = make_head_locked_timeline_doc();
    return write_mono(doc, {uid}, "mr_apple_head_timeline", frames);
}

// One DirectSpeakers block: its label and, when timed, its start and duration.
struct DsBlockSpec {
    std::string label;
    std::optional<std::chrono::nanoseconds> start;
    std::optional<std::chrono::nanoseconds> duration;
};

// A DirectSpeakers channel at a fixed nominal position, added to `pf`; returns its track UID.
std::shared_ptr<fixture::AudioTrackUid> add_ds_channel(const std::shared_ptr<fixture::Document>& doc,
                                                       const std::shared_ptr<fixture::AudioPackFormat>& pf,
                                                       const std::string& name,
                                                       float azimuth,
                                                       float elevation,
                                                       const std::vector<DsBlockSpec>& blocks) {
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{name + "CF"},
                                                  fixture::TypeDefinition::direct_speakers);
    for (const auto& spec : blocks) {
        fixture::AudioBlockFormatDirectSpeakers block{fixture::SphericalSpeakerPosition{
            fixture::Azimuth{azimuth}, fixture::Elevation{elevation}, fixture::Distance{1.0F}}};
        block.add(fixture::SpeakerLabel{spec.label});
        block.set(fixture::Gain{1.0F});
        if (spec.start) {
            block.set(fixture::Rtime{fixture::Time{*spec.start}});
        }
        if (spec.duration) {
            block.set(fixture::Duration{fixture::Time{*spec.duration}});
        }
        cf->add(block);
    }
    doc->add(cf);
    pf->add_reference(cf);
    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{name + "SF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);
    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{name + "TF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);
    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);
    return uid;
}

// One DirectSpeakers object of one channel with `blocks`, 8192 frames.
std::filesystem::path write_ds_blocks_fixture(float azimuth, float elevation, const std::vector<DsBlockSpec>& blocks) {
    auto doc = fixture::Document::create();
    auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"AppleDsBlocksPF"},
                                               fixture::TypeDefinition::direct_speakers);
    auto uid = add_ds_channel(doc, pf, "AppleDsBlocks", azimuth, elevation, blocks);
    doc->add(pf);
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{"AppleDsBlocksObject"});
    obj->add_reference(uid);
    doc->add(obj);
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"AppleDsBlocksContent"});
    content->add_reference(obj);
    doc->add(content);
    auto prog = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"AppleDsBlocksProgramme"});
    prog->add_reference(content);
    doc->add(prog);
    fixture::reassign_ids(doc);
    return write_mono(doc, {fixture::format_id(uid->get<fixture::AudioTrackUidId>())}, "mr_apple_ds_input", 8192U);
}

// A single DirectSpeakers bed channel at a fixed speaker position and label.
std::filesystem::path write_ds_fixture(float azimuth, float elevation, const std::string& label) {
    return write_ds_blocks_fixture(azimuth, elevation, {{label, std::nullopt, std::nullopt}});
}

// An Objects object at `azimuth` and a DirectSpeakers object of M+030 and LFE1, all three tracks on
// the one file channel.
std::filesystem::path write_mixed_paths_fixture(float azimuth) {
    const auto [doc, object_uid] = make_object_doc(azimuth, 1.0F, 0.0F, false);
    auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"AppleMixedDsPF"},
                                               fixture::TypeDefinition::direct_speakers);
    auto front = add_ds_channel(doc, pf, "AppleMixedFront", 30.0F, 0.0F, {{"M+030", std::nullopt, std::nullopt}});
    auto lfe = add_ds_channel(doc, pf, "AppleMixedLfe", 0.0F, 0.0F, {{"LFE1", std::nullopt, std::nullopt}});
    doc->add(pf);
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{"AppleMixedBed"});
    obj->add_reference(front);
    obj->add_reference(lfe);
    doc->add(obj);
    doc->get_elements<fixture::AudioContent>().front()->add_reference(obj);
    fixture::reassign_ids(doc);
    const auto uid = [](const std::shared_ptr<fixture::AudioTrackUid>& u) {
        return fixture::format_id(u->get<fixture::AudioTrackUidId>());
    };
    const auto object_track = doc->get_elements<fixture::AudioTrackUid>().front();
    return write_mono(doc, {uid(object_track), uid(front), uid(lfe)}, "mr_apple_mixed_input", 8192U);
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

double
channel_energy(const std::vector<float>& samples, uint32_t channels, uint32_t ch, std::size_t begin, std::size_t end) {
    double energy = 0.0;
    for (std::size_t frame = begin; frame < end; ++frame) {
        const double sample = samples[(frame * channels) + ch];
        energy += sample * sample;
    }
    return energy;
}

double total_energy(const std::vector<float>& samples, uint16_t channels) {
    double total = 0.0;
    for (uint16_t c = 0; c < channels; ++c) {
        total += channel_energy(samples, channels, c);
    }
    return total;
}

// Number of output channels carrying more than `frac` of the total energy.
int active_channels(const std::vector<float>& samples, uint16_t channels, double frac) {
    const double total = total_energy(samples, channels);
    int active = 0;
    for (uint16_t c = 0; c < channels; ++c) {
        if (channel_energy(samples, channels, c) > frac * total) {
            ++active;
        }
    }
    return active;
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

// A C ABI render of `input` with the Apple renderer to `layout` (peak limiting off) after
// `configure`, read back when it has `channels` channels.
std::optional<std::vector<float>> render_capi(const std::filesystem::path& input,
                                              std::string_view stem,
                                              const std::string& layout,
                                              uint16_t channels,
                                              const std::function<void(adm_render_options_t*)>& configure = {}) {
    const auto out = temp_path(stem, ".wav");
    FileGuard out_guard(out);
    adm_context_t* context = adm_create_context();
    adm_render_options_t* options = adm_create_render_options();
    adm_render_options_set_renderer(options, ADM_RENDERER_APPLE);
    adm_render_options_set_output_layout(options, layout.c_str());
    adm_render_options_set_peak_limit(options, 0);
    if (configure) {
        configure(options);
    }
    adm_render_result_t* result = nullptr;
    const auto code =
        adm_render_file_ex(context, input.string().c_str(), out.string().c_str(), options, nullptr, nullptr, &result);
    if (code != ADM_ERROR_OK) {
        std::cerr << "context: " << adm_context_last_error_message(context) << "\n";
    }
    adm_destroy_render_result(result);
    adm_destroy_render_options(options);
    adm_destroy_context(context);
    if (!check(code == ADM_ERROR_OK, "apple render succeeds")) {
        return std::nullopt;
    }
    auto audio = apple_test::read_wav(out);
    if (!check(audio.has_value() && audio->channels == channels, "apple output has the layout's channels")) {
        return std::nullopt;
    }
    return std::move(audio->samples);
}

// A C ABI render of a one-object fixture.
std::optional<std::vector<float>> render_apple(float azimuth,
                                               std::string_view stem,
                                               const std::string& layout,
                                               uint16_t channels,
                                               float gain = 1.0F,
                                               float width = 0.0F,
                                               bool channel_lock = false,
                                               const std::function<void(adm_render_options_t*)>& configure = {}) {
    const auto in = write_fixture(azimuth, 8192U, gain, width, channel_lock);
    FileGuard in_guard(in);
    return render_capi(in, stem, layout, channels, configure);
}

std::optional<std::vector<float>> render_apple_stereo(float azimuth, std::string_view stem, float gain = 1.0F) {
    return render_apple(azimuth, stem, "binaural", 2U, gain);
}

// An island render (prepare + render) of `settings` to a WAVE file, read back with `channels`.
std::optional<std::vector<float>> render_island(const Settings& settings, uint16_t channels, std::string_view stem) {
    Prepared prepared(settings);
    if (!check(prepared.ok(), "apple island prepares")) {
        std::cerr << "context: " << prepared.reply.message << "\n";
        return std::nullopt;
    }
    const auto out = temp_path(stem, ".wav");
    FileGuard out_guard(out);
    Caller caller;
    const auto reply = apple_test::render(prepared, settings, out, caller);
    if (!check(reply.ok(), "apple island renders")) {
        std::cerr << "context: " << reply.message << "\n";
        return std::nullopt;
    }
    auto audio = apple_test::read_wav(out);
    if (!check(audio.has_value() && audio->channels == channels, "apple island output has the layout's channels")) {
        return std::nullopt;
    }
    return std::move(audio->samples);
}

Settings settings_for(const std::filesystem::path& input, std::string layout = "binaural") {
    Settings settings;
    settings.input = input.string();
    settings.layout = std::move(layout);
    return settings;
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

// The Apple backend's capability record as the C ABI publishes it (adm_capabilities_json).
bool verify_capabilities() {
    adm_context_t* context = adm_create_context();
    char* raw = nullptr;
    const auto code = adm_capabilities_json(context, &raw);
    const std::string text = raw != nullptr ? raw : "";
    adm_free_string(raw);
    adm_destroy_context(context);
    if (!check(code == ADM_ERROR_OK, "capabilities JSON")) {
        return false;
    }
    const auto json = nlohmann::json::parse(text);
    const auto& backends = json.at("backends");
    const auto apple = std::find_if(backends.begin(), backends.end(), [](const nlohmann::json& b) {
        return b.value("renderer", std::string{}) == "apple";
    });
    if (!check(apple != backends.end(), "the macOS build publishes the apple backend")) {
        return false;
    }
    const auto& caps = *apple;
    bool ok = true;
    ok &= check(caps.at("backend_name") == "apple", "backend name");
    ok &= check(caps.at("supports_objects") == true, "objects supported");
    ok &= check(caps.at("supports_direct_speakers") == true, "direct speakers supported");
    ok &= check(caps.at("supports_hoa") == false, "hoa unsupported");
    ok &= check(caps.at("supports_object_divergence") == true, "object divergence advertised");
    ok &= check(caps.at("supports_diffuse") == false, "diffuse unsupported");
    const auto has_layout = [&](const std::string& id, uint16_t channels, bool binaural) {
        const auto& layouts = caps.at("layouts");
        return std::any_of(layouts.begin(), layouts.end(), [&](const nlohmann::json& l) {
            return l.at("id") == id && l.at("channel_count") == channels && l.at("is_binaural") == binaural;
        });
    };
    for (const auto& layout : k_expected_layouts) {
        ok &= check(has_layout(layout.id, layout.channels, layout.binaural), layout.display_name);
    }
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
    bool ok = true;
    ok &= check(channel_energy(*left, 2U, 0U) > channel_energy(*left, 2U, 1U),
                "ADM azimuth +90 (left) is louder in left output channel");
    ok &= check(channel_energy(*right, 2U, 1U) > channel_energy(*right, 2U, 0U),
                "ADM azimuth -90 (right) is louder in right output channel");
    return ok;
}

// kSpatialMixerParam_Gain is in dB; a linear gain of 0 must map to the -120 dB floor
// (mute), not 0 dB (unity). A gain=0 object must therefore render as (near) silence.
bool verify_zero_gain_is_silent() {
    const auto out = render_apple_stereo(0.0F, "mr_apple_silence", 0.0F);
    if (!out) {
        return false;
    }
    return check(total_energy(*out, 2U) < 1.0e-2, "gain=0 object renders as silence (dB floor, not 0 dB unity)");
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

// The public compatibility switch must reach AUSpatialMixer and configure every
// speaker bus without producing an unsupported-property error.
bool verify_speaker_rendering_flags_switch() {
    const auto apple_native = render_apple(45.0F, "mr_apple_flags_on", "4+7+0", 12U, 1.0F, 0.0F, false, [](auto* o) {
        adm_render_options_set_apple_speaker_rendering_flags(o, 1);
    });
    return check(apple_native.has_value(), "Apple speaker rendering flags mode renders successfully");
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

// A binaural HRTF render requested via the legacy "0+2+0" alias must tag the CAF
// container as Binaural (106), not a generic two-channel layout (101).
bool verify_binaural_container_tag() {
    const auto in = write_fixture(0.0F, 4096U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    const auto out = temp_path("mr_apple_tag", ".caf");
    FileGuard out_guard(out);
    adm_context_t* context = adm_create_context();
    adm_render_options_t* options = adm_create_render_options();
    adm_render_options_set_renderer(options, ADM_RENDERER_APPLE);
    adm_render_options_set_output_layout(options, "0+2+0"); // legacy alias -> must normalize to binaural tag
    adm_render_options_set_peak_limit(options, 0);
    const auto code =
        adm_render_file_ex(context, in.string().c_str(), out.string().c_str(), options, nullptr, nullptr, nullptr);
    adm_destroy_render_options(options);
    adm_destroy_context(context);
    if (!check(code == ADM_ERROR_OK, "apple 0+2+0 → CAF render succeeds")) {
        return false;
    }
    const auto tag = caf_channel_layout_tag(out);
    constexpr uint32_t k_binaural = (106U << 16) | 2U;
    constexpr uint32_t k_stereo = (101U << 16) | 2U;
    if (!check(tag.has_value(), "CAF has a chan chunk")) {
        return false;
    }
    bool ok = check(*tag == k_binaural, "CAF tagged Binaural (106), not Stereo");
    ok &= check(*tag != k_stereo, "CAF not tagged plain Stereo for a binaural render");
    return ok;
}

bool verify_render_window_frame_count() {
    const auto in = write_fixture(0.0F, 4096U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    const auto out = temp_path("mr_apple_window", ".wav");
    FileGuard out_guard(out);
    adm_context_t* context = adm_create_context();
    adm_render_options_t* options = adm_create_render_options();
    adm_render_options_set_renderer(options, ADM_RENDERER_APPLE);
    adm_render_options_set_output_layout(options, "binaural");
    adm_render_options_set_peak_limit(options, 0);
    adm_render_options_set_render_start_sec(options, 1024.0 / 48000.0);
    adm_render_options_set_render_end_sec(options, 2048.0 / 48000.0);
    const auto code =
        adm_render_file_ex(context, in.string().c_str(), out.string().c_str(), options, nullptr, nullptr, nullptr);
    adm_destroy_render_options(options);
    adm_destroy_context(context);
    if (!check(code == ADM_ERROR_OK, "apple render window succeeds")) {
        return false;
    }
    const auto audio = apple_test::read_wav(out);
    if (!check(audio.has_value(), "apple render window output opens")) {
        return false;
    }
    bool ok = true;
    ok &= check(audio->channels == 2U, "apple render window output is binaural");
    ok &= check(audio->frames == 1024U, "apple render window writes only requested frames");
    return ok;
}

// The AUSpatialMixer factory presets (island settings; the C ABI has no offline preset option).
bool verify_spatial_presets_render_binaural() {
    const auto in = write_fixture(0.0F, 8192U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    bool ok = true;
    for (const uint32_t preset : {1U, 2U}) { // headphone default, headphone movie
        auto settings = settings_for(in);
        settings.spatial_preset = preset;
        const auto rendered = render_island(settings, 2U, "mr_apple_spatial_preset");
        if (!check(rendered.has_value(), "apple binaural factory preset renders")) {
            ok = false;
            continue;
        }
        ok &= check(total_energy(*rendered, 2U) > 1.0e-5, "apple binaural factory preset output is not silent");
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

// --speaker-spread-mode none must disable the extent cloud: a wide object renders as a
// point (localized), aligning Apple with the VBAP/binaural spread-mode capability gating.
bool verify_spread_mode_none_disables_extent() {
    const auto spread_mode = [](adm_speaker_spread_mode_t mode) {
        return [mode](adm_render_options_t* o) { adm_render_options_set_speaker_spread_mode(o, mode); };
    };
    const auto spread = render_apple(
        0.0F, "mr_apple_spread_auto", "4+7+0", 12U, 1.0F, 0.8F, false, spread_mode(ADM_SPEAKER_SPREAD_AUTOMATIC));
    const auto none = render_apple(
        0.0F, "mr_apple_spread_none", "4+7+0", 12U, 1.0F, 0.8F, false, spread_mode(ADM_SPEAKER_SPREAD_NONE));
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

// A single DirectSpeakers bed channel through the C ABI to `layout` with a routing mode.
std::optional<std::vector<float>>
render_ds(float azimuth,
          float elevation,
          const std::string& label,
          const std::string& layout,
          uint16_t channels,
          std::string_view stem,
          adm_direct_speakers_routing_mode_t routing = ADM_DIRECT_SPEAKERS_ROUTING_AUTOMATIC) {
    const auto in = write_ds_fixture(azimuth, elevation, label);
    FileGuard in_guard(in);
    return render_capi(in, stem, layout, channels, [routing](adm_render_options_t* o) {
        adm_render_options_set_direct_speakers_routing_mode(o, routing);
    });
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

bool verify_direct_speakers_routing_modes() {
    const auto automatic =
        render_ds(45.0F, 15.0F, "U+045", "9+10+3", 24U, "mr_apple_ds_auto", ADM_DIRECT_SPEAKERS_ROUTING_AUTOMATIC);
    const auto label =
        render_ds(45.0F, 15.0F, "U+045", "9+10+3", 24U, "mr_apple_ds_label", ADM_DIRECT_SPEAKERS_ROUTING_LABEL);
    const auto position =
        render_ds(45.0F, 15.0F, "U+045", "9+10+3", 24U, "mr_apple_ds_position", ADM_DIRECT_SPEAKERS_ROUTING_POSITION);
    if (!automatic || !label || !position) {
        return false;
    }

    bool ok = true;
    ok &= check(*automatic == *label, "Apple speaker automatic DirectSpeakers routing resolves to label");
    ok &= check(loudest_channel(*label, 24U) == 12U, "Apple label routing sends U+045 directly to ch12");
    for (uint16_t channel = 0; channel < 24U; ++channel) {
        if (channel != 12U) {
            ok &= check(channel_energy(*label, 24U, channel) < 1.0e-12,
                        "Apple label routing is one-hot with no output-slot leakage");
        }
    }
    ok &= check(active_channels(*position, 24U, 0.01) >= 2,
                "Apple position routing preserves the AmbienceBed spatialization path");
    ok &= check(channel_energy(*position, 24U, 3U) < 1.0e-12 && channel_energy(*position, 24U, 9U) < 1.0e-12,
                "Apple position-routed non-LFE source never enters either LFE output");

    const auto unknown = render_ds(
        45.0F, 15.0F, "UNKNOWN", "9+10+3", 24U, "mr_apple_ds_unknown_fallback", ADM_DIRECT_SPEAKERS_ROUTING_LABEL);
    if (!unknown) {
        return false;
    }
    ok &= check(active_channels(*unknown, 24U, 0.01) >= 2,
                "Apple unmatched label falls back to AmbienceBed spatialization, not nearest one-hot");
    ok &= check(channel_energy(*unknown, 24U, 3U) < 1.0e-12 && channel_energy(*unknown, 24U, 9U) < 1.0e-12,
                "Apple unmatched non-LFE label fallback never enters either LFE output");

    const auto alias =
        render_ds(0.0F, 0.0F, "L", "9+10+3", 24U, "mr_apple_ds_alias_l", ADM_DIRECT_SPEAKERS_ROUTING_LABEL);
    if (!alias) {
        return false;
    }
    ok &= check(loudest_channel(*alias, 24U) == 6U, "Apple alias L routes to 22.2 M+030 (ch6)");

    const auto position_lfe =
        render_ds(0.0F, 0.0F, "LFE", "9+10+3", 24U, "mr_apple_ds_position_lfe", ADM_DIRECT_SPEAKERS_ROUTING_POSITION);
    if (!position_lfe) {
        return false;
    }
    ok &= check(loudest_channel(*position_lfe, 24U) == 3U, "Apple position mode preserves dedicated LFE1 routing");

    // Binaural accepts automatic and position routing; an explicit label routing is unsupported.
    const auto routing_input = write_fixture(0.0F, 4096U, 1.0F, 0.0F, false);
    FileGuard routing_guard{routing_input};
    auto settings = settings_for(routing_input);
    const auto prepare_code = [&](uint32_t routing) {
        settings.routing = routing;
        return Prepared(settings).reply.code;
    };
    ok &= check(prepare_code(0U) == 0, "Apple binaural automatic DirectSpeakers routing prepares");
    ok &= check(prepare_code(2U) == 0, "Apple binaural explicit position routing prepares");
    ok &= check(prepare_code(1U) == ADM_ERROR_UNSUPPORTED, "Apple binaural explicit label routing is unsupported");
    return ok;
}

// An ADM DirectSpeakers block always has a nominal position (only a channel bed's LFE has none), so
// the front-centre fallback of a block without one is checked in Rust (mradm-render tests/apple.rs).
bool verify_direct_speakers_missing_position_fallback() {
    // M+135 is not a 5.1 output: label routing spatializes it from its label direction, not from
    // the block's front-left nominal position.
    const auto input = write_ds_fixture(30.0F, 0.0F, "M+135");
    FileGuard input_guard{input};
    auto settings = settings_for(input, "0+5+0");
    settings.routing = 1U; // label
    Prepared prepared(settings);
    const auto out = temp_path("mr_apple_ds_rear_label_fallback", ".wav");
    FileGuard out_guard{out};
    Caller caller;
    if (!check(prepared.ok() && apple_test::render(prepared, settings, out, caller).ok(),
               "Apple rear-label fallback render succeeds")) {
        return false;
    }
    const auto audio = apple_test::read_wav(out);
    if (!check(audio.has_value() && audio->channels == 6U, "Apple rear-label fallback output opens")) {
        return false;
    }
    const auto& samples = audio->samples;
    const double rear_left = channel_energy(samples, 6U, 4U);
    const double front =
        channel_energy(samples, 6U, 0U) + channel_energy(samples, 6U, 1U) + channel_energy(samples, 6U, 2U);
    const auto warned = [&](std::string_view needle) {
        return prepared.caller.warned(needle) || caller.warned(needle);
    };
    bool ok = true;
    ok &= check(rear_left > front, "Apple M+135 label direction keeps the bed behind when rendered to 5.1");
    ok &=
        check(warned("spatializing from label direction"), "Apple absent output slot reports label-direction fallback");
    ok &= check(!warned("no nominal position"), "Apple known rear label does not fall back to front-centre");
    return ok;
}

bool verify_mixed_object_direct_and_lfe_paths() {
    constexpr uint16_t k_channels = 24U;
    // An Objects object and a DirectSpeakers object of M+030 and LFE1, all on file channel 1.
    const auto input = write_mixed_paths_fixture(-90.0F);
    FileGuard input_guard{input};
    const auto samples = render_capi(input, "mr_apple_mixed_paths", "9+10+3", k_channels, [](auto* o) {
        adm_render_options_set_direct_speakers_routing_mode(o, ADM_DIRECT_SPEAKERS_ROUTING_LABEL);
    });
    if (!check(samples.has_value(), "Apple mixed Objects/DirectSpeakers/LFE render succeeds")) {
        return false;
    }
    double spatial_energy = 0.0;
    for (uint16_t channel = 0; channel < k_channels; ++channel) {
        if (channel != 3U && channel != 6U && channel != 9U) {
            spatial_energy += channel_energy(*samples, k_channels, channel);
        }
    }
    bool ok = true;
    ok &= check(spatial_energy > 1.0e-6, "Apple mixed render keeps the Objects spatial path active");
    ok &= check(channel_energy(*samples, k_channels, 6U) > 1.0e-6,
                "Apple mixed render keeps the label-direct M+030 path active");
    ok &=
        check(channel_energy(*samples, k_channels, 3U) > 1.0e-6, "Apple mixed render keeps the LFE1 side path active");
    ok &= check(channel_energy(*samples, k_channels, 9U) == 0.0,
                "Apple mixed render does not leak LFE1 into the LFE2 slot");
    return ok;
}

// Listener head orientation: a +90° yaw (head turned left) must move a front point source
// toward the right ear, and the binaural output must differ from the identity (head-forward)
// render. Locks the Apple backend's HeadYaw sign convention. Offline orientation is an island
// setting (the C ABI has no offline listener orientation).
bool verify_listener_orientation() {
    const auto render_yaw = [](float yaw, std::optional<bool> object_locked, std::optional<bool> block_locked) {
        const auto in = write_fixture(0.0F, 8192U, 1.0F, 0.0F, false, false, object_locked, block_locked);
        FileGuard in_guard(in);
        auto settings = settings_for(in);
        settings.orientation = {yaw, 0.0F, 0.0F};
        return render_island(settings, 2U, "mr_apple_yaw");
    };
    const auto forward = render_yaw(0.0F, std::nullopt, std::nullopt);
    const auto turned = render_yaw(90.0F, std::nullopt, std::nullopt);
    const auto adm_locked = render_yaw(90.0F, true, std::nullopt);
    const auto block_unlocked = render_yaw(90.0F, true, false);
    if (!forward || !turned || !adm_locked || !block_unlocked) {
        return check(false, "listener-orientation renders succeed");
    }
    const double id_l = channel_energy(*forward, 2U, 0U);
    const double id_r = channel_energy(*forward, 2U, 1U);
    const double turn_l = channel_energy(*turned, 2U, 0U);
    const double turn_r = channel_energy(*turned, 2U, 1U);
    const double locked_l = channel_energy(*adm_locked, 2U, 0U);
    const double locked_r = channel_energy(*adm_locked, 2U, 1U);
    bool ok = true;
    ok &= check(rms_difference(*forward, *turned) > 1.0e-4, "listener yaw changes the binaural output");
    ok &= check(std::abs(id_l - id_r) < 0.25 * (id_l + id_r), "identity orientation keeps a front source L/R balanced");
    ok &= check(turn_r > turn_l, "yaw=+90 (head turned left) moves a front source toward the right ear");
    ok &= check(std::abs(locked_l - locked_r) < std::abs(turn_l - turn_r) * 0.5,
                "offline Apple render uses source ADM headLocked");
    ok &= check(channel_energy(*block_unlocked, 2U, 1U) > channel_energy(*block_unlocked, 2U, 0U),
                "offline Apple render lets explicit block false override AudioObject true");
    return ok;
}

// A whole-timeline monitor stream of `prepared`, pulled in the repeating `chunks` pattern.
std::optional<std::vector<float>> stream_full(const Prepared& prepared,
                                              const Settings& settings,
                                              const std::vector<std::size_t>& chunks,
                                              const std::vector<Override>& overrides = {}) {
    auto out = apple_test::stream_all(prepared, settings, chunks, overrides);
    check(out.has_value(), "apple stream opens and processes");
    return out;
}

// The offline island render of `prepared` (the stream's reference), read back with `channels`.
std::optional<std::vector<float>>
offline_reference(const Prepared& prepared, const Settings& settings, uint16_t channels, std::string_view stem) {
    const auto out = temp_path(stem, ".wav");
    FileGuard out_guard(out);
    Caller caller;
    if (!check(apple_test::render(prepared, settings, out, caller).ok(), "apple island offline reference renders")) {
        return std::nullopt;
    }
    auto audio = apple_test::read_wav(out);
    if (!check(audio.has_value() && audio->channels == channels, "apple island offline reference opens")) {
        return std::nullopt;
    }
    return std::move(audio->samples);
}

bool verify_apple_direct_stream_and_dynamic_blocks() {
    constexpr uint16_t k_channels = 12U;
    constexpr uint64_t k_total_frames = 8192U;
    // Deliberately not aligned to k_render_block: direct-bus target changes must
    // occur on the metadata sample, not at the next AUSpatialMixer slice.
    constexpr uint64_t k_boundary = 4099U;
    // 85.395833 ms is frame 4099 at 48 kHz; the second block ends at the file's end (frame 8192).
    const auto input =
        write_ds_blocks_fixture(30.0F,
                                0.0F,
                                {{"M+030", std::chrono::nanoseconds{0}, std::chrono::nanoseconds{85395833}},
                                 {"M-030", std::chrono::nanoseconds{85395833}, std::chrono::nanoseconds{85270834}}});
    FileGuard input_guard{input};
    auto settings = settings_for(input, "4+7+0");
    settings.routing = 1U; // label
    Prepared prepared(settings);
    if (!check(prepared.ok(), "direct stream: Apple label prepare")) {
        return false;
    }
    const auto reference = offline_reference(prepared, settings, k_channels, "mr_apple_direct_stream_ref");
    if (!reference) {
        return false;
    }
    const auto range_energy = [&](uint64_t begin, uint64_t end, uint16_t channel) {
        return channel_energy(*reference, k_channels, channel, begin, end);
    };
    bool ok = true;
    ok &= check(range_energy(0, k_boundary, 0U) > 1.0e-6 && range_energy(0, k_boundary, 1U) == 0.0,
                "Apple direct bus sends the first block to M+030 only");
    ok &= check(range_energy(k_boundary, k_total_frames, 1U) > 1.0e-6 &&
                    range_energy(k_boundary, k_total_frames, 0U) == 0.0,
                "Apple direct bus switches target exactly at the second block boundary");

    const auto streamed = stream_full(prepared, settings, {333U, 1000U, 7U, 512U});
    if (!streamed) {
        return false;
    }
    ok &= check(*streamed == *reference, "Apple direct bus streaming is bit-identical to offline rendering");

    Stream seek_stream(prepared, settings);
    if (!check(seek_stream.ok() && seek_stream.seek(k_boundary), "Apple direct bus seek succeeds")) {
        return false;
    }
    std::vector<float> seek_block(std::size_t{512U} * k_channels, 0.0F);
    const auto seek_frames = seek_stream.process(seek_block, 512U);
    ok &= check(seek_frames == 512U, "Apple direct bus renders after seek");
    if (seek_frames == 512U) {
        ok &= check(std::equal(seek_block.begin(),
                               seek_block.end(),
                               reference->begin() + static_cast<std::ptrdiff_t>(k_boundary * k_channels)),
                    "Apple direct bus seek restores event target and source position");
    }

    Stream loop_stream(prepared, settings);
    if (!check(loop_stream.ok(), "Apple direct bus loop stream opens")) {
        return false;
    }
    std::vector<float> loop_block(std::size_t{512U} * k_channels, 0.0F);
    const auto initial = loop_stream.process(loop_block, 512U);
    const bool repositioned = loop_stream.reposition_source(0);
    loop_stream.loop_render_reset(0);
    std::ranges::fill(loop_block, 0.0F);
    const auto looped = loop_stream.process(loop_block, 512U);
    ok &= check(initial.has_value() && repositioned && looped == 512U, "Apple direct bus loop reset succeeds");
    if (looped == 512U) {
        ok &= check(std::equal(loop_block.begin(), loop_block.end(), reference->begin()),
                    "Apple direct bus loop reset restores the first event target");
    }

    const auto gained = stream_full(prepared, settings, {512U}, {{k_object_id, -12.0412F, false, std::nullopt, ""}});
    const auto muted = stream_full(prepared, settings, {512U}, {{k_object_id, -12.0412F, true, std::nullopt, ""}});
    if (!gained || !muted) {
        return false;
    }
    const double reference_energy = total_energy(*reference, k_channels);
    const double gained_energy = total_energy(*gained, k_channels);
    ok &= check(gained_energy > reference_energy * 0.05 && gained_energy < reference_energy * 0.08,
                "Apple direct bus live gain override applies the expected scalar");
    ok &= check(std::ranges::all_of(*muted, [](float sample) { return sample == 0.0F; }),
                "Apple direct bus live mute produces exact silence");
    return ok;
}

// AppleStream (realtime) must reproduce the offline render, and its output must be independent of
// the caller's pull chunk size (FIFO + canonical k_render_block).
bool verify_apple_stream_matches_window() {
    const auto in = write_fixture(45.0F, 8192U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    const auto settings = settings_for(in);
    Prepared prepared(settings);
    if (!check(prepared.ok(), "stream: apple prepare")) {
        return false;
    }
    const auto ref = offline_reference(prepared, settings, 2U, "mr_apple_stream_ref");
    const auto uniform = stream_full(prepared, settings, {1024});
    const auto varied = stream_full(prepared, settings, {333, 1000, 512, 7});
    if (!ref || !uniform || !varied) {
        return false;
    }
    bool ok = true;
    ok &= check(uniform->size() == ref->size(), "stream output frame count matches the offline render");
    ok &= check(*uniform == *varied, "stream output is identical regardless of pull chunk size (FIFO correct)");
    double max_diff = 0.0;
    const std::size_t n = std::min(uniform->size(), ref->size());
    for (std::size_t i = 0; i < n; ++i) {
        max_diff = std::max(max_diff, std::fabs(static_cast<double>((*uniform)[i]) - static_cast<double>((*ref)[i])));
    }
    ok &= check(max_diff < 1.0e-4, "stream output matches the offline render");
    return ok;
}

// Output-stage rendering (方案 B): produce_intermediate (worker, reads source PCM) + render_output
// (audio callback, applies the orientation + runs the AU) with a STATIC identity orientation must be
// bit-identical to the single-stage process() path — same AU, same per-block params, just split into
// two calls so the head pose can enter at playout. This is the core correctness gate for the split.
bool verify_apple_output_stage_matches_process() {
    const auto in = write_fixture(45.0F, 8192U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    const auto settings = settings_for(in);
    Prepared prepared(settings);
    if (!check(prepared.ok(), "output-stage: apple prepare")) {
        return false;
    }
    // Reference: single-stage process() in fixed 512-frame chunks.
    const auto reference = stream_full(prepared, settings, {512});
    if (!reference) {
        return false;
    }
    Stream stream(prepared, settings);
    if (!check(stream.ok() && stream.renders_orientation_at_output(),
               "output-stage: binaural opts into output-stage")) {
        return false;
    }
    const uint32_t in_ch = stream.intermediate_channels();
    const uint32_t out_ch = stream.out_channels();
    std::vector<float> out;
    std::vector<float> interm;
    std::vector<float> buf;
    constexpr std::size_t k = 512;
    while (true) {
        interm.assign(k * in_ch, 0.0F);
        const auto got = stream.produce_intermediate(interm, k);
        if (!check(got.has_value(), "output-stage: produce_intermediate succeeds")) {
            return false;
        }
        if (*got == 0) {
            break;
        }
        buf.assign(*got * out_ch, 0.0F);
        const std::size_t rendered = stream.render_output(interm.data(), *got * in_ch, buf, *got);
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(rendered * out_ch));
    }
    bool ok = check(out.size() == reference->size(), "output-stage: same total samples as process()");
    if (ok) {
        ok = check(out == *reference, "output-stage: bit-identical to single-stage process()");
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
    const auto settings = settings_for(in);
    Prepared prepared(settings);
    if (!check(prepared.ok(), "reposition-source: apple prepare")) {
        return false;
    }
    Stream stream(prepared, settings);
    if (!check(stream.ok(), "reposition-source: open stream")) {
        return false;
    }
    const uint32_t in_ch = stream.intermediate_channels();
    constexpr std::size_t k = 1024;
    std::vector<float> block_a(k * in_ch, 0.0F);
    std::vector<float> block_b(k * in_ch, 0.0F);
    std::vector<float> block_a2(k * in_ch, 0.0F);
    const auto got_a = stream.produce_intermediate(block_a, k);   // source [0, k)
    const auto got_b = stream.produce_intermediate(block_b, k);   // source [k, 2k)
    const bool rep = stream.reposition_source(0);                 // Stage A wrap → 0
    const auto got_a2 = stream.produce_intermediate(block_a2, k); // source [0, k) again
    bool ok = check(got_a == k && got_b == k && rep && got_a2 == k, "reposition-source: produce + reposition succeed");
    ok &= check(block_a2 == block_a, "reposition-source: re-reads the source from the wrap point");
    ok &= check(block_a != block_b, "reposition-source: distinct source blocks (sanity)");
    return ok;
}

// AppleStream must honor a live listener orientation (the realtime head-tracking / free-look
// path): turning the head left moves a front source toward the right ear, exactly like the
// offline orientation (verify_listener_orientation).
bool verify_apple_stream_listener_orientation() {
    const auto in = write_fixture(0.0F, 8192U, 1.0F, 0.0F, false); // front source
    FileGuard in_guard(in);
    const auto settings = settings_for(in);
    Prepared prepared(settings);
    if (!check(prepared.ok(), "stream-orient: apple prepare")) {
        return false;
    }
    const auto forward = stream_full(prepared, settings, {1024});
    // Turned: a fresh stream with yaw=+90° (head turned left) pushed BEFORE the first process();
    // the first slice applies the pending orientation to the AU.
    Stream turned_stream(prepared, settings);
    if (!forward || !check(turned_stream.ok(), "stream-orient: open turned stream")) {
        return false;
    }
    turned_stream.set_orientation(90.0F);
    const auto turned = turned_stream.pull_all({1024});
    if (!check(turned.has_value(), "stream-orient: turned process succeeds")) {
        return false;
    }
    bool ok = true;
    ok &= check(rms_difference(*forward, *turned) > 1.0e-4, "stream live yaw changes the binaural output");
    ok &= check(channel_energy(*turned, 2U, 1U) > channel_energy(*turned, 2U, 0U),
                "stream yaw=+90 (head turned left) moves a front source toward the right ear");
    return ok;
}

// If the stream is created with an initial non-identity listener orientation, head-locked live
// overrides must use that same initial pose for per-bus compensation before the first process().
bool verify_apple_stream_initial_head_locked_orientation() {
    const auto in = write_fixture(0.0F, 8192U, 1.0F, 0.0F, false); // front source
    FileGuard in_guard(in);
    auto settings = settings_for(in);
    settings.orientation = {90.0F, 0.0F, 0.0F};
    Prepared prepared(settings);
    if (!check(prepared.ok(), "stream-headlock: apple prepare")) {
        return false;
    }
    const auto world_locked = stream_full(prepared, settings, {1024});
    const auto head_locked = stream_full(prepared, settings, {1024}, {{k_object_id, 0.0F, false, true, ""}});
    if (!world_locked || !head_locked) {
        return false;
    }
    const double world_l = channel_energy(*world_locked, 2U, 0U);
    const double world_r = channel_energy(*world_locked, 2U, 1U);
    const double locked_l = channel_energy(*head_locked, 2U, 0U);
    const double locked_r = channel_energy(*head_locked, 2U, 1U);
    bool ok = true;
    ok &= check(world_r > world_l, "stream-headlock: initial yaw moves world-locked front source right");
    ok &= check(std::abs(locked_r - locked_l) < std::abs(world_r - world_l) * 0.5,
                "stream-headlock: head-locked override compensates the initial listener yaw");
    return ok;
}

// The AudioObject's headLocked reaches its block (the inheritance itself is an import rule, tested in
// mradm-scene): without a live value the stream follows ADM, a gain-only override does not shadow
// it, and an explicit live false follows the listener rotation.
bool verify_apple_stream_adm_head_locked() {
    const auto in = write_fixture(0.0F, 8192U, 1.0F, 0.0F, false, false, true);
    FileGuard in_guard(in);
    auto settings = settings_for(in);
    settings.orientation = {90.0F, 0.0F, 0.0F};
    Prepared prepared(settings);
    if (!check(prepared.ok(), "stream-adm-headlock: Apple prepare")) {
        return false;
    }
    const auto inherited = stream_full(prepared, settings, {1024U});
    const auto gained = stream_full(prepared, settings, {1024U}, {{k_object_id, -3.0F, false, std::nullopt, ""}});
    const auto unlocked = stream_full(prepared, settings, {1024U}, {{k_object_id, 0.0F, false, false, ""}});
    if (!inherited || !gained || !unlocked) {
        return false;
    }
    const double unlocked_imbalance = std::abs(channel_energy(*unlocked, 2U, 1U) - channel_energy(*unlocked, 2U, 0U));
    bool ok = true;
    ok &= check(channel_energy(*unlocked, 2U, 1U) > channel_energy(*unlocked, 2U, 0U),
                "stream-adm-headlock: explicit live false follows listener rotation");
    ok &= check(std::abs(channel_energy(*inherited, 2U, 1U) - channel_energy(*inherited, 2U, 0U)) <
                    unlocked_imbalance * 0.5,
                "stream-adm-headlock: no live value inherits ADM");
    ok &= check(std::abs(channel_energy(*gained, 2U, 1U) - channel_energy(*gained, 2U, 0U)) < unlocked_imbalance * 0.5,
                "stream-adm-headlock: gain-only override does not shadow ADM");
    return ok;
}

bool verify_apple_head_locked_timeline() {
    constexpr std::size_t k_frames = 19200U;
    const auto in = write_head_locked_timeline_fixture(k_frames);
    FileGuard in_guard(in);
    auto settings = settings_for(in);
    settings.orientation = {90.0F, 0.0F, 0.0F};
    Prepared prepared(settings);
    if (!check(prepared.ok(), "apple-headtrack-timeline: Apple prepare")) {
        return false;
    }
    const auto realtime = stream_full(prepared, settings, {333U, 1024U, 511U});
    const auto offline = offline_reference(prepared, settings, 2U, "mr_apple_head_timeline_output");
    if (!realtime || !offline || realtime->size() < k_frames * 2U || offline->size() < k_frames * 2U) {
        return check(false, "apple-headtrack-timeline: realtime and offline renders");
    }
    const auto verify_windows =
        [&](const std::vector<float>& samples, const char* unlocked_msg, const char* locked_msg) {
            constexpr std::size_t first_begin = 3072U;
            constexpr std::size_t first_end = 8192U;
            constexpr std::size_t second_begin = 11264U;
            constexpr std::size_t second_end = 17408U;
            const double first_l = channel_energy(samples, 2U, 0U, first_begin, first_end);
            const double first_r = channel_energy(samples, 2U, 1U, first_begin, first_end);
            const double second_l = channel_energy(samples, 2U, 0U, second_begin, second_end);
            const double second_r = channel_energy(samples, 2U, 1U, second_begin, second_end);
            bool windows_ok = true;
            windows_ok &= check(first_r > first_l, unlocked_msg);
            windows_ok &= check(std::abs(second_r - second_l) < std::abs(first_r - first_l) * 0.5, locked_msg);
            return windows_ok;
        };
    bool ok = verify_windows(*realtime,
                             "apple-headtrack-timeline: realtime false block follows listener rotation",
                             "apple-headtrack-timeline: realtime true block stays head-relative");
    ok &= verify_windows(*offline,
                         "apple-headtrack-timeline: offline false block follows listener rotation",
                         "apple-headtrack-timeline: offline true block stays head-relative");
    return ok;
}

// A scene with no renderable buses (here: a muted object) must open a silent stream — not
// fail trying to drive a 0-input AUSpatialMixer — and produce full-length silence.
bool verify_apple_stream_silent() {
    constexpr uint32_t k_frames = 4096U;
    const auto in = write_fixture(0.0F, k_frames, 1.0F, 0.0F, false, /*mute=*/true);
    FileGuard in_guard(in);
    const auto settings = settings_for(in);
    Prepared prepared(settings);
    if (!check(prepared.ok(), "silent: apple prepare")) {
        return false;
    }
    const auto out = stream_full(prepared, settings, {512, 100});
    if (!out) {
        return check(false, "silent: open_stream + process succeeds for empty buses");
    }
    bool ok = true;
    ok &= check(out->size() == static_cast<std::size_t>(k_frames) * 2U, "silent: full-length silence produced");
    ok &= check(std::ranges::all_of(*out, [](float v) { return v == 0.0F; }), "silent: output is all zero");
    // seek() on a silent stream must not dereference the (absent) AU / reader.
    Stream stream(prepared, settings);
    if (!check(stream.ok(), "silent: open_stream for seek")) {
        return false;
    }
    ok &= check(stream.seek(1000), "silent: seek succeeds without AU/reader");
    std::vector<float> buf(std::size_t{64} * 2U, 1.0F);
    const auto produced = stream.process(buf, 64);
    ok &= check(produced == 64U, "silent: process after seek produces frames");
    ok &= check(std::ranges::all_of(buf, [](float v) { return v == 0.0F; }), "silent: post-seek output is silence");
    return ok;
}

// A live gain override must scale the object's bus gain on the next block, so the stream output is
// quieter by the override factor — validating BusPlan.object_id → live gain map end to end through
// the real AUSpatialMixer.
bool verify_apple_stream_gain_override() {
    const auto in = write_fixture(45.0F, 8192U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    const auto settings = settings_for(in);
    Prepared prepared(settings);
    if (!check(prepared.ok(), "override: apple prepare")) {
        return false;
    }
    const auto baseline = stream_full(prepared, settings, {1024});
    // -12.04 dB ≈ 0.25 linear, applied before the first pull.
    const auto overridden = stream_full(prepared, settings, {1024}, {{k_object_id, -12.041F, false, std::nullopt, ""}});
    if (!baseline || !overridden) {
        return false;
    }
    bool ok = check(overridden->size() == baseline->size(), "override: same frame count as baseline");
    const auto buffer_rms = [](const std::vector<float>& v) {
        const double total = std::accumulate(v.begin(), v.end(), 0.0, [](double acc, float s) {
            return acc + (static_cast<double>(s) * static_cast<double>(s));
        });
        return v.empty() ? 0.0 : std::sqrt(total / static_cast<double>(v.size()));
    };
    const double b = buffer_rms(*baseline);
    const double o = buffer_rms(*overridden);
    ok &= check(b > 1.0e-3, "override: baseline has signal energy");
    // 0.25 linear; allow generous tolerance for HRTF spectral coloration around the scalar.
    ok &= check(o < b * 0.5, "override: -12 dB override clearly attenuates the output");
    ok &= check(o > b * 0.1, "override: attenuation is the gain scalar, not silence");
    return ok;
}

// GUI binaural monitoring drives AppleStream through its output-stage path. A mid-play gain edit
// must therefore enter the AU as a sample-domain envelope: the first edited frame keeps the intact
// old convolution state, then the input reaches the target over 20 ms.
bool verify_apple_output_stage_live_gain_ramp() {
    constexpr std::size_t k_boundary = 1024U;
    const auto in = write_fixture(45.0F, 16384U, 1.0F, 0.0F, false);
    FileGuard in_guard(in);
    const auto settings = settings_for(in);
    Prepared prepared(settings);
    if (!check(prepared.ok(), "output-gain-ramp: prepare")) {
        return false;
    }
    const auto baseline = stream_full(prepared, settings, {512});
    Stream stream(prepared, settings);
    if (!baseline ||
        !check(stream.ok() && stream.renders_orientation_at_output(), "output-gain-ramp: open output-stage stream")) {
        return false;
    }
    const uint32_t in_ch = stream.intermediate_channels();
    const uint32_t out_ch = stream.out_channels();
    std::vector<float> changed;
    std::vector<float> intermediate;
    std::vector<float> rendered;
    std::size_t total = 0U;
    while (true) {
        constexpr std::size_t k_chunk = 512U;
        intermediate.assign(k_chunk * in_ch, 0.0F);
        const auto produced = stream.produce_intermediate(intermediate, k_chunk);
        if (!check(produced.has_value(), "output-gain-ramp: produce intermediate")) {
            return false;
        }
        if (*produced == 0U) {
            break;
        }
        if (total == k_boundary) {
            stream.set_overrides({{k_object_id, -20.0F, false, std::nullopt, ""}}, 1U);
        }
        rendered.assign(*produced * out_ch, 0.0F);
        const std::size_t got = stream.render_output(intermediate.data(), *produced * in_ch, rendered, *produced);
        changed.insert(changed.end(), rendered.begin(), rendered.begin() + static_cast<std::ptrdiff_t>(got * out_ch));
        total += got;
    }
    if (!check(changed.size() == baseline->size(), "output-gain-ramp: frame count unchanged")) {
        return false;
    }
    bool ok = true;
    for (uint32_t channel = 0; channel < out_ch; ++channel) {
        const std::size_t index = (k_boundary * out_ch) + channel;
        ok &= check(std::fabs(changed[index] - (*baseline)[index]) < 2.0e-4F,
                    "output-gain-ramp: first edited frame preserves the old AU state");
    }
    const std::size_t settled_begin = k_boundary + 2048U;
    const std::size_t settled_end = std::min<std::size_t>(settled_begin + 4096U, total);
    double baseline_energy = 0.0;
    double changed_energy = 0.0;
    for (std::size_t frame = settled_begin; frame < settled_end; ++frame) {
        for (uint32_t channel = 0; channel < out_ch; ++channel) {
            const std::size_t index = (frame * out_ch) + channel;
            baseline_energy += static_cast<double>((*baseline)[index]) * (*baseline)[index];
            changed_energy += static_cast<double>(changed[index]) * changed[index];
        }
    }
    ok &= check(changed_energy < baseline_energy * 0.03 && changed_energy > baseline_energy * 0.003,
                "output-gain-ramp: settles near the -20 dB target");
    return ok;
}

// BEGIN AUSpatialMixer HRTF probe (second part, unchanged by R3 F3)
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

// END AUSpatialMixer HRTF probe

} // namespace

int main() try {
    bool ok = true;
    ok &= verify_capabilities();
    ok &= verify_directional_sign();
    ok &= verify_zero_gain_is_silent();
    ok &= verify_speaker_panning();
    ok &= verify_speaker_rendering_flags_switch();
    ok &= verify_speaker_layouts_render();
    ok &= verify_binaural_container_tag();
    ok &= verify_render_window_frame_count();
    ok &= verify_spatial_presets_render_binaural();
    ok &= verify_extent_spread();
    ok &= verify_spread_mode_none_disables_extent();
    ok &= verify_channel_lock_snaps();
    ok &= verify_bed_and_lfe_routing();
    ok &= verify_22_2_lfe_routing();
    ok &= verify_direct_speakers_routing_modes();
    ok &= verify_direct_speakers_missing_position_fallback();
    ok &= verify_mixed_object_direct_and_lfe_paths();
    ok &= verify_apple_direct_stream_and_dynamic_blocks();
    ok &= verify_apple_stream_matches_window();
    ok &= verify_apple_output_stage_matches_process();
    ok &= verify_apple_reposition_source();
    ok &= verify_apple_stream_listener_orientation();
    ok &= verify_apple_stream_initial_head_locked_orientation();
    ok &= verify_apple_stream_adm_head_locked();
    ok &= verify_apple_head_locked_timeline();
    ok &= verify_apple_stream_silent();
    ok &= verify_apple_stream_gain_override();
    ok &= verify_apple_output_stage_live_gain_ramp();
    ok &= verify_spatial_mixer_hrtf_modes_probe();
    ok &= verify_listener_orientation();
    return ok ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "apple smoke test exception: " << error.what() << '\n';
    return 1;
}
