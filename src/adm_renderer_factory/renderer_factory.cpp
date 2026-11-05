#include "renderer_factory.h"

#include <stdexcept>

#include "engine_result.h"

// Selection, routing and matrix rules live in Rust mradm-engine `request` and the monitor's stream
// opening in `stream`; the Apple island constructs its own adapter.
namespace mradm {

namespace {

using render_common::engine_text;

const uint8_t* bytes(std::string_view text) noexcept {
    return reinterpret_cast<const uint8_t*>(text.data());
}

} // namespace

std::string normalize_output_layout(const std::string& layout) {
    MradmAdmText out{};
    if (mradm_engine_normalize_layout(bytes(layout), layout.size(), &out) != 0) {
        throw std::logic_error("output layout normalization failed");
    }
    return engine_text(out);
}

} // namespace mradm
