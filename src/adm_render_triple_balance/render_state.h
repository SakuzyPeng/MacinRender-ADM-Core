#pragma once
#include <memory>

#include "adm/render.h"

#include "speaker_pcm.h"
namespace mradm::triple_balance {
// The Rust preparation (mradm-engine `backend`, mradm-render `triple_balance`, PCM consistency 4.6,
// 5.2 C1): the offline kernel, and in standard mode the file stream's plan, live keys and size
// tracks.
using Prepared = render_common::EnginePrepared;
Result<std::unique_ptr<IRenderStream>> open_stream(const Prepared& prepared, const RenderPlan& plan);
} // namespace mradm::triple_balance
