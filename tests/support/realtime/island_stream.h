#pragma once

#include <cstdint>
#include <memory>

#include "adm/errors.h"
#include "adm/render.h"

#include "engine_ffi.h"

// A C++ IRenderStream behind the Rust monitor's island table: the
// trampolines catch every exception (a render_failed error with its what()) and carry the
// override revision. `keep` (an Apple backend's renderer and prepared state) is released with the
// stream.
namespace mradm::realtime {

[[nodiscard]] MradmEngineIslandStream island_table(std::unique_ptr<IRenderStream> stream,
                                                   std::shared_ptr<void> keep = {});

// Writes `error` into the island error buffers and returns its code.
int32_t island_error(const Error& error, const MradmEngineIslandError* e) noexcept;

} // namespace mradm::realtime
