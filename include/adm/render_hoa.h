#pragma once

#include <memory>

#include "adm/render.h"

namespace mradm {

CapabilityReport hoa_capabilities();
std::unique_ptr<IRenderer> create_hoa_renderer();

} // namespace mradm
