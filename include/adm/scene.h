#pragma once

#include <cstdint>
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

// Reference from a SceneObject to a specific track in the BW64 file.
struct SceneTrackRef {
    // 0-based channel index; nullopt when the UID has no matching CHNA entry.
    std::optional<uint16_t> channel_index;
    std::string track_uid; // AudioTrackUID identifier (e.g. "ATU_00000001")
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
