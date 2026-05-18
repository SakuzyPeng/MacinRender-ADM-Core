#pragma once

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

struct SceneObject {
    std::string id;
    std::string name;
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
