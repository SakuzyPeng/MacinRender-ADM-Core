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
    float azimuth{0.0f};
    float elevation{0.0f};
    float distance{1.0f};
    bool has_position{false};
    float gain{1.0f};
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
};

struct SceneContent {
    std::string id;
    std::string name;
    std::vector<std::string> object_ids;
};

struct SceneProgramme {
    std::string id;
    std::string name;
    std::vector<std::string> content_ids;
};

struct AdmScene {
    SceneInfo info;
    std::vector<SceneProgramme> programmes;
    std::vector<SceneContent> contents;
    std::vector<SceneObject> objects;
};

} // namespace mradm
