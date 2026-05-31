#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "adm/capability.h"
#include "adm/errors.h"
#include "adm/scene.h"

namespace mradm {

struct DiffusePolicy {
    std::optional<bool> enabled;
    std::optional<float> scale;
    std::optional<float> max;
};

struct ExtentPolicy {
    std::optional<bool> enabled;
    std::optional<float> scale;
    std::optional<float> width_scale;
    std::optional<float> height_scale;
    std::optional<float> depth_scale;
    std::optional<float> max;
};

struct DivergencePolicy {
    std::optional<bool> enabled;
    std::optional<float> scale;
    std::optional<float> range_scale;
    std::optional<float> max_range;
};

struct ChannelLockPolicy {
    std::optional<bool> enabled;       // false disables; true force-enables channel lock
    std::optional<float> max_distance; // channelLockMaxDistance (applies when channel lock is on)
};

// Object-level level control (applied once per AudioObject, not per block).
struct GainPolicy {
    std::optional<float> scale;   // linear multiplier on object gain
    std::optional<float> gain_db; // dB offset (multiplies by 10^(gain_db/20))
    std::optional<bool> mute;     // sets object.mute
};

// Block-level position override. Absolute components overwrite, offset adds,
// lock_* force a fixed value (winning over absolute + offset). Cartesian block
// positions are converted to polar before applying; output is always polar.
struct PositionPolicy {
    std::optional<float> azimuth;   // absolute overwrite (degrees)
    std::optional<float> elevation; // absolute overwrite (degrees)
    std::optional<float> distance;  // absolute overwrite
    struct Offset {
        std::optional<float> azimuth;
        std::optional<float> elevation;
        std::optional<float> distance;
    };
    std::optional<Offset> offset;
    std::optional<float> lock_azimuth;   // force azimuth to this value
    std::optional<float> lock_elevation; // force elevation to this value
};

struct InterpolationPolicy {
    std::optional<bool> honor_jump_position;
    std::optional<uint32_t> max_ms;
};

struct SemanticPolicyOverride {
    std::optional<DiffusePolicy> diffuse;
    std::optional<ExtentPolicy> extent;
    std::optional<DivergencePolicy> divergence;
    std::optional<ChannelLockPolicy> channel_lock;
    std::optional<InterpolationPolicy> interpolation;
    std::optional<GainPolicy> gain;         // object-level
    std::optional<PositionPolicy> position; // block-level
};

struct SemanticObjectRule : SemanticPolicyOverride {
    std::string id;
    std::string name;
    std::string name_glob;
    std::string track_uid;
    // Additional match dimensions (OR-combined with the identity fields above).
    std::optional<bool> all;           // matches every object
    std::optional<int> importance_min; // object.importance >= min (when present)
    std::optional<int> importance_max; // object.importance <= max (when present)
    std::optional<int> dialogue_id;    // object.dialogue_id == value
    std::string content;               // belongs to content with this id or name
    std::string programme;             // belongs to programme with this id or name
};

struct SemanticPolicy {
    static constexpr const char* schema_id = "mradm.semantic-policy.v1";

    std::optional<SemanticPolicyOverride> global;
    std::vector<SemanticObjectRule> objects;
};

struct SemanticPolicyReportOptions {
    std::string renderer;
    std::string policy_path;
    CapabilityReport capabilities;
};

[[nodiscard]] Result<SemanticPolicy> load_semantic_policy_file(const std::filesystem::path& path);

[[nodiscard]] Result<void> apply_semantic_policy(AdmScene& scene,
                                                 const SemanticPolicy& policy,
                                                 uint32_t sample_rate,
                                                 std::vector<std::string>* warnings = nullptr);

[[nodiscard]] Result<void> write_semantic_report_file(const std::filesystem::path& path,
                                                      const AdmScene& original,
                                                      const AdmScene& effective,
                                                      const SemanticPolicy* policy,
                                                      const SemanticPolicyReportOptions& options,
                                                      const std::vector<std::string>& warnings = {});

[[nodiscard]] Result<void> write_semantic_policy_template_file(const std::filesystem::path& path,
                                                               const AdmScene& scene);

} // namespace mradm
