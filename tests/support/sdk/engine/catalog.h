#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "adm/render.h"
#include "adm/render_service.h"

#include "engine_test_ffi.h"

namespace mradm::engine {

// The discovery documents built in Rust mradm-engine, which reads the island facts (encoder
// availability, the Apple backend and the system-spatial layouts) from mradm_platform_islands().
enum class Document : uint32_t {
    capabilities = 0,
    layouts = 1,
    output_formats = 2,
    input_layouts = 3,
    support_matrix = 4,
};

// The document as UTF-8 JSON (nlohmann dump(2) layout). The output-format documents probe encoder
// availability, which in IAMF builds may spawn a short-lived MP4 packager probe.
[[nodiscard]] std::string document(Document kind);

// The output channel-order reference and the output container formats, read from their documents.
[[nodiscard]] std::vector<OutputLayoutRow> build_output_layouts();
[[nodiscard]] OutputFormats build_output_formats();

} // namespace mradm::engine
