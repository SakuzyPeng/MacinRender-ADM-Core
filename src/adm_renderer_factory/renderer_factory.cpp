#include "renderer_factory.h"

#include <filesystem>
#include <stdexcept>

#include "adm/direct_speakers_matrix.h"
#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_room.h"
#include "adm/render_triple_balance.h"
#include "adm/render_vbap.h"

#include "../adm_core/direct_matrix_records.h"
#include "engine_result.h"
#include "scene_handle.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif

// Selection, routing and matrix rules live in Rust mradm-engine `request` (PCM consistency 5.2 B);
// this side constructs the C++ IRenderer adapters the streaming paths still drive.
namespace mradm {

namespace {

using render_common::engine_text;
using render_common::EngineOutcome;

const uint8_t* bytes(std::string_view text) noexcept {
    return reinterpret_cast<const uint8_t*>(text.data());
}

#ifdef __APPLE__
constexpr uint32_t k_apple_available = 1U;
#else
constexpr uint32_t k_apple_available = 0U;
#endif

std::unique_ptr<IRenderer> create_renderer(RendererSelection backend) {
    switch (backend) {
    case RendererSelection::ear:
        return create_ear_renderer();
    case RendererSelection::saf:
        return create_vbap_renderer();
    case RendererSelection::triple_balance:
        return create_triple_balance_renderer();
    case RendererSelection::room_binaural:
        return create_room_renderer();
    case RendererSelection::hoa:
        return create_hoa_renderer();
    case RendererSelection::saf_binaural:
        return create_binaural_renderer();
#ifdef __APPLE__
    case RendererSelection::apple:
        return create_apple_renderer();
#endif
    default:
        throw std::logic_error("mradm-engine resolved an unknown backend");
    }
}

} // namespace

std::string normalize_output_layout(const std::string& layout) {
    MradmAdmText out{};
    if (mradm_engine_normalize_layout(bytes(layout), layout.size(), &out) != 0) {
        throw std::logic_error("output layout normalization failed");
    }
    return engine_text(out);
}

Result<ResolvedRenderer>
resolve_renderer(RendererSelection requested, const std::string& requested_layout, bool internal_allow_speaker_stereo) {
    MradmEngineRenderer record{};
    const EngineOutcome outcome([&](MradmEngineResult** out, MradmEngineStatus* status) {
        return mradm_engine_resolve_renderer(static_cast<uint32_t>(requested),
                                             bytes(requested_layout),
                                             requested_layout.size(),
                                             internal_allow_speaker_stereo ? 1U : 0U,
                                             k_apple_available,
                                             &record,
                                             out,
                                             status);
    });
    if (!outcome.ok()) {
        return tl::unexpected{outcome.error()};
    }
    ResolvedRenderer resolved;
    resolved.selected = static_cast<RendererSelection>(record.selected);
    resolved.backend = static_cast<RendererSelection>(record.backend);
    resolved.effective_output_layout = engine_text(record.effective_layout);
    for (const auto& line : outcome.lines()) {
        resolved.diagnostics.emplace_back(static_cast<LogLevel>(line.level), engine_text(line.message));
    }
    resolved.renderer = create_renderer(resolved.backend);
    return resolved;
}

Result<void> validate_direct_speakers_routing(RendererSelection backend,
                                              std::string_view effective_output_layout,
                                              DirectSpeakersRoutingMode mode) {
    const EngineOutcome outcome([&](MradmEngineResult** out, MradmEngineStatus* status) {
        return mradm_engine_validate_routing(static_cast<uint32_t>(backend),
                                             bytes(effective_output_layout),
                                             effective_output_layout.size(),
                                             static_cast<uint32_t>(mode),
                                             out,
                                             status);
    });
    if (!outcome.ok()) {
        return tl::unexpected{outcome.error()};
    }
    return {};
}

Result<std::shared_ptr<const DirectSpeakersMatrix>>
resolve_direct_speakers_matrix(const RenderOptions& options,
                               const detail::RenderSceneHandle& scene,
                               std::string_view effective_output_layout,
                               LogSink& logs) {
    const auto& json = options.direct_speakers_matrix_json;
    const auto& path = options.direct_speakers_matrix_path;
    const auto utf8 = path ? path->u8string() : std::u8string{};
    const std::string_view utf8_path{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    const auto label = path ? path->string() : std::string{};
    const MradmEngineMatrixRequest request{static_cast<uint32_t>(options.direct_speakers_routing_mode),
                                           json ? 1U : 0U,
                                           engine_text(json ? std::string_view{*json} : std::string_view{}),
                                           path ? 1U : 0U,
                                           engine_text(utf8_path),
                                           engine_text(label)};
    uint32_t has_matrix = 0;
    MradmDirectMatrixView view{};
    const EngineOutcome outcome([&](MradmEngineResult** out, MradmEngineStatus* status) {
        return mradm_engine_resolve_matrix(scene.raw,
                                           &request,
                                           bytes(effective_output_layout),
                                           effective_output_layout.size(),
                                           &has_matrix,
                                           &view,
                                           out,
                                           status);
    });
    outcome.emit(logs, "direct-speakers-matrix");
    if (!outcome.ok()) {
        return tl::unexpected{outcome.error()};
    }
    if (has_matrix == 0U) {
        return std::shared_ptr<const DirectSpeakersMatrix>{};
    }
    return std::make_shared<const DirectSpeakersMatrix>(detail::matrix_from_view(view));
}

} // namespace mradm
