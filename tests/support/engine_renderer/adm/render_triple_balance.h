#pragma once

#include <memory>

#include "adm/render.h"

namespace mradm {

// Offline Cartesian room rendering, independent of SAF. Supported layouts are
// 7.1.4, 9.1.6 and the project's 22.2 extension; see capability/semantic reports.
CapabilityReport triple_balance_capabilities();
std::unique_ptr<IRenderer> create_triple_balance_renderer();

} // namespace mradm
