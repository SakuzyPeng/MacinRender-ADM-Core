#pragma once

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "adm/options.h"
#include "adm/render.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif

#include "../adm_render_room/room_internal.h"
#include "engine_ffi.h"
#include "engine_result.h"
#include "speaker_pcm.h"

// The engine request record (MradmEngineRenderRequest) of a RenderRequest and the Apple island's
// adapter, shared by the offline render service and the monitor's stream opening (PCM consistency
// 5.2 C2, 5.3 B).
namespace mradm::engine {

// The Apple backend the engine hands back as an island; the engine requests it only where the
// platform record carries the Apple backend.
[[nodiscard]] inline Result<std::unique_ptr<IRenderer>> apple_renderer() {
#ifdef __APPLE__
    return create_apple_renderer();
#else
    return make_error(ErrorCode::internal_error, "the Apple backend is not available in this build");
#endif
}

[[nodiscard]] inline std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] inline MradmEnginePath engine_path(const std::string& text) {
    return {render_common::engine_text(text), render_common::engine_text(text)};
}

// The request's texts, owned for the render; paths are UTF-8 for opening and in messages.
class RequestTexts {
  public:
    explicit RequestTexts(const RenderRequest& request) : input(utf8(request.input_path)) {
        const auto& o = request.options;
        if (request.output_path) {
            output = utf8(*request.output_path);
        }
        if (o.sofa_path) {
            sofa = utf8(*o.sofa_path);
            sofa_name = utf8(o.sofa_path->filename());
        }
        if (o.semantic_policy_path) {
            policy = utf8(*o.semantic_policy_path);
        }
        if (o.direct_speakers_matrix_path) {
            matrix = utf8(*o.direct_speakers_matrix_path);
        }
        if (o.semantic_report_path) {
            report = utf8(*o.semantic_report_path);
        }
        hrir = utf8(o.room.hrir_manifest);
        labels.resize(o.input_channel_labels.size());
        std::ranges::transform(
            o.input_channel_labels, labels.begin(), [](std::string_view l) { return render_common::engine_text(l); });
        layers.resize(o.iamf_layers.size());
        std::ranges::transform(
            o.iamf_layers, layers.begin(), [](std::string_view l) { return render_common::engine_text(l); });
    }

    [[nodiscard]] MradmEngineRenderRequest record(const RenderRequest& request) const {
        const auto& o = request.options;
        const auto& screens = o.screens;
        const auto& pose = o.listener_orientation;
        const void* profile = o.room.profile ? room_internal::Access::get(*o.room.profile) : nullptr;
        return {engine_path(input),
                request.output_path ? 1U : 0U,
                engine_path(output),
                static_cast<uint32_t>(o.renderer),
                render_common::engine_text(o.output_layout),
                o.input_layout ? 1U : 0U,
                render_common::engine_text(o.input_layout ? std::string_view{*o.input_layout} : std::string_view{}),
                labels.data(),
                labels.size(),
                o.measure_loudness ? 1U : 0U,
                o.loudness_target_lufs,
                o.peak_limit ? 1U : 0U,
                o.peak_limit_dbtp,
                o.peak_normalize_to_limit ? 1U : 0U,
                o.final_gain_db,
                static_cast<uint32_t>(o.output_bit_depth),
                o.opus_bitrate_per_ch_kbps,
                o.apac_bitrate_kbps,
                o.apac_drc_music ? 1U : 0U,
                o.apac_container == RenderOptions::ApacContainer::caf ? 1U : 0U,
                o.sofa_path ? 1U : 0U,
                engine_path(sofa),
                render_common::engine_text(sofa_name),
                o.semantic_policy_path ? 1U : 0U,
                engine_path(policy),
                o.semantic_policy_json ? 1U : 0U,
                render_common::engine_text(o.semantic_policy_json ? std::string_view{*o.semantic_policy_json}
                                                                  : std::string_view{}),
                o.direct_speakers_matrix_path ? 1U : 0U,
                engine_path(matrix),
                o.direct_speakers_matrix_json ? 1U : 0U,
                render_common::engine_text(o.direct_speakers_matrix_json
                                               ? std::string_view{*o.direct_speakers_matrix_json}
                                               : std::string_view{}),
                o.semantic_report_path ? 1U : 0U,
                engine_path(report),
                o.capture_semantic_report ? 1U : 0U,
                o.default_interp_ms,
                o.object_smoothing_frames,
                o.speaker_geometry == SpeakerGeometry::apple ? 1U : 0U,
                static_cast<uint32_t>(o.direct_speakers_routing_mode),
                static_cast<uint32_t>(o.speaker_spread_mode),
                static_cast<uint32_t>(o.triple_balance_mode),
                static_cast<uint32_t>(o.binaural_spread_mode),
                o.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U,
                static_cast<uint32_t>(o.apple_spatial_preset),
                o.apple_speaker_rendering_flags ? 1U : 0U,
                {pose.yaw_deg, pose.pitch_deg, pose.roll_deg},
                o.render_start_sec,
                o.render_end_sec ? 1U : 0U,
                o.render_end_sec.value_or(0.0),
                profile,
                render_common::engine_text(hrir),
                o.room.metres_per_unit,
                static_cast<uint32_t>(o.room.quality),
                screens.reference ? 1U : 0U,
                render_common::screen_geometry(screens.reference.value_or(ScreenGeometry{})),
                screens.reproduction ? 1U : 0U,
                render_common::screen_geometry(screens.reproduction.value_or(ScreenGeometry{})),
                o.internal_allow_speaker_stereo ? 1U : 0U,
                o.iamf_container == RenderOptions::IamfContainer::mp4 ? 1U : 0U,
                layers.data(),
                layers.size()};
    }

  private:
    std::string input;
    std::string output;
    std::string sofa;
    std::string sofa_name;
    std::string policy;
    std::string matrix;
    std::string report;
    std::string hrir;
    std::vector<MradmAdmText> labels;
    std::vector<MradmAdmText> layers;
};

} // namespace mradm::engine
