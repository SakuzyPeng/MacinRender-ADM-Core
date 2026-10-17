#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "adm/capability.h"
#include "adm/io.h"

namespace mradm::detail {
// One imported Rust render scene (mradm-scene); defined in scene_importer.cpp.
struct RenderSceneHandle;
} // namespace mradm::detail

namespace mradm::io {

struct SemanticReportOptions {
    std::string renderer;
    std::string policy_path;
    CapabilityReport capabilities;
    // The backend's own JSON object, reported as renderer_effective when not empty.
    std::string renderer_effective_json;
};

// The Rust render scene and its public AdmScene (PCM consistency 4.3, the semantic policy since 4.4).
// The import, the policy, its report and template, and the render-scene checkpoints are Rust; this
// adapter rebuilds the public scene from the scene's view whenever the scene changes.
class RenderScene {
  public:
    // options == nullptr imports ADM BW64 only; otherwise the options route ADM or a channel bed.
    // `checkpoint` names the render-scene checkpoint the import records in diagnostic builds.
    [[nodiscard]] static Result<RenderScene>
    import(const std::string& path, const SceneImportOptions* options, const char* checkpoint = nullptr);

    [[nodiscard]] const AdmScene& scene() const { return scene_; }
    [[nodiscard]] std::shared_ptr<const detail::RenderSceneHandle> handle() const { return handle_; }

    // Applies mradm.semantic-policy.v1 JSON; errors carry `label` (`<memory>` or the file path).
    Result<void> apply_policy(std::string_view json, const std::string& label);
    // The "rule did not match" warnings of the applied policy.
    [[nodiscard]] std::vector<std::string> policy_warnings() const;
    // mradm.semantic-report.v1 of the original against the current scene; no trailing newline.
    [[nodiscard]] Result<std::string> semantic_report(const SemanticReportOptions& options) const;
    // The neutral policy template of the scene; no trailing newline.
    [[nodiscard]] Result<std::string> policy_template() const;
    // Writes render-scene/offline.<stage>.i32 in diagnostic builds; a no-op otherwise.
    void checkpoint(const char* stage) const;

  private:
    RenderScene(std::shared_ptr<detail::RenderSceneHandle> handle, std::string path)
        : handle_(std::move(handle)), path_(std::move(path)) {}
    Result<void> refresh();

    std::shared_ptr<detail::RenderSceneHandle> handle_;
    std::string path_;
    AdmScene scene_;
};

// A policy file's text; I/O errors use the policy's own messages and the path as context.
Result<std::string> read_policy_file(const std::filesystem::path& path);
// Writes a report or template with a trailing newline, LF on every platform. `what` names it in
// the I/O error messages ("report", "policy template").
Result<void> write_policy_json(const std::filesystem::path& path, std::string_view json, std::string_view what);

} // namespace mradm::io
