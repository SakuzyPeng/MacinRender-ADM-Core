#pragma once

#include <cstdint>
#include <string_view>

namespace mradm {

enum class RenderStage {
    validating,
    probing,
    importing_scene,
    planning,
    rendering,
    post_processing,
    finished,
};

enum class RenderOperation {
    unknown,
    validate_request,
    probe_input,
    import_scene,
    apply_semantic_policy,
    plan_render,
    prepare_backend,
    render_audio,
    trim_output,
    apply_gain,
    convert_bit_depth,
    encode_flac,
    encode_opus,
    encode_apac,
    encode_iamf,
    package_iamf_mp4,
    write_metadata,
    finish,
};

// cppcheck-suppress-begin unusedStructMember
struct ProgressEvent {
    RenderStage stage{RenderStage::validating};
    RenderOperation operation{RenderOperation::unknown};
    double fraction{0.0};
    double stage_fraction{0.0};
    uint64_t current_frame{0};
    uint64_t total_frames{0};
    std::string_view message;
};
// cppcheck-suppress-end unusedStructMember

class ProgressSink {
  public:
    virtual ~ProgressSink() = default;
    virtual void on_progress(const ProgressEvent& event) = 0;
};

class NullProgressSink final : public ProgressSink {
  public:
    void on_progress(const ProgressEvent&) override {}
};

} // namespace mradm
