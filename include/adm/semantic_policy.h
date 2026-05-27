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
    std::optional<bool> enabled;
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
};

struct SemanticObjectRule : SemanticPolicyOverride {
    std::string id;
    std::string name;
    std::string name_glob;
    std::string track_uid;
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

} // namespace mradm
