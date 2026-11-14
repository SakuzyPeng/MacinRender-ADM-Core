#pragma once

#include <memory>

#include "adm/renderer.h"

#include "engine_test_ffi.h"

namespace mradm::render_common {

// A Rust backend preparation (mradm-engine `backend`): immutable and reused
// across offline windows and file streams (PreviewSession scrubbing). The Rust preparation keeps
// the input format the stream checks compare.
struct EnginePrepared final : IPreparedRender {
    std::unique_ptr<MradmRenderPrepared, decltype(&mradm_render_prepared_destroy)> handle{
        nullptr, &mradm_render_prepared_destroy};
};

} // namespace mradm::render_common
