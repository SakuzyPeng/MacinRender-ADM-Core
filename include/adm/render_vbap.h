#pragma once

#include <memory>

#include "adm/render.h"

namespace mradm {

CapabilityReport vbap_capabilities();
std::unique_ptr<IRenderer> create_vbap_renderer();

} // namespace mradm
