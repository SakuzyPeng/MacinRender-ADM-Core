#include "apple_island.h"

#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "adm/render.h"

#include "../adm_audio/island_driver.h"
#include "../adm_core/direct_matrix_records.h"
#include "../adm_render_common/scene_handle.h"
#include "apple_backend.h"

// The Apple backend behind the engine: RenderPlans from the engine's settings over the existing
// AUSpatialMixer backend, whose stream is the monitor's island table. Exceptions stay on this
// side of the Rust frames.
namespace mradm {
namespace {

using island::text;

// Holds the backend's prepared state; streams and renders share it.
struct Prepared {
    std::shared_ptr<const apple::Prepared> state;
};

[[nodiscard]] RenderPlan plan_of(const MradmAppleSettings& s) {
    RenderPlan plan;
    plan.input_path = island::native(s.input);
    plan.output_layout = text(s.layout);
    plan.direct_speakers_routing_mode = static_cast<DirectSpeakersRoutingMode>(s.routing);
    if (s.has_matrix != 0U) {
        plan.direct_speakers_matrix = std::make_shared<const DirectSpeakersMatrix>(detail::matrix_from_view(s.matrix));
    }
    plan.lfe_routing_mode = s.split_power != 0U ? LfeRoutingMode::split_power : LfeRoutingMode::direct;
    plan.speaker_spread_mode = static_cast<SpeakerSpreadMode>(s.speaker_spread);
    plan.binaural_spread_mode = static_cast<BinauralSpreadMode>(s.binaural_spread);
    plan.apple_spatial_preset = static_cast<AppleSpatialPreset>(s.spatial_preset);
    plan.apple_speaker_rendering_flags = s.speaker_rendering_flags != 0U;
    plan.listener_orientation = {s.orientation[0], s.orientation[1], s.orientation[2]};
    return plan;
}

void reply_error(MradmEngineIslandReply& reply, const Error& error) {
    island::fail(reply, error);
}

// The render islands catch everything: the engine reports it as an internal error.
template <class F> int32_t replied(MradmEngineIslandReply* reply, const F& f) noexcept {
    if (reply == nullptr) {
        return 1;
    }
    *reply = {};
    reply->ffmpeg_major = -1;
    try {
        f(*reply);
    } catch (...) {
        reply_error(*reply, {ErrorCode::internal_error, "render island failed", {}});
    }
    return 0;
}

int32_t prepare(const MradmAppleSettings* settings,
                MradmSceneHandle* scene,
                const MradmIslandContext* context,
                MradmApplePrepared** output,
                MradmEngineIslandReply* reply) noexcept {
    // The handle owns the scene from here on, whatever happens.
    std::shared_ptr<const detail::RenderSceneHandle> owned;
    try {
        owned = std::make_shared<const detail::RenderSceneHandle>(scene);
    } catch (...) {
        mradm_scene_destroy(scene);
        return replied(reply, [](MradmEngineIslandReply& r) {
            reply_error(r, {ErrorCode::internal_error, "render island failed", {}});
        });
    }
    if (settings == nullptr || context == nullptr || output == nullptr) {
        return 1;
    }
    *output = nullptr;
    return replied(reply, [&](MradmEngineIslandReply& r) {
        auto plan = plan_of(*settings);
        plan.scene_handle = owned;
        island::Logs logs(*context);
        auto prepared = apple::prepare(plan, logs);
        if (!prepared) {
            reply_error(r, prepared.error());
            return;
        }
        *output =
            reinterpret_cast<MradmApplePrepared*>(std::make_unique<Prepared>(Prepared{std::move(*prepared)}).release());
    });
}

int32_t render(const MradmApplePrepared* prepared,
               const MradmAppleSettings* settings,
               MradmAdmText output,
               uint32_t has_window,
               uint64_t window_start,
               uint64_t window_count,
               const MradmIslandContext* context,
               MradmEngineIslandReply* reply) noexcept {
    if (prepared == nullptr || settings == nullptr || context == nullptr) {
        return 1;
    }
    return replied(reply, [&](MradmEngineIslandReply& r) {
        auto plan = plan_of(*settings);
        plan.output_path = island::native(output);
        if (has_window != 0U) {
            plan.render_window = RenderWindow{window_start, window_count};
        }
        const island::Cancel cancel(*context);
        plan.cancel_token = cancel.token();
        island::Progress progress(*context);
        island::Logs logs(*context);
        const auto& state = *reinterpret_cast<const Prepared*>(prepared)->state;
        auto metrics = apple::render_window(state, plan, progress, logs);
        if (!metrics) {
            reply_error(r, metrics.error());
            return;
        }
        r.has_lufs = metrics->measured_lufs ? 1U : 0U;
        r.lufs = metrics->measured_lufs.value_or(0.0);
        r.has_peak = metrics->measured_peak_dbtp ? 1U : 0U;
        r.peak_dbtp = metrics->measured_peak_dbtp.value_or(0.0);
    });
}

int32_t open_stream(const MradmApplePrepared* prepared,
                    const MradmAppleSettings* settings,
                    const MradmIslandContext* context,
                    MradmEngineIslandStream* output,
                    const MradmEngineIslandError* e) noexcept {
    if (prepared == nullptr || settings == nullptr || context == nullptr || output == nullptr || e == nullptr) {
        return 1;
    }
    try {
        const auto plan = plan_of(*settings);
        island::Logs logs(*context);
        auto stream = apple::open_stream(*reinterpret_cast<const Prepared*>(prepared)->state, plan, logs);
        if (!stream) {
            return apple::fail(stream.error(), e);
        }
        *output = *stream;
        return 0;
    } catch (const std::exception& error) {
        return apple::fail(Error{static_cast<ErrorCode>(apple::k_thrown), error.what(), {}}, e);
    } catch (...) {
        return apple::fail(Error{static_cast<ErrorCode>(apple::k_thrown), "unknown exception", {}}, e);
    }
}

void destroy(MradmApplePrepared* prepared) noexcept {
    delete reinterpret_cast<Prepared*>(prepared);
}

constexpr MradmAppleIslands k_islands{&prepare, &render, &open_stream, &destroy};

} // namespace
} // namespace mradm

extern "C" const MradmAppleIslands* mradm_apple_islands(void) {
    return &mradm::k_islands;
}
