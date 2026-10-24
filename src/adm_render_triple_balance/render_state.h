#pragma once
#include <memory>

#include "adm/render.h"

#include "speaker_pcm.h"
namespace mradm::triple_balance {
// The Rust preparation (mradm-render `triple_balance`, PCM consistency 4.6): the offline kernel,
// and in standard mode the file stream's plan, live keys and size tracks.
struct Prepared final : IPreparedRender {
    std::shared_ptr<render_common::RustPrepared> rust;
    uint16_t output_channels{};
    uint32_t sample_rate{};
    TripleBalanceMode mode{TripleBalanceMode::standard};
};
Result<std::unique_ptr<IRenderStream>> open_stream(const Prepared& prepared, const RenderPlan& plan);
} // namespace mradm::triple_balance
