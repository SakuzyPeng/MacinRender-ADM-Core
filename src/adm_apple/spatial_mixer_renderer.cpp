#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ebur128.h>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>
#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render_apple.h"
#include "adm/scene.h"

#include "render_common.h"
#include "speaker_layouts.h"

namespace mradm {
namespace {

// Build the resolved output speaker set (for channelLock) from a project speaker layout.
// Returns empty for binaural output (no discrete speakers) so channelLock is dropped.
[[nodiscard]] std::vector<SceneOutputSpeaker> output_speakers(std::string_view layout_id) {
    std::vector<SceneOutputSpeaker> speakers;
    if (const auto* layout = render_layouts::find_speaker_layout(layout_id)) {
        speakers.reserve(layout->speakers.size());
        std::ranges::transform(layout->speakers, std::back_inserter(speakers), [](const auto& spk) {
            return SceneOutputSpeaker{spk.azimuth, spk.elevation, spk.is_lfe};
        });
    }
    return speakers;
}

// AUSpatialMixer slice + control-rate update granularity (~10 ms at 48 kHz). The AU
// smooths parameter changes between updates, so per-block az/el/gain is smooth enough.
constexpr UInt32 k_render_block = 512;

[[nodiscard]] std::string os_status_message(OSStatus status) {
    return fmt::format("OSStatus {}", static_cast<int>(status));
}

[[nodiscard]] Error apple_status_error(std::string message, OSStatus status) {
    return {ErrorCode::render_failed, std::move(message), os_status_message(status)};
}

[[nodiscard]] std::optional<SInt32> apple_spatial_preset_number(AppleSpatialPreset preset) {
    switch (preset) {
    case AppleSpatialPreset::off:
        return std::nullopt;
    case AppleSpatialPreset::headphone_default:
        return 1;
    case AppleSpatialPreset::headphone_movie:
        return 2;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view apple_spatial_preset_display_name(AppleSpatialPreset preset) {
    switch (preset) {
    case AppleSpatialPreset::off:
        return "off";
    case AppleSpatialPreset::headphone_default:
        return "Headphone Media Playback Default";
    case AppleSpatialPreset::headphone_movie:
        return "Headphone Media Playback Movie";
    }
    return "unknown";
}

// SpatialMixer azimuth is +ve to the RIGHT; project ADM azimuth is +ve to the LEFT
// (BS.2051). Negate when feeding the AU, or the whole soundfield mirrors L<->R.
[[nodiscard]] float sm_azimuth(float adm_azimuth) {
    return -adm_azimuth;
}

// kSpatialMixerParam_Gain is in dB (range -120..20, default 0 = unity). ADM gains are
// linear, so convert; a non-positive linear gain (silent / inactive bus) floors to the
// -120 dB minimum (effectively muted) rather than 0 dB, which would be unity.
[[nodiscard]] float linear_gain_to_db(float linear) {
    constexpr float k_min_db = -120.0F; // kSpatialMixerParam_Gain minimum
    constexpr float k_max_db = 20.0F;   // kSpatialMixerParam_Gain maximum
    if (linear <= 1.0e-6F) {
        return k_min_db;
    }
    return std::clamp(20.0F * std::log10(linear), k_min_db, k_max_db);
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

[[nodiscard]] Result<AudioUnitGuard> create_spatial_mixer_unit() {
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Mixer;
    desc.componentSubType = kAudioUnitSubType_SpatialMixer;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr) {
        return tl::unexpected{
            Error{ErrorCode::unsupported, "AUSpatialMixer component is not available", "subtype=3dem"}};
    }
    AudioUnit unit = nullptr;
    const OSStatus status = AudioComponentInstanceNew(comp, &unit);
    if (status != noErr || unit == nullptr) {
        return tl::unexpected{apple_status_error("failed to create AUSpatialMixer instance", status)};
    }
    return AudioUnitGuard{unit};
}

[[nodiscard]] AudioStreamBasicDescription pcm_float_format(UInt32 channels, double sample_rate) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = sample_rate;
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

[[nodiscard]] Result<void> set_uint32_property(AudioUnit unit,
                                               AudioUnitPropertyID property,
                                               AudioUnitScope scope,
                                               AudioUnitElement element,
                                               UInt32 value,
                                               std::string_view label) {
    const OSStatus status = AudioUnitSetProperty(unit, property, scope, element, &value, sizeof(value));
    if (status != noErr) {
        return tl::unexpected{apple_status_error(fmt::format("failed to set AUSpatialMixer {}", label), status)};
    }
    return {};
}

[[nodiscard]] Result<void> set_present_preset(AudioUnit unit, AppleSpatialPreset spatial_preset) {
    const auto preset_number = apple_spatial_preset_number(spatial_preset);
    if (!preset_number.has_value()) {
        return {};
    }
    AUPreset preset{};
    preset.presetNumber = preset_number.value();
    preset.presetName = nullptr;
    const OSStatus status = AudioUnitSetProperty(
        unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &preset, sizeof(preset));
    if (status != noErr) {
        return tl::unexpected{apple_status_error(
            fmt::format("failed to set AUSpatialMixer factory preset {}", preset_number.value()), status)};
    }
    return {};
}

[[nodiscard]] Result<void> set_stream_format(AudioUnit unit,
                                             AudioUnitScope scope,
                                             AudioUnitElement element,
                                             UInt32 channels,
                                             double sample_rate,
                                             std::string_view label) {
    auto fmt = pcm_float_format(channels, sample_rate);
    const OSStatus status =
        AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, element, &fmt, sizeof(fmt));
    if (status != noErr) {
        return tl::unexpected{apple_status_error(fmt::format("failed to set AUSpatialMixer {}", label), status)};
    }
    return {};
}

// Tag a mono input bus as LFE so SpatialMixer routes it without spatialization.
[[nodiscard]] Result<void> set_lfe_input_layout(AudioUnit unit, AudioUnitElement element) {
    AudioChannelLayout layout{};
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
    layout.mNumberChannelDescriptions = 1;
    layout.mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_LFEScreen;
    const auto size =
        static_cast<UInt32>(offsetof(AudioChannelLayout, mChannelDescriptions) + sizeof(AudioChannelDescription));
    const OSStatus status = AudioUnitSetProperty(
        unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, element, &layout, size);
    if (status != noErr) {
        return tl::unexpected{apple_status_error("failed to set LFE input channel layout", status)};
    }
    return {};
}

// CoreAudio output channel-layout tags, matching caf_io.cpp's canonical project-layout
// -> AudioChannelLayoutTag mapping so the AU's output channel order is identical to the
// container writers' expected order (verified: VBAP pans ADM-left -> output channel 0).
constexpr AudioChannelLayoutTag k_tag_mpeg_5_1_a = (121U << 16) | 6U;
constexpr AudioChannelLayoutTag k_tag_wave_7_1 = (189U << 16) | 8U;
constexpr AudioChannelLayoutTag k_tag_atmos_5_1_2 = (194U << 16) | 8U;
constexpr AudioChannelLayoutTag k_tag_atmos_5_1_4 = (195U << 16) | 10U;
constexpr AudioChannelLayoutTag k_tag_atmos_7_1_4 = (192U << 16) | 12U;
constexpr AudioChannelLayoutTag k_tag_atmos_9_1_6 = (193U << 16) | 16U;
constexpr AudioChannelLayoutTag k_tag_cicp_13 = (204U << 16) | 24U;

struct AppleSpeakerLayout {
    std::string_view id;
    std::string_view display_name;
    uint16_t channels;
    uint16_t lfe_count;
    bool is_3d;
    AudioChannelLayoutTag layout_tag;
};

// clang-format off
constexpr std::array<AppleSpeakerLayout, 7> k_apple_speaker_layouts{{
    {"0+5+0",  "5.1",   6,  1, false, k_tag_mpeg_5_1_a},
    {"wav71",  "7.1",   8,  1, false, k_tag_wave_7_1},
    {"2+5+0",  "5.1.2", 8,  1, true,  k_tag_atmos_5_1_2},
    {"4+5+0",  "5.1.4", 10, 1, true,  k_tag_atmos_5_1_4},
    {"4+7+0",  "7.1.4", 12, 1, true,  k_tag_atmos_7_1_4},
    {"9.1.6",  "9.1.6", 16, 1, true,  k_tag_atmos_9_1_6},
    {"9+10+3", "22.2",  24, 2, true,  k_tag_cicp_13},
}};
// clang-format on

[[nodiscard]] const AppleSpeakerLayout* find_apple_speaker_layout(std::string_view layout_id) {
    const auto it = std::ranges::find_if(
        k_apple_speaker_layouts, [layout_id](const AppleSpeakerLayout& layout) { return layout.id == layout_id; });
    if (it == k_apple_speaker_layouts.end()) {
        return nullptr;
    }
    return std::addressof(*it);
}

// Resolved output target for one render. binaural -> 2ch HRTF (Headphones output type,
// no speaker layout); speaker -> Nch VBAP into a standard CoreAudio layout tag.
// writer_layout is the container layout id passed to the writer: the binaural path
// normalizes the default "0+2+0" alias to "binaural" so CAF/APAC tag the output with
// kAudioChannelLayoutTag_Binaural rather than plain Stereo (it is a binaural signal).
struct OutputProfile {
    uint16_t channels{2};
    bool binaural{true};
    AudioChannelLayoutTag layout_tag{0};
    std::string_view writer_layout{"binaural"};
};

[[nodiscard]] std::optional<OutputProfile> resolve_output_profile(std::string_view layout_id) {
    if (layout_id == "binaural" || layout_id == "0+2+0") {
        return OutputProfile{.channels = 2, .binaural = true, .layout_tag = 0, .writer_layout = "binaural"};
    }
    if (const auto* layout = find_apple_speaker_layout(layout_id); layout != nullptr) {
        return OutputProfile{
            .channels = layout->channels,
            .binaural = false,
            .layout_tag = layout->layout_tag,
            .writer_layout = layout->id,
        };
    }
    return std::nullopt;
}

[[nodiscard]] Result<void> set_output_layout_tag(AudioUnit unit, AudioChannelLayoutTag tag) {
    AudioChannelLayout layout{};
    layout.mChannelLayoutTag = tag;
    const OSStatus status = AudioUnitSetProperty(
        unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, 0, &layout, sizeof(layout));
    if (status != noErr) {
        return tl::unexpected{apple_status_error("failed to set output channel layout", status)};
    }
    return {};
}

// One resolved control point for a SpatialMixer input bus (already in AU convention).
struct BusEvent {
    uint64_t start_sample{0};
    uint64_t end_sample{0};
    float azimuth{0.0F};   // SpatialMixer convention (sign-flipped from ADM)
    float elevation{0.0F}; // degrees, +ve up
    float distance{1.0F};  // metres
    float gain{0.0F};      // linear
};

struct BusPlan {
    uint16_t source_channel{0};
    UInt32 source_mode{kSpatialMixerSourceMode_PointSource};
    bool is_lfe{false};
    std::string object_id;         // owning SceneObject::id, for live gain overrides
    std::string speaker_label_key; // normalized DirectSpeakers label (empty for Objects); per-channel live gain key
    std::vector<BusEvent> events;  // sorted by start_sample
};

// Immutable render recipe: the resolved output target plus one BusPlan per SpatialMixer
// input element. The AU itself (mutable DSP state) is created in render_window, not here,
// so this stays shareable across PreviewSession windows.
struct ApplePrepared final : IPreparedRender {
    OutputProfile profile;
    std::vector<BusPlan> buses;
};

// Flatten one prepared object block into per-bus events. When apply_extent is true each
// divergence source is further expanded into the 17-point extent disk cloud (a point
// object stays one point); when false the width/height/depth are ignored and the source
// stays a single point, honoring --speaker-spread-mode / --binaural-spread-mode none.
// The result is the set of coherent point sources for the block; slot i is fed to bus i.
[[nodiscard]] std::vector<BusEvent>
object_block_events(const render_common::PreparedObjectBlock& prepared, const SceneObject& obj, bool apply_extent) {
    std::vector<BusEvent> events;
    for (const auto& source : prepared.sources) {
        const auto polar = scene_position_to_polar(source.position);
        const float distance = std::max(polar.distance, 1.0e-3F);
        if (!apply_extent) {
            BusEvent ev;
            ev.start_sample = prepared.start_sample;
            ev.end_sample = prepared.end_sample;
            ev.azimuth = sm_azimuth(polar.azimuth);
            ev.elevation = std::clamp(polar.elevation, -90.0F, 90.0F);
            ev.distance = distance;
            ev.gain = source.gain * obj.gain;
            events.push_back(ev);
            continue;
        }
        const auto cloud = render_common::extent_disk_cloud(source.position, source.width, source.height, source.depth);
        for (const auto& point : cloud) {
            BusEvent ev;
            ev.start_sample = prepared.start_sample;
            ev.end_sample = prepared.end_sample;
            ev.azimuth = sm_azimuth(point.azimuth);
            ev.elevation = std::clamp(point.elevation, -90.0F, 90.0F);
            ev.distance = distance;
            ev.gain = source.gain * obj.gain * point.weight;
            events.push_back(ev);
        }
    }
    return events;
}

// Build the immutable bus recipe from the resolved scene. Objects become PointSource
// buses (divergence expands to parallel buses, padding inactive slots with silent
// events); DirectSpeakers become AmbienceBed buses (LFE -> Bypass). channelLock snaps to
// `speakers` (the resolved output layout for speaker output; empty for binaural -> dropped).
[[nodiscard]] std::vector<BusPlan> build_bus_plans(const AdmScene& scene,
                                                   LogSink& logs,
                                                   bool apply_extent,
                                                   const std::vector<SceneOutputSpeaker>& speakers) {
    std::vector<BusPlan> buses;
    bool screen_ref_warned = false;

    for (const auto& obj : scene.objects) {
        if (obj.mute) {
            continue;
        }
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value()) {
                continue;
            }
            const uint16_t ch = *track.channel_index;

            if (!track.blocks.empty()) {
                // Each block expands to a variable number of coherent point sources
                // (divergence × extent cloud). Allocate one bus per slot up to the max
                // across blocks; blocks with fewer sources pad the trailing slots silent.
                struct BlockEvents {
                    uint64_t start_sample{0};
                    uint64_t end_sample{0};
                    std::vector<BusEvent> events;
                };
                std::vector<BlockEvents> blocks;
                blocks.reserve(track.blocks.size());
                std::size_t max_slots = 1;
                for (const auto& raw : track.blocks) {
                    auto pb = render_common::prepare_object_block(raw, obj, speakers, logs, "apple", screen_ref_warned);
                    BlockEvents be{pb.start_sample, pb.end_sample, object_block_events(pb, obj, apply_extent)};
                    max_slots = std::max(max_slots, be.events.size());
                    blocks.push_back(std::move(be));
                }

                std::vector<BusPlan> obj_buses(max_slots);
                for (auto& bp : obj_buses) {
                    bp.source_channel = ch;
                    bp.source_mode = kSpatialMixerSourceMode_PointSource;
                    bp.object_id = obj.id;
                }
                for (const auto& be : blocks) {
                    for (std::size_t k = 0; k < max_slots; ++k) {
                        if (k < be.events.size()) {
                            obj_buses[k].events.push_back(be.events[k]);
                        } else {
                            BusEvent silent;
                            silent.start_sample = be.start_sample;
                            silent.end_sample = be.end_sample;
                            obj_buses[k].events.push_back(silent);
                        }
                    }
                }
                buses.insert(
                    buses.end(), std::make_move_iterator(obj_buses.begin()), std::make_move_iterator(obj_buses.end()));
            }

            if (!track.ds_blocks.empty()) {
                BusPlan bp;
                bp.source_channel = ch;
                bp.object_id = obj.id;
                // Per-channel live-gain key: this DS track's normalized speaker label (empty → whole-object).
                if (!track.ds_blocks.front().speaker_labels.empty()) {
                    bp.speaker_label_key =
                        render_common::canonicalise_speaker_label(track.ds_blocks.front().speaker_labels.front());
                }
                bp.is_lfe = std::ranges::any_of(track.ds_blocks, render_common::direct_speakers_block_is_lfe);
                bp.source_mode = bp.is_lfe ? kSpatialMixerSourceMode_Bypass : kSpatialMixerSourceMode_AmbienceBed;
                for (const auto& ds : track.ds_blocks) {
                    BusEvent ev;
                    ev.start_sample = ds.start_sample;
                    ev.end_sample = std::min(ds.end_sample, obj.end_sample);
                    if (ds.has_position) {
                        ev.azimuth = sm_azimuth(ds.azimuth);
                        ev.elevation = std::clamp(ds.elevation, -90.0F, 90.0F);
                        ev.distance = std::max(ds.distance, 1.0e-3F);
                    }
                    ev.gain = ds.gain * obj.gain;
                    bp.events.push_back(ev);
                }
                buses.push_back(std::move(bp));
            }
        }
    }

    for (auto& bp : buses) {
        std::ranges::sort(bp.events, {}, &BusEvent::start_sample);
    }
    return buses;
}

// Set the global listener head-orientation parameters (HeadYaw/Pitch/Roll). The project
// domain convention is yaw +left (matching ADM azimuth); SpatialMixer's own azimuth is
// +right, so yaw is mirrored like sm_azimuth(). pitch (+up) and roll follow the unit's own
// sign. Only meaningful for binaural (headphone) output. Short-circuits on first failure.
[[nodiscard]] OSStatus apply_head_orientation(AudioUnit unit, const ListenerOrientation& o) {
    OSStatus status = AudioUnitSetParameter(unit, kSpatialMixerParam_HeadYaw, kAudioUnitScope_Global, 0, -o.yaw_deg, 0);
    if (status == noErr) {
        status = AudioUnitSetParameter(unit, kSpatialMixerParam_HeadPitch, kAudioUnitScope_Global, 0, o.pitch_deg, 0);
    }
    if (status == noErr) {
        status = AudioUnitSetParameter(unit, kSpatialMixerParam_HeadRoll, kAudioUnitScope_Global, 0, o.roll_deg, 0);
    }
    return status;
}

struct HeadVec {
    double x{};
    double y{};
    double z{};
};

struct HeadQuat {
    double w{1.0};
    double x{};
    double y{};
    double z{};
};

[[nodiscard]] HeadQuat quat_mul(const HeadQuat& a, const HeadQuat& b) {
    return {
        (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z),
        (a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y),
        (a.w * b.y) - (a.x * b.z) + (a.y * b.w) + (a.z * b.x),
        (a.w * b.z) + (a.x * b.y) - (a.y * b.x) + (a.z * b.w),
    };
}

[[nodiscard]] HeadQuat axis_angle(const HeadVec& axis, double radians) {
    const double half = radians * 0.5;
    const double s = std::sin(half);
    return {std::cos(half), axis.x * s, axis.y * s, axis.z * s};
}

[[nodiscard]] HeadVec rotate_by_quat(HeadVec v, const HeadQuat& q) {
    const HeadQuat p{0.0, v.x, v.y, v.z};
    const HeadQuat qc{q.w, -q.x, -q.y, -q.z};
    const HeadQuat r = quat_mul(quat_mul(q, p), qc);
    return {r.x, r.y, r.z};
}

// Head-lock 补偿:把一个总线的方向(SpatialMixer 约定:az +右、el +上)按听者头朝向预旋转,
// 使全局 HeadYaw/Pitch/Roll 对其恰好抵消 → 该源锁在头上(head-locked),不随转头移动。
// 坐标:x=右、y=前、z=上。组合顺序与 GUI 头部姿态反馈保持一致:roll(绕前轴 y) →
// pitch(绕右轴 x) → yaw(绕上轴 z)。yaw 已由 smoke/真机方向锁定;pitch/roll 仍建议真机标定。
[[nodiscard]] std::pair<float, float> head_lock_compensate(float az_deg, float el_deg, const ListenerOrientation& o) {
    constexpr double d2r = 0.017453292519943295;
    constexpr double r2d = 57.29577951308232;
    const double a = az_deg * d2r;
    const double e = el_deg * d2r;
    const HeadVec v{std::sin(a) * std::cos(e), std::cos(a) * std::cos(e), std::sin(e)};

    const HeadQuat q_roll = axis_angle({0.0, 1.0, 0.0}, o.roll_deg * d2r);
    const HeadQuat q_pitch = axis_angle({1.0, 0.0, 0.0}, o.pitch_deg * d2r);
    const HeadQuat q_yaw = axis_angle({0.0, 0.0, 1.0}, o.yaw_deg * d2r);
    const HeadQuat head_to_world = quat_mul(q_yaw, quat_mul(q_pitch, q_roll));
    const HeadVec rotated = rotate_by_quat(v, head_to_world);

    const auto az = static_cast<float>(std::atan2(rotated.x, rotated.y) * r2d);
    const auto el = static_cast<float>(std::asin(std::clamp(rotated.z, -1.0, 1.0)) * r2d);
    return {az, el};
}

// Set the four per-source SpatialMixer parameters for one input bus, short-circuiting on
// the first failure so a bad parameter set surfaces instead of silently continuing.
[[nodiscard]] OSStatus set_bus_parameters(
    AudioUnit unit, AudioUnitElement element, float azimuth, float elevation, float distance, float gain_db) {
    OSStatus status =
        AudioUnitSetParameter(unit, kSpatialMixerParam_Azimuth, kAudioUnitScope_Input, element, azimuth, 0);
    if (status == noErr) {
        status =
            AudioUnitSetParameter(unit, kSpatialMixerParam_Elevation, kAudioUnitScope_Input, element, elevation, 0);
    }
    if (status == noErr) {
        status = AudioUnitSetParameter(unit, kSpatialMixerParam_Distance, kAudioUnitScope_Input, element, distance, 0);
    }
    if (status == noErr) {
        status = AudioUnitSetParameter(unit, kSpatialMixerParam_Gain, kAudioUnitScope_Input, element, gain_db, 0);
    }
    return status;
}

[[nodiscard]] const BusEvent* active_event(const BusPlan& bus, uint64_t frame, std::size_t& cursor) {
    while (cursor + 1 < bus.events.size() && frame >= bus.events[cursor + 1].start_sample) {
        ++cursor;
    }
    if (cursor < bus.events.size()) {
        const auto& ev = bus.events[cursor];
        if (frame >= ev.start_sample && frame < ev.end_sample) {
            return &ev;
        }
    }
    return nullptr;
}

// Per-input-bus pull callback: copies one source channel out of the shared interleaved
// staging block that render_window fills before each AudioUnitRender call.
struct InputBusContext {
    const float* staging{nullptr};
    uint16_t source_channel{0};
    uint16_t num_input_channels{1};
};

// cppcheck-suppress constParameterCallback ; AURenderCallback mandates void* (non-const).
OSStatus input_render_callback(void* ref_con,
                               AudioUnitRenderActionFlags* /*flags*/,
                               const AudioTimeStamp* /*time_stamp*/,
                               UInt32 /*bus_number*/,
                               UInt32 frames,
                               AudioBufferList* io_data) {
    const auto* ctx = static_cast<const InputBusContext*>(ref_con);
    for (UInt32 b = 0; b < io_data->mNumberBuffers; ++b) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) — CoreAudio flexible array.
        auto* out = static_cast<float*>(io_data->mBuffers[b].mData);
        for (UInt32 f = 0; f < frames; ++f) {
            out[f] = ctx->staging[(static_cast<std::size_t>(f) * ctx->num_input_channels) + ctx->source_channel];
        }
    }
    return noErr;
}

struct EburDeleter {
    void operator()(ebur128_state* s) const noexcept { ebur128_destroy(&s); }
};
using EburPtr = std::unique_ptr<ebur128_state, EburDeleter>;

[[nodiscard]] RenderMetrics collect_metrics(ebur128_state* state, uint16_t num_out_ch) {
    RenderMetrics metrics;
    if (state == nullptr) {
        return metrics;
    }
    double loudness = 0.0;
    if (ebur128_loudness_global(state, &loudness) == EBUR128_SUCCESS && std::isfinite(loudness)) {
        metrics.measured_lufs = loudness;
    }
    double max_peak = 0.0;
    for (unsigned int ch = 0; ch < num_out_ch; ++ch) {
        double ch_peak = 0.0;
        if (ebur128_true_peak(state, ch, &ch_peak) == EBUR128_SUCCESS) {
            max_peak = std::max(max_peak, ch_peak);
        }
    }
    if (max_peak > 0.0) {
        metrics.measured_peak_dbtp = 20.0 * std::log10(max_peak);
    }
    return metrics;
}

// Configure a freshly created AUSpatialMixer for a render: factory preset, output type /
// format / layout, per-input-bus stream format + algorithm + source mode + LFE + the
// staging pull callback, then AudioUnitInitialize. Shared by the offline render_window and
// the realtime AppleStream so the two never drift. `staging_data` must remain valid (and
// large enough: num_in_ch * k_render_block) for the unit's lifetime — the per-bus pull
// callbacks read it; `contexts` is resized to buses.size() and likewise must outlive use.
// NOLINTNEXTLINE(readability-function-size)
[[nodiscard]] Result<void> configure_spatial_mixer_unit(AudioUnit unit,
                                                        const OutputProfile& profile,
                                                        const std::vector<BusPlan>& buses,
                                                        uint16_t num_in_ch,
                                                        uint16_t num_out_ch,
                                                        double sample_rate,
                                                        UInt32 spatialization_algorithm,
                                                        AppleSpatialPreset preset,
                                                        const ListenerOrientation& listener_orientation,
                                                        const std::string& output_layout,
                                                        const float* staging_data,
                                                        std::vector<InputBusContext>& contexts,
                                                        LogSink& logs) {
    if (preset != AppleSpatialPreset::off) {
        if (!profile.binaural) {
            return make_error(ErrorCode::invalid_argument,
                              "apple factory spatial presets require binaural output",
                              "layout=" + output_layout);
        }
        if (auto r = set_present_preset(unit, preset); !r) {
            return tl::unexpected{r.error()};
        }
        logs.log(LogLevel::info,
                 "apple",
                 fmt::format("applied AUSpatialMixer factory preset: {}", apple_spatial_preset_display_name(preset)));
    }

    if (profile.binaural) {
        if (auto r = set_uint32_property(unit,
                                         kAudioUnitProperty_SpatialMixerOutputType,
                                         kAudioUnitScope_Global,
                                         0,
                                         kSpatialMixerOutputType_Headphones,
                                         "output type");
            !r) {
            return tl::unexpected{r.error()};
        }
    }
    if (auto r = set_uint32_property(unit,
                                     kAudioUnitProperty_ElementCount,
                                     kAudioUnitScope_Input,
                                     0,
                                     static_cast<UInt32>(buses.size()),
                                     "input element count");
        !r) {
        return tl::unexpected{r.error()};
    }
    if (auto r = set_uint32_property(unit,
                                     kAudioUnitProperty_MaximumFramesPerSlice,
                                     kAudioUnitScope_Global,
                                     0,
                                     k_render_block,
                                     "maximum frames per slice");
        !r) {
        return tl::unexpected{r.error()};
    }
    if (auto r = set_stream_format(unit, kAudioUnitScope_Output, 0, num_out_ch, sample_rate, "output stream format");
        !r) {
        return tl::unexpected{r.error()};
    }
    // Speaker output: a standard CoreAudio layout tag gives VBAP the speaker geometry and
    // fixes the output channel order to match the container writers (caf_io.cpp mapping).
    if (!profile.binaural) {
        if (auto r = set_output_layout_tag(unit, profile.layout_tag); !r) {
            return tl::unexpected{r.error()};
        }
    }

    contexts.assign(buses.size(), InputBusContext{});
    for (std::size_t i = 0; i < buses.size(); ++i) {
        const auto& bus = buses[i];
        const auto element = static_cast<AudioUnitElement>(i);
        if (auto r = set_stream_format(unit, kAudioUnitScope_Input, element, 1, sample_rate, "input stream format");
            !r) {
            return tl::unexpected{r.error()};
        }
        if (auto r = set_uint32_property(unit,
                                         kAudioUnitProperty_SpatializationAlgorithm,
                                         kAudioUnitScope_Input,
                                         element,
                                         spatialization_algorithm,
                                         "spatialization algorithm");
            !r) {
            return tl::unexpected{r.error()};
        }
        if (auto r = set_uint32_property(unit,
                                         kAudioUnitProperty_SpatialMixerSourceMode,
                                         kAudioUnitScope_Input,
                                         element,
                                         bus.source_mode,
                                         "source mode");
            !r) {
            return tl::unexpected{r.error()};
        }
        if (bus.is_lfe) {
            if (auto r = set_lfe_input_layout(unit, element); !r) {
                return tl::unexpected{r.error()};
            }
        }
        contexts[i] = InputBusContext{staging_data, bus.source_channel, num_in_ch};
        AURenderCallbackStruct callback{};
        callback.inputProc = &input_render_callback;
        callback.inputProcRefCon = &contexts[i];
        const OSStatus status = AudioUnitSetProperty(
            unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, element, &callback, sizeof(callback));
        if (status != noErr) {
            return tl::unexpected{apple_status_error("failed to set input render callback", status)};
        }
    }

    const OSStatus init_status = AudioUnitInitialize(unit);
    if (init_status != noErr) {
        return tl::unexpected{apple_status_error("failed to initialize AUSpatialMixer", init_status)};
    }

    // Listener head orientation (binaural only). A factory preset resets unit/bus
    // parameters, so this must come after both the preset and AudioUnitInitialize.
    // Applied here in the shared setup so both render_window and the realtime stream
    // pick it up.
    if (profile.binaural && !listener_orientation.is_identity()) {
        const OSStatus head_status = apply_head_orientation(unit, listener_orientation);
        if (head_status != noErr) {
            return tl::unexpected{apple_status_error("failed to set listener orientation", head_status)};
        }
        logs.log(LogLevel::info,
                 "apple",
                 fmt::format("listener orientation: yaw={:.1f} pitch={:.1f} roll={:.1f} deg",
                             static_cast<double>(listener_orientation.yaw_deg),
                             static_cast<double>(listener_orientation.pitch_deg),
                             static_cast<double>(listener_orientation.roll_deg)));
    }
    return {};
}

// Realtime streaming session over the same AUSpatialMixer recipe as render_window. It
// renders k_render_block-aligned slices on demand into a FIFO, from which process() serves
// any requested frame count. The slice cadence + per-block parameter updates + AU sample
// time match render_window exactly, so a gap-free run from frame 0 is bit-identical to the
// offline path (the smoke test asserts this). seek() resets the AU + readers (a small
// discontinuity, acceptable for monitoring).
class AppleStream final : public IRenderStream {
  public:
    [[nodiscard]] static Result<std::unique_ptr<AppleStream>>
    create(const ApplePrepared& prepared, const RenderPlan& plan, LogSink& logs) {
        const auto& info = plan.scene.info;
        const uint16_t num_in_ch = info.num_channels;
        const auto& profile = prepared.profile;
        const uint16_t num_out_ch = profile.channels;
        const UInt32 algo =
            profile.binaural ? kSpatializationAlgorithm_HRTFHQ : kSpatializationAlgorithm_VectorBasedPanning;

        // No renderable buses → silent stream: skip AU + reader entirely (mirrors the
        // offline render_window empty-bus fast path); process() emits silence.
        const bool silent = prepared.buses.empty();

        AudioUnitGuard guard{nullptr};
        if (!silent) {
            auto unit_res = create_spatial_mixer_unit();
            if (!unit_res) {
                return tl::unexpected{unit_res.error()};
            }
            guard = std::move(*unit_res);
        }

        std::unique_ptr<AppleStream> stream{new AppleStream(std::move(guard),
                                                            prepared.profile,
                                                            prepared.buses,
                                                            num_in_ch,
                                                            num_out_ch,
                                                            info.sample_rate,
                                                            info.num_frames,
                                                            silent)};

        if (silent) {
            return stream;
        }

        if (auto r = configure_spatial_mixer_unit(stream->unit_.get(),
                                                  stream->profile_,
                                                  stream->buses_,
                                                  num_in_ch,
                                                  num_out_ch,
                                                  static_cast<double>(info.sample_rate),
                                                  algo,
                                                  plan.apple_spatial_preset,
                                                  plan.listener_orientation,
                                                  plan.output_layout,
                                                  stream->staging_.data(),
                                                  stream->contexts_,
                                                  logs);
            !r) {
            return tl::unexpected{r.error()};
        }
        stream->live_orientation_ = plan.listener_orientation;

        stream->reader_ = bw64::readFile(plan.input_path);
        if (!stream->reader_) {
            return make_error(
                ErrorCode::io_error, "failed to open input for realtime apple stream", "input=" + plan.input_path);
        }
        return stream;
    }

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        std::size_t produced = 0;
        while (produced < frames) {
            if (fifo_read_ >= fifo_.size()) {
                if (ended_) {
                    break;
                }
                if (auto r = render_slice(); !r) {
                    return tl::unexpected{r.error()};
                }
                if (fifo_read_ >= fifo_.size()) {
                    break; // slice produced nothing (EOF)
                }
            }
            const std::size_t avail = (fifo_.size() - fifo_read_) / num_out_ch_;
            const std::size_t take = std::min(frames - produced, avail);
            std::copy_n(fifo_.data() + fifo_read_, take * num_out_ch_, out.data() + (produced * num_out_ch_));
            fifo_read_ += take * num_out_ch_;
            produced += take;
        }
        return produced;
    }

    [[nodiscard]] Result<void> seek(uint64_t frame) override {
        // Silent stream has no AU / reader: just reposition the silence cursor.
        if (silent_) {
            fifo_.clear();
            fifo_read_ = 0;
            producer_pos_ = frame;
            ended_ = false;
            return {};
        }
        // Reset the black-box AU state, reposition the reader, rewind the per-bus event
        // cursors (active_event only advances forward), and drop the FIFO.
        const OSStatus status = AudioUnitReset(unit_.get(), kAudioUnitScope_Global, 0);
        if (status != noErr) {
            return tl::unexpected{apple_status_error("failed to reset AUSpatialMixer on seek", status)};
        }
        render_common::seek_reader_abs(*reader_, frame);
        std::ranges::fill(ev_cursor_, std::size_t{0});
        fifo_.clear();
        fifo_read_ = 0;
        producer_pos_ = frame;
        ended_ = false;
        return {};
    }

    // Worker-thread only (same thread as process()): rebuild the per-object live gain
    // multiplier table. Absent objects render at unity (the prepared gain). The next
    // render_slice picks the new values up; already-FIFO'd / ring-buffered audio is not
    // re-rendered, so the change is heard after the current buffer drains.
    void set_overrides(const LiveOverrides& overrides) override {
        // Store the snapshot; the per-bus gain is resolved at render time by object_id + the bus's
        // DirectSpeakers speaker label, so a single bed can be gained per channel (empty label →
        // whole object). The next render_slice picks the new values up.
        live_overrides_ = overrides;
    }

    // Worker-thread only (same thread as process()): stash the latest listener head orientation.
    // render_slice() applies it to the AU's global HeadYaw/Pitch/Roll params on the next slice.
    // No-op effect on non-binaural / silent units (those params are ignored by the AU).
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        live_orientation_ = orientation;
        orientation_dirty_ = true;
    }

    [[nodiscard]] uint32_t out_channels() const override { return num_out_ch_; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return profile_.writer_layout; }

  private:
    // Live gain multiplier (linear) for one bus, resolved by its owning object id + DirectSpeakers
    // speaker label (per-channel override wins over whole-object); 1.0 when no override applies.
    [[nodiscard]] float live_gain_for(const BusPlan& bus) const {
        return render_common::resolve_live_channel_gain(live_overrides_, bus.object_id, bus.speaker_label_key)
            .value_or(1.0F);
    }

    // 该总线是否 head-locked(锁在头上、排除头追踪),按对象 + 声道解析(同 gain)。
    [[nodiscard]] bool live_head_locked_for(const BusPlan& bus) const {
        return render_common::resolve_live_head_locked(live_overrides_, bus.object_id, bus.speaker_label_key);
    }

    AppleStream(AudioUnitGuard unit,
                OutputProfile profile,
                std::vector<BusPlan> buses,
                uint16_t num_in_ch,
                uint16_t num_out_ch,
                uint32_t sample_rate,
                uint64_t total_frames,
                bool silent)
        : profile_(profile), buses_(std::move(buses)),
          staging_(static_cast<std::size_t>(num_in_ch) * k_render_block, 0.0F),
          out_planar_(num_out_ch, std::vector<float>(k_render_block, 0.0F)),
          abl_storage_(sizeof(AudioBufferList) + (sizeof(AudioBuffer) * (static_cast<std::size_t>(num_out_ch) - 1))),
          ev_cursor_(buses_.size(), 0), num_out_ch_(num_out_ch), sample_rate_(sample_rate), total_frames_(total_frames),
          silent_(silent), unit_(std::move(unit)) {}

    [[nodiscard]] Result<void> render_slice() {
        if (producer_pos_ >= total_frames_) {
            ended_ = true;
            return {};
        }
        const auto frames_now = static_cast<UInt32>(std::min<uint64_t>(k_render_block, total_frames_ - producer_pos_));
        fifo_.assign(static_cast<std::size_t>(frames_now) * num_out_ch_, 0.0F);
        fifo_read_ = 0;

        // No renderable buses (all muted / empty scene): emit silence, matching the offline
        // render_window fast path and avoiding driving an AUSpatialMixer with 0 input buses.
        if (silent_) {
            producer_pos_ += frames_now;
            return {};
        }

        // Apply a pending live head orientation before rendering this slice (binaural HeadYaw/
        // Pitch/Roll global params; the AU ignores them on non-binaural output). Worker-thread
        // only, so the direct AudioUnitSetParameter is safe.
        if (orientation_dirty_) {
            const OSStatus head_status = apply_head_orientation(unit_.get(), live_orientation_);
            if (head_status != noErr) {
                return tl::unexpected{apple_status_error("failed to set listener orientation", head_status)};
            }
            orientation_dirty_ = false;
        }

        reader_->read(staging_.data(), frames_now);

        for (std::size_t i = 0; i < buses_.size(); ++i) {
            const BusEvent* ev = active_event(buses_[i], producer_pos_, ev_cursor_[i]);
            float azimuth = ev != nullptr ? ev->azimuth : 0.0F;
            float elevation = ev != nullptr ? ev->elevation : 0.0F;
            const float distance = ev != nullptr ? ev->distance : 1.0F;
            const float base_gain = ev != nullptr ? ev->gain : 0.0F;
            // head-locked 总线:把方向按头朝向补偿,使全局 AU 头旋转对其抵消(锁在头上)。
            // 头朝向恒等时补偿是 no-op,故未开头追踪 / world-locked 时零影响。
            if (!live_orientation_.is_identity() && live_head_locked_for(buses_[i])) {
                const auto [caz, cel] = head_lock_compensate(azimuth, elevation, live_orientation_);
                azimuth = caz;
                elevation = cel;
            }
            const float gain_db = linear_gain_to_db(base_gain * live_gain_for(buses_[i]));
            const OSStatus s = set_bus_parameters(
                unit_.get(), static_cast<AudioUnitElement>(i), azimuth, elevation, distance, gain_db);
            if (s != noErr) {
                return tl::unexpected{apple_status_error("failed to set SpatialMixer input parameter", s)};
            }
        }

        auto* abl = reinterpret_cast<AudioBufferList*>(abl_storage_.data());
        abl->mNumberBuffers = num_out_ch_;
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index) — CoreAudio flexible array.
        for (uint16_t ch = 0; ch < num_out_ch_; ++ch) {
            abl->mBuffers[ch].mNumberChannels = 1;
            abl->mBuffers[ch].mDataByteSize = frames_now * sizeof(float);
            abl->mBuffers[ch].mData = out_planar_[ch].data();
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
        AudioTimeStamp time_stamp{};
        time_stamp.mFlags = kAudioTimeStampSampleTimeValid;
        time_stamp.mSampleTime = static_cast<Float64>(producer_pos_);
        AudioUnitRenderActionFlags flags = 0;
        const OSStatus render_status = AudioUnitRender(unit_.get(), &flags, &time_stamp, 0, frames_now, abl);
        if (render_status != noErr) {
            return tl::unexpected{apple_status_error("AudioUnitRender failed", render_status)};
        }

        // fifo_ was sized + zeroed at the top of this slice; interleave the AU output into it.
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index) — CoreAudio flexible array.
        for (uint16_t ch = 0; ch < num_out_ch_; ++ch) {
            const float* src = out_planar_[ch].data();
            for (UInt32 f = 0; f < frames_now; ++f) {
                fifo_[(static_cast<std::size_t>(f) * num_out_ch_) + ch] = src[f];
            }
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
        producer_pos_ += frames_now;
        return {};
    }

    OutputProfile profile_;
    std::vector<BusPlan> buses_;
    std::unique_ptr<bw64::Bw64Reader> reader_;
    std::vector<float> staging_;
    std::vector<InputBusContext> contexts_; // referenced by the AU's per-bus pull callbacks
    std::vector<std::vector<float>> out_planar_;
    std::vector<std::uint8_t> abl_storage_;
    std::vector<std::size_t> ev_cursor_;
    uint16_t num_out_ch_;
    uint32_t sample_rate_;
    uint64_t total_frames_;
    uint64_t producer_pos_{0};
    std::vector<float> fifo_;
    std::size_t fifo_read_{0};
    bool ended_{false};
    bool silent_{false};
    LiveOverrides live_overrides_;         // live override snapshot; gain resolved per bus (worker-only)
    ListenerOrientation live_orientation_; // live head orientation; applied in render_slice when dirty (worker-only)
    bool orientation_dirty_{false};        // set by set_listener_orientation; cleared once applied to the AU
    // Declared LAST so it is destroyed FIRST: ~AudioUnitGuard runs AudioUnitUninitialize
    // before staging_/contexts_ (which the AU's input pull callbacks reference) are freed.
    AudioUnitGuard unit_;
};

class AppleRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return apple_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        const auto profile = resolve_output_profile(plan.output_layout);
        if (!profile) {
            return make_error(ErrorCode::unsupported,
                              fmt::format("apple backend does not support output layout '{}'", plan.output_layout),
                              "layout=" + plan.output_layout);
        }

        // Honor the spread-mode capability controls: binaural none/saf_spreader and
        // speaker none disable extent spreading (the object renders as a point), aligning
        // with the binaural and VBAP backends.
        //
        // Intentional divergence from VBAP: the extent disk cloud is just a set of point
        // sources, so it applies on every CoreAudio layout — including 2D ones (5.1, 7.1),
        // where a wide object correctly spreads across the horizontal speakers. VBAP cannot
        // spread on 2D layouts (SAF's 2D VBAP API has no spread parameter), so its automatic
        // mode is a no-op there; SpatialMixer has no such limitation, so automatic spreads on
        // 2D too. Use --speaker-spread-mode none to force point rendering.
        const bool apply_extent = profile->binaural ? (plan.binaural_spread_mode == BinauralSpreadMode::automatic ||
                                                       plan.binaural_spread_mode == BinauralSpreadMode::cloud)
                                                    : (plan.speaker_spread_mode != SpeakerSpreadMode::none);

        // channelLock snaps an object to the nearest output speaker. Speaker output has a
        // resolved layout, so build its speaker set and let apply_channel_lock honor it;
        // binaural has no discrete speakers, so the set stays empty and channelLock drops.
        const std::vector<SceneOutputSpeaker> speakers =
            profile->binaural ? std::vector<SceneOutputSpeaker>{} : output_speakers(plan.output_layout);

        auto buses = build_bus_plans(plan.scene, logs, apply_extent, speakers);

        const auto num_in_ch = plan.scene.info.num_channels;
        const auto invalid =
            std::ranges::find_if(buses, [num_in_ch](const BusPlan& bus) { return bus.source_channel >= num_in_ch; });
        if (invalid != buses.end()) {
            return make_error(ErrorCode::render_failed,
                              fmt::format("track channel index {} is outside input channel count {}",
                                          invalid->source_channel,
                                          num_in_ch),
                              "input=" + plan.input_path);
        }
        if (buses.empty()) {
            logs.log(LogLevel::warning, "apple", "no renderable tracks found (all muted?), writing silence");
        }

        auto prepared = std::make_shared<ApplePrepared>();
        prepared->profile = *profile;
        prepared->buses = std::move(buses);
        return std::static_pointer_cast<IPreparedRender>(prepared);
    }

    [[nodiscard]] Result<RenderMetrics>
    render_window(const IPreparedRender& prep, const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const ApplePrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "apple: open_stream received an incompatible prepared state", {});
        }
        auto stream = AppleStream::create(*prepared, plan, logs);
        if (!stream) {
            return tl::unexpected{stream.error()};
        }
        return std::unique_ptr<IRenderStream>{std::move(*stream)};
    }
};

// NOLINTNEXTLINE(readability-function-size)
Result<RenderMetrics> AppleRenderer::render_window(const IPreparedRender& prep,
                                                   const RenderPlan& plan,
                                                   ProgressSink& progress,
                                                   LogSink& logs) {
    const auto* prepared = dynamic_cast<const ApplePrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(
            ErrorCode::internal_error, "apple: render_window received an incompatible prepared state", {});
    }

    const auto& info = plan.scene.info;
    const auto num_in_ch = info.num_channels;
    const auto num_frames = info.num_frames;
    const auto sample_rate = info.sample_rate;
    const auto& profile = prepared->profile;
    const uint16_t num_out_ch = profile.channels;
    const UInt32 spatialization_algorithm =
        profile.binaural ? kSpatializationAlgorithm_HRTFHQ : kSpatializationAlgorithm_VectorBasedPanning;
    const auto& buses = prepared->buses;

    // On-demand output window (RenderPlan::render_window). AUSpatialMixer is a
    // black-box stateful AudioUnit, especially in HRTF mode, so windowed rendering
    // starts one aligned render block before the requested window when possible.
    // Pre-roll frames update SpatialMixer state but are not written. Absolute
    // frame positions are still used for ADM events and AudioUnit sample time.
    const bool windowed = plan.render_window.has_value();
    const uint64_t win_start = windowed ? std::min(plan.render_window->start_frame, num_frames) : 0;
    const uint64_t win_end = windowed ? std::min(win_start + plan.render_window->frame_count, num_frames) : num_frames;
    uint64_t start_pos = 0;
    if (windowed && win_start >= k_render_block) {
        start_pos = ((win_start / k_render_block) - 1U) * k_render_block;
    }
    const uint64_t frames_to_write = win_end > win_start ? win_end - win_start : 0;

    // Normalize the container layout: a binaural HRTF render tags the output as binaural
    // (not the "0+2+0" stereo alias it may have been requested as), so CAF/APAC carry
    // kAudioChannelLayoutTag_Binaural instead of plain Stereo.
    const std::string writer_layout{profile.writer_layout};
    if (plan.output_layout != writer_layout) {
        logs.log(LogLevel::info,
                 "apple",
                 fmt::format("output layout '{}' rendered as binaural HRTF; tagging container as '{}'",
                             plan.output_layout,
                             writer_layout));
    }
    auto writer_res =
        audio::WriterHandle::open(plan.output_path, num_out_ch, static_cast<uint32_t>(sample_rate), writer_layout);
    if (!writer_res) {
        return tl::unexpected{writer_res.error()};
    }
    auto& writer = *writer_res;

    EburPtr lufs_st{
        ebur128_init(num_out_ch, static_cast<unsigned long>(sample_rate), EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};

    constexpr std::size_t k_num_buffers = 2;
    std::array<std::vector<float>, k_num_buffers> out_buffers;
    for (auto& buffer : out_buffers) {
        buffer.assign(static_cast<std::size_t>(num_out_ch) * k_render_block, 0.0F);
    }
    std::array<std::future<void>, k_num_buffers> meter_pending;
    render_common::SerialWorker meter;
    std::size_t buf_idx = 0;

    // No renderable buses: write silence (still a valid, correctly-sized output).
    if (buses.empty()) {
        uint64_t frames_done = start_pos;
        while (frames_done < win_end) {
            if (plan.cancel_token.stop_requested()) {
                return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
            }
            if (meter_pending.at(buf_idx).valid()) {
                meter_pending.at(buf_idx).get();
            }
            const auto& out_interleaved = out_buffers.at(buf_idx);
            const uint64_t frames_now = std::min<uint64_t>(k_render_block, num_frames - frames_done);
            const uint64_t w_lo = std::max(frames_done, win_start);
            const uint64_t w_hi = std::min(frames_done + frames_now, win_end);
            const bool emit = w_hi > w_lo;
            const std::size_t emit_off = emit ? static_cast<std::size_t>(w_lo - frames_done) : 0;
            const std::size_t emit_count = emit ? static_cast<std::size_t>(w_hi - w_lo) : 0;
            if (emit && writer.write(out_interleaved.data() + (emit_off * num_out_ch), emit_count) != emit_count) {
                return make_error(ErrorCode::io_error, "short write while rendering", "output=" + plan.output_path);
            }
            if (emit && lufs_st) {
                ebur128_state* state = lufs_st.get();
                const float* data = out_interleaved.data() + (emit_off * num_out_ch);
                meter_pending.at(buf_idx) =
                    meter.post([state, data, emit_count] { ebur128_add_frames_float(state, data, emit_count); });
            }
            buf_idx = (buf_idx + 1U) % k_num_buffers;
            frames_done += frames_now;
        }
        for (auto& pending : meter_pending) {
            if (pending.valid()) {
                pending.get();
            }
        }
        progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
        return collect_metrics(lufs_st.get(), num_out_ch);
    }

    // Declare the callback backing storage BEFORE the AU so it is destroyed AFTER the AU's
    // AudioUnitUninitialize/Dispose runs (the input pull callbacks reference staging via
    // contexts) — matching configure_spatial_mixer_unit's lifetime contract.
    std::vector<float> staging(static_cast<std::size_t>(num_in_ch) * k_render_block, 0.0F);
    std::vector<InputBusContext> contexts;

    auto unit_res = create_spatial_mixer_unit();
    if (!unit_res) {
        return tl::unexpected{unit_res.error()};
    }
    AudioUnit unit = unit_res->get();

    if (auto r = configure_spatial_mixer_unit(unit,
                                              profile,
                                              buses,
                                              num_in_ch,
                                              num_out_ch,
                                              static_cast<double>(sample_rate),
                                              spatialization_algorithm,
                                              plan.apple_spatial_preset,
                                              plan.listener_orientation,
                                              plan.output_layout,
                                              staging.data(),
                                              contexts,
                                              logs);
        !r) {
        return tl::unexpected{r.error()};
    }

    logs.log(LogLevel::info,
             "apple",
             fmt::format("AUSpatialMixer rendering {} buses → {} ({}ch, {}), {} frames",
                         buses.size(),
                         plan.output_layout,
                         num_out_ch,
                         profile.binaural ? "HRTF binaural" : "VBAP speakers",
                         frames_to_write));
    progress.on_progress({RenderStage::rendering, RenderOperation::render_audio, 0.3, 0.0, 0, 0, "rendering audio"});

    auto reader = bw64::readFile(plan.input_path);
    if (start_pos > 0) {
        render_common::seek_reader_abs(*reader, start_pos);
    }

    // Output is non-interleaved; AudioUnitRender writes one buffer per output channel.
    std::vector<std::vector<float>> out_planar(num_out_ch, std::vector<float>(k_render_block, 0.0F));
    std::vector<std::uint8_t> abl_storage(sizeof(AudioBufferList) +
                                          (sizeof(AudioBuffer) * (static_cast<std::size_t>(num_out_ch) - 1)));
    auto* abl = reinterpret_cast<AudioBufferList*>(abl_storage.data());
    abl->mNumberBuffers = num_out_ch;

    std::vector<std::size_t> ev_cursor(buses.size(), 0);

    AudioTimeStamp time_stamp{};
    time_stamp.mFlags = kAudioTimeStampSampleTimeValid;

    uint64_t frames_done = start_pos;
    const uint64_t progress_total = std::max<uint64_t>(1, win_end - start_pos);
    const auto progress_span = static_cast<double>(progress_total);

    while (frames_done < win_end) {
        if (plan.cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
        }
        if (meter_pending.at(buf_idx).valid()) {
            meter_pending.at(buf_idx).get();
        }
        auto& out_interleaved = out_buffers.at(buf_idx);
        const auto frames_now = static_cast<UInt32>(std::min<uint64_t>(k_render_block, num_frames - frames_done));

        reader->read(staging.data(), frames_now);

        for (std::size_t i = 0; i < buses.size(); ++i) {
            const BusEvent* ev = active_event(buses[i], frames_done, ev_cursor[i]);
            const float azimuth = ev != nullptr ? ev->azimuth : 0.0F;
            const float elevation = ev != nullptr ? ev->elevation : 0.0F;
            const float distance = ev != nullptr ? ev->distance : 1.0F;
            const float gain_db = linear_gain_to_db(ev != nullptr ? ev->gain : 0.0F);
            const auto element = static_cast<AudioUnitElement>(i);
            const OSStatus param_status = set_bus_parameters(unit, element, azimuth, elevation, distance, gain_db);
            if (param_status != noErr) {
                return tl::unexpected{apple_status_error("failed to set SpatialMixer input parameter", param_status)};
            }
        }

        time_stamp.mSampleTime = static_cast<Float64>(frames_done);
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index) — CoreAudio flexible array.
        for (uint16_t ch = 0; ch < num_out_ch; ++ch) {
            abl->mBuffers[ch].mNumberChannels = 1;
            abl->mBuffers[ch].mDataByteSize = frames_now * sizeof(float);
            abl->mBuffers[ch].mData = out_planar[ch].data();
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
        AudioUnitRenderActionFlags flags = 0;
        const OSStatus render_status = AudioUnitRender(unit, &flags, &time_stamp, 0, frames_now, abl);
        if (render_status != noErr) {
            return tl::unexpected{apple_status_error("AudioUnitRender failed", render_status)};
        }

        for (uint16_t ch = 0; ch < num_out_ch; ++ch) {
            const float* src = out_planar[ch].data();
            for (UInt32 f = 0; f < frames_now; ++f) {
                out_interleaved[(static_cast<std::size_t>(f) * num_out_ch) + ch] = src[f];
            }
        }

        const uint64_t w_lo = std::max(frames_done, win_start);
        const uint64_t w_hi = std::min(frames_done + frames_now, win_end);
        const bool emit = w_hi > w_lo;
        const std::size_t emit_off = emit ? static_cast<std::size_t>(w_lo - frames_done) : 0;
        const std::size_t emit_count = emit ? static_cast<std::size_t>(w_hi - w_lo) : 0;

        if (emit && writer.write(out_interleaved.data() + (emit_off * num_out_ch), emit_count) != emit_count) {
            return make_error(ErrorCode::io_error, "short write while rendering", "output=" + plan.output_path);
        }

        if (emit && lufs_st) {
            ebur128_state* state = lufs_st.get();
            const float* data = out_interleaved.data() + (emit_off * num_out_ch);
            meter_pending.at(buf_idx) =
                meter.post([state, data, emit_count] { ebur128_add_frames_float(state, data, emit_count); });
        }

        buf_idx = (buf_idx + 1U) % k_num_buffers;
        frames_done += frames_now;
        const uint64_t progress_done = std::min(frames_done, win_end) - start_pos;
        const double stage_fraction = static_cast<double>(progress_done) / progress_span;
        const double frac = 0.3 + (0.6 * stage_fraction);
        progress.on_progress({RenderStage::rendering,
                              RenderOperation::render_audio,
                              frac,
                              stage_fraction,
                              progress_done,
                              progress_total,
                              "rendering"});
    }

    for (auto& pending : meter_pending) {
        if (pending.valid()) {
            pending.get();
        }
    }

    progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
    logs.log(LogLevel::info, "apple", fmt::format("wrote {} frames to {}", frames_to_write, plan.output_path));
    return collect_metrics(lufs_st.get(), num_out_ch);
}

} // namespace

CapabilityReport apple_capabilities() {
    CapabilityReport r;
    r.backend_name = "apple";
    r.backend_version = "spatial-mixer-0.3";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supports_channel_lock = true;      // snaps to the output speaker set (speaker output; binaural drops)
    r.supports_object_divergence = true; // expand_object_divergence -> parallel buses
    r.supports_screen_ref = false;
    r.supports_diffuse = false;      // SpatialMixer has no ADM decorrelator
    r.supports_render_window = true; // seek + one-block pre-roll for SpatialMixer state
    r.supported_layouts = {
        {"binaural", "Apple AUSpatialMixer binaural", 2, false, 0, true, true},
    };
    for (const auto& layout : k_apple_speaker_layouts) {
        r.supported_layouts.push_back({std::string{layout.id},
                                       std::string{layout.display_name},
                                       layout.channels,
                                       layout.is_3d,
                                       layout.lfe_count,
                                       true, // supports_spread: 17-point extent disk cloud
                                       false});
    }
    return r;
}

std::unique_ptr<IRenderer> create_apple_renderer() {
    return std::make_unique<AppleRenderer>();
}

} // namespace mradm
