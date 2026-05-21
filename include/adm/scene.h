#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace mradm {

struct SceneInfo {
    uint32_t sample_rate{0};
    uint16_t num_channels{0};
    uint64_t num_frames{0};
    std::string file_path;
};

// Spatial position from one AudioBlockFormatObjects (polar or Cartesian).
struct SceneBlockPosition {
    bool cartesian{false};
    float azimuth{0.0f};
    float elevation{0.0f};
    float distance{1.0f};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// Rendering metadata from one AudioBlockFormatObjects.
struct SceneObjectBlock {
    SceneBlockPosition position;
    float gain{1.0f};
    float diffuse{0.0f};
    float width{0.0f};
    float height{0.0f};
    float depth{0.0f};
    // Sample-accurate time window within the file.  end_sample == UINT64_MAX
    // means the block extends to the end of the file (duration not specified).
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    // When jump_position is true the renderer switches gains instantly at
    // start_sample.  Otherwise it linearly interpolates from the previous block.
    // interp_length_samples == nullopt means use the renderer-default ramp.
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
    // P2 fields: libear throws not_implemented if these are non-default.
    // Importer reads them so renderers can warn and degrade before calling libear.
    bool channel_lock{false};
    float divergence{0.0f};
    bool screen_ref{false};
};

// Rendering metadata from one AudioBlockFormatDirectSpeakers.
struct SceneDirectSpeakersBlock {
    std::vector<std::string> speaker_labels;
    std::string pack_format_id;
    // Nominal position (always polar — Cartesian positions are converted at import time).
    float azimuth{0.0f};
    float elevation{0.0f};
    float distance{1.0f};
    bool has_position{false};
    // Optional position range hints (BS.2076 §8.4); absent when not specified in ADM.
    std::optional<float> azimuth_min;
    std::optional<float> azimuth_max;
    std::optional<float> elevation_min;
    std::optional<float> elevation_max;
    std::optional<float> distance_min;
    std::optional<float> distance_max;
    float gain{1.0f};
    // Set when the parent AudioChannelFormat carries a lowPass channelFrequency element.
    // Identifies this channel as LFE — nearest-speaker fallback is suppressed.
    std::optional<float> low_pass_hz;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
};

// Reference from a SceneObject to a specific track in the BW64 file.
struct SceneTrackRef {
    // 0-based channel index; nullopt when the UID has no matching CHNA entry.
    std::optional<uint16_t> channel_index;
    std::string track_uid; // AudioTrackUID identifier (e.g. "ATU_00000001")
    // Objects blocks from each AudioChannelFormat under the UID's pack format.
    std::vector<SceneObjectBlock> blocks;
    // DirectSpeakers blocks from each AudioChannelFormat under the UID's pack format.
    std::vector<SceneDirectSpeakersBlock> ds_blocks;
};

// AudioObject-level positionOffset (BS.2076 §4.4.1).
// Coordinate system must match the block position being offset.
// Mismatched coordinate systems are ignored (spec violation by content).
struct ScenePositionOffset {
    bool cartesian{false}; // true = X/Y/Z offset; false = azimuth/elevation/distance
    float azimuth{0.0f};
    float elevation{0.0f};
    float distance{0.0f};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// Apply a ScenePositionOffset to a block position.  Returns the modified copy.
[[nodiscard]] inline SceneBlockPosition apply_position_offset(SceneBlockPosition pos, const ScenePositionOffset& off) {
    if (!pos.cartesian && !off.cartesian) {
        pos.azimuth += off.azimuth;
        pos.elevation = std::clamp(pos.elevation + off.elevation, -90.0F, 90.0F);
        pos.distance = std::max(0.0F, pos.distance + off.distance);
    } else if (pos.cartesian && off.cartesian) {
        pos.x += off.x;
        pos.y += off.y;
        pos.z += off.z;
    }
    return pos;
}

struct SceneObject {
    std::string id;
    std::string name;
    float gain{1.0f};
    bool mute{false};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    std::optional<ScenePositionOffset> position_offset;
    std::vector<SceneTrackRef> tracks;
    std::vector<std::string> labels; // audioObjectLabel values
    std::optional<int> importance;   // 0–10; absent when not specified
    // dialogue / dialogueId: 0=non-dialogue, 1=dialogue, 2=mixed; absent when unset.
    // Note: BS.2076-2 defines a default of 2, but libadm treats it as optional.
    std::optional<unsigned int> dialogue_id;
};

// Time-window and gain from one AudioBlockFormatHoa entry.
struct SceneHOAChannelBlock {
    float gain{1.0f}; // AudioBlockFormatHoa.gain (DefaultParameter, default 1)
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
};

// One HOA input channel within a HOA pack (one UID/BW64 track).
// blocks is empty when no AudioBlockFormatHoa is found; such channels are
// skipped by the importer and must not appear in SceneHOATracks.channels.
struct SceneHOAChannel {
    std::optional<uint16_t> channel_index; // 0-based BW64 channel; nullopt if missing from CHNA
    std::string track_uid;
    int order{0};  // from first AudioBlockFormatHoa (fixed per channel)
    int degree{0}; // from first AudioBlockFormatHoa (fixed per channel)
    std::vector<SceneHOAChannelBlock> blocks;
};

// All channels belonging to one (AudioObject, AudioPackFormat) HOA pack.
// channels[i] must be in the same order as orders/degrees will be passed to
// GainCalculatorHOA::calculate(), i.e. the decode matrix row i maps to
// channels[i].channel_index.
struct SceneHOATracks {
    std::string object_id;
    std::string pack_format_id;
    std::string normalization{"SN3D"};
    double nfc_ref_dist{0.0};
    bool screen_ref{false};
    float gain{1.0f};
    bool mute{false};
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    std::vector<SceneHOAChannel> channels;
};

// Loudness metadata from AudioProgramme (BS.2076 §4.1.7 / BS.2127 §5.2.2).
// All fields are optional — only present when the ADM document sets them.
struct SceneLoudnessMetadata {
    std::optional<float> integrated_loudness; // LUFS  (IntegratedLoudness)
    std::optional<float> max_true_peak;       // dBTP  (MaxTruePeak)
    std::optional<float> loudness_range;      // LU    (LoudnessRange)
    std::optional<float> max_momentary;       // LUFS  (MaxMomentary)
    std::optional<float> max_short_term;      // LUFS  (MaxShortTerm)
    std::optional<float> dialogue_loudness;   // LUFS  (DialogueLoudness)
    std::optional<std::string> loudness_method;
};

struct SceneContent {
    std::string id;
    std::string name;
    std::vector<std::string> object_ids;
    std::optional<std::string> language;           // audioContentLanguage
    std::vector<std::string> labels;               // audioContentLabel values
    std::optional<SceneLoudnessMetadata> loudness; // first loudnessMetadata entry
    // dialogue / contentKind (BS.2076 §4.3): human-readable strings or absent.
    // dialogue_kind: "non-dialogue" | "dialogue" | "mixed"
    // content_kind:  sub-kind string (e.g. "music", "voiceover", "complete-main")
    std::optional<std::string> dialogue_kind;
    std::optional<std::string> content_kind;
};

struct SceneProgramme {
    std::string id;
    std::string name;
    std::vector<std::string> content_ids;
    std::optional<std::string> language; // audioProgrammeLanguage
    std::vector<std::string> labels;     // audioProgrammeLabel values
    // Programme time bounds within the file.  start_sample is 0 when unset (default).
    // end_sample is absent when no <end> element is present (programme runs to file end).
    uint64_t start_sample{0};
    std::optional<uint64_t> end_sample;
    // First LoudnessMetadata entry from the programme, if any.
    std::optional<SceneLoudnessMetadata> loudness;
    // True when the ADM document includes an audioProgrammeReferenceScreen element.
    // libadm parses only the element's presence, not its inner geometry (screenCentrePosition,
    // screenWidth, aspectRatio), so no further detail is available here.
    bool has_reference_screen{false};
};

struct AdmScene {
    SceneInfo info;
    std::vector<SceneProgramme> programmes;
    std::vector<SceneContent> contents;
    std::vector<SceneObject> objects;
    std::vector<SceneHOATracks> hoa_tracks;
    // Warnings produced during import (e.g. skipped typeDefinitions).
    // Renderers emit these via LogSink before rendering starts.
    std::vector<std::string> import_warnings;
};

} // namespace mradm
