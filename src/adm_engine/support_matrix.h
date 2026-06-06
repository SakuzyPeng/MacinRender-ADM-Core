#pragma once

#include <string>

namespace mradm::engine {

// Serialize the renderer/layout/output-target support matrix to JSON (UTF-8).
// The matrix combines renderer capabilities, layout/channel-order support, and
// output-format/container constraints so GUI clients do not need to join the
// separate capabilities/layouts/formats tables themselves.
[[nodiscard]] std::string render_support_matrix_to_json();

} // namespace mradm::engine
