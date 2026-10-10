#include "live_scene_trace.h"

#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
namespace mradm::consistency::detail {
namespace {
// One provider in ADMRenderCommon, including when engine and renderers are separate
// shared libraries. An inline thread_local in the header can be duplicated across DLLs.
thread_local std::string_view current_renderer_role = "current";
} // namespace

std::string_view renderer_role() noexcept {
    return current_renderer_role;
}

std::string_view set_renderer_role(std::string_view role) noexcept {
    const auto previous = current_renderer_role;
    current_renderer_role = role;
    return previous;
}
} // namespace mradm::consistency::detail
#endif
