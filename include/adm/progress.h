#pragma once

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

struct ProgressEvent {
    RenderStage stage{RenderStage::validating};
    double fraction{0.0};
    std::string_view message;
};

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
