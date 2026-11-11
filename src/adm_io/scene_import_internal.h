#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "adm/io.h"

struct MradmSceneHandle;

namespace mradm::detail {
// One imported Rust render scene (mradm-scene); defined in adm_render_common/scene_handle.h.
struct RenderSceneHandle;
} // namespace mradm::detail

namespace mradm::io {

// The audio format, the top-level counts and the import warnings of an imported scene.
struct SceneSummary {
    uint32_t sample_rate{0};
    uint16_t channels{0};
    uint64_t frames{0};
    uint32_t programmes{0};
    uint32_t objects{0};
    std::vector<std::string> warnings;
};

// The Rust render scene. The import, the policy (mradm-engine), its report and template, the
// inspect summary and the render-scene checkpoints are Rust; no C++ copy of the scene is built.
class RenderScene {
  public:
    // options == nullptr imports ADM BW64 only; otherwise the options route ADM or a channel bed.
    // `checkpoint` names the render-scene checkpoint the import records in diagnostic builds.
    [[nodiscard]] static Result<RenderScene>
    import(const std::string& path, const SceneImportOptions* options, const char* checkpoint = nullptr);
    // Takes over `raw` (an engine render's scene copy) with `path` as its input path.
    [[nodiscard]] static Result<RenderScene> adopt(MradmSceneHandle* raw, std::string path);

    [[nodiscard]] std::shared_ptr<const detail::RenderSceneHandle> handle() const { return handle_; }

    // The neutral policy template of the scene; no trailing newline.
    [[nodiscard]] Result<std::string> policy_template() const;
    // The scene inspect JSON (mradm.scene-inspect v1).
    [[nodiscard]] Result<std::string> inspect_json() const;
    // The `mradm inspect` summary after its `File:` line.
    [[nodiscard]] Result<std::string> inspect_text() const;
    [[nodiscard]] Result<SceneSummary> summary() const;
    // Writes the source file with this scene's semantic edits patched into its AXML: PCM and every
    // other chunk are copied byte for byte (a temporary file replaces `dst`). Positions, user levels
    // and HOA pack gain or mute are not written back; edits to shared nodes that disagree and edits
    // to common definitions are unsupported.
    [[nodiscard]] Result<void> write(const std::string& dst) const;
    // Writes render-scene/offline.<stage>.i32 in diagnostic builds; a no-op otherwise.
    void checkpoint(const char* stage) const;

  private:
    RenderScene(std::shared_ptr<detail::RenderSceneHandle> handle, std::string path)
        : handle_(std::move(handle)), path_(std::move(path)) {}

    std::shared_ptr<detail::RenderSceneHandle> handle_;
    std::string path_;
};

// Writes a policy template with a trailing newline, LF on every platform. `what` names it in the I/O
// error messages ("policy template").
Result<void> write_policy_json(const std::filesystem::path& path, std::string_view json, std::string_view what);

} // namespace mradm::io
