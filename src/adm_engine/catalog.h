#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adm/render.h"

#include "engine_ffi.h"

namespace mradm::engine {

// The discovery documents built in Rust mradm-engine. This side supplies
// the island facts: encoder availability, the Apple backend and the system-spatial layouts.
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

// The platform facts of an offline render: IAMF availability and, in Apple
// builds, the Apple backend's capability records.
class RenderPlatform {
  public:
    RenderPlatform();
    ~RenderPlatform();
    RenderPlatform(const RenderPlatform&) = delete;
    RenderPlatform& operator=(const RenderPlatform&) = delete;
    RenderPlatform(RenderPlatform&&) = delete;
    RenderPlatform& operator=(RenderPlatform&&) = delete;
    [[nodiscard]] const MradmEnginePlatform* get() const noexcept { return &platform_; }

  private:
    class Apple;
    std::unique_ptr<Apple> apple_;
    MradmEnginePlatform platform_{0, 0, 0, -1, nullptr, nullptr, 0};
};

} // namespace mradm::engine
