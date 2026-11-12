#include "apple_island.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>

#include "adm/render.h"
#include "adm/render_apple.h"

#include "../adm_audio/island_driver.h"
#include "../adm_core/direct_matrix_records.h"
#include "../adm_render_common/scene_handle.h"

// The Apple backend behind the engine: RenderPlans from the engine's settings over the existing
// AUSpatialMixer renderer, and its stream as the monitor's island table. Exceptions stay on this
// side of the Rust frames.
namespace mradm {
namespace {

using island::text;

// Holds the backend's prepared state; streams and renders share it.
struct Prepared {
    std::shared_ptr<IPreparedRender> state;
};

// Both members retain the Apple backend for as long as its stream lives.
struct KeepAlive {
    std::unique_ptr<IRenderer> renderer; // cppcheck-suppress unusedStructMember
    // cppcheck-suppress unusedStructMember
    std::shared_ptr<IPreparedRender> prepared;
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

constexpr int32_t k_thrown = 100;

void write(std::string_view text, uint8_t* out, size_t* len) {
    if (len == nullptr) {
        return;
    }
    const auto n = (*len == 0U || out == nullptr) ? 0U : std::min(text.size(), *len - 1U);
    if (out != nullptr && *len != 0U) {
        std::copy_n(text.data(), n, out);
        out[n] = 0U;
    }
    *len = n;
}

int32_t fail(const Error& error, const MradmEngineIslandError* e) noexcept {
    write(error.message, e->message, e->message_len);
    write(error.context, e->context, e->context_len);
    return static_cast<int32_t>(error.code);
}

int32_t fail(const char* what, const MradmEngineIslandError* e) noexcept {
    return fail(Error{ErrorCode::render_failed, what, {}}, e);
}

std::string_view view(const MradmAdmText& text) {
    return text.data == nullptr ? std::string_view{}
                                : std::string_view{reinterpret_cast<const char*>(text.data), text.len};
}

LiveOverrides overrides_of(const MradmRenderLiveOverride* values, size_t count, uint64_t revision) {
    LiveOverrides result;
    result.revision = revision;
    result.objects.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& v = values[i];
        LiveObjectOverride o;
        o.object_id = std::string{view(v.base.object_id)};
        o.speaker_label = std::string{view(v.base.speaker_label)};
        o.mute = v.base.mute != 0U;
        o.gain_db = v.base.gain_db;
        if (v.base.has_head_locked != 0U) {
            o.head_locked = v.base.head_locked != 0U;
        }
        o.extent_scale = v.extent_scale;
        o.extent_width_scale = v.extent_width_scale;
        o.extent_height_scale = v.extent_height_scale;
        o.extent_depth_scale = v.extent_depth_scale;
        o.diffuse_scale = v.diffuse_scale;
        o.divergence_scale = v.divergence_scale;
        result.objects.push_back(std::move(o));
    }
    return result;
}

// Retained state outlives the borrowing stream. The stop source is replaced per cancellable seek.
struct Island {
    std::shared_ptr<void> keep; // cppcheck-suppress unusedStructMember
    std::unique_ptr<IRenderStream> stream;
    std::mutex cancel_mutex;
    std::stop_source cancel;
};

Island& island_of(void* ctx) {
    return *static_cast<Island*>(ctx);
}

template <class F> int32_t guarded(const MradmEngineIslandError* e, const F& f) noexcept {
    try {
        return f();
    } catch (const std::exception& error) {
        return fail(error.what(), e);
    } catch (...) {
        return fail("unknown exception", e);
    }
}

int32_t produce(Result<std::size_t> r, size_t* produced, const MradmEngineIslandError* e) {
    if (!r) {
        return fail(r.error(), e);
    }
    *produced = *r;
    return 0;
}

int32_t done(const Result<void>& r, const MradmEngineIslandError* e) {
    return r ? 0 : fail(r.error(), e);
}


MradmEngineIslandStream island_table(std::unique_ptr<IRenderStream> stream, std::shared_ptr<void> keep) {
    const bool output_stage = stream->renders_orientation_at_output() && stream->intermediate_channels() > 0U;
    MradmEngineIslandStream t{};
    t.out_channels = stream->out_channels();
    t.sample_rate = stream->sample_rate();
    t.requires_preroll = stream->seek_requires_preroll() ? 1U : 0U;
    t.intermediate_channels = output_stage ? stream->intermediate_channels() : 0U;
    t.renders_orientation = stream->renders_orientation_at_output() ? 1U : 0U;
    t.ctx = new Island{std::move(keep), std::move(stream), {}, {}};
    t.process = [](void* ctx,
                   float* out,
                   size_t len,
                   size_t frames,
                   size_t* produced,
                   const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] { return produce(island_of(ctx).stream->process({out, len}, frames), produced, e); });
    };
    t.seek = [](void* ctx, uint64_t frame, const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] { return done(island_of(ctx).stream->seek(frame), e); });
    };
    t.cancel_begin = [](void* ctx) noexcept {
        auto& i = island_of(ctx);
        const std::lock_guard lock(i.cancel_mutex);
        i.cancel = std::stop_source{};
    };
    t.cancel_request = [](void* ctx) noexcept {
        auto& i = island_of(ctx);
        const std::lock_guard lock(i.cancel_mutex);
        i.cancel.request_stop();
    };
    t.seek_cancellable = [](void* ctx, uint64_t frame, const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] {
            auto& i = island_of(ctx);
            std::stop_token token;
            {
                const std::lock_guard lock(i.cancel_mutex);
                token = i.cancel.get_token();
            }
            return done(i.stream->seek_with_cancel(frame, token), e);
        });
    };
    t.validate = [](void* ctx,
                    const MradmRenderLiveOverride* values,
                    size_t count,
                    uint64_t revision,
                    const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] {
            return done(island_of(ctx).stream->validate_overrides(overrides_of(values, count, revision)), e);
        });
    };
    t.set_overrides = [](void* ctx, const MradmRenderLiveOverride* values, size_t count, uint64_t revision) noexcept {
        island_of(ctx).stream->set_overrides(overrides_of(values, count, revision));
    };
    t.set_orientation = [](void* ctx, float yaw, float pitch, float roll) noexcept {
        island_of(ctx).stream->set_listener_orientation({yaw, pitch, roll});
    };
    t.produce_intermediate = [](void* ctx,
                                float* out,
                                size_t len,
                                size_t frames,
                                size_t* produced,
                                const MradmEngineIslandError* e) noexcept {
        return guarded(
            e, [&] { return produce(island_of(ctx).stream->produce_intermediate({out, len}, frames), produced, e); });
    };
    t.render_output = [](void* ctx,
                         const float* in,
                         size_t in_len,
                         float* out,
                         size_t out_len,
                         size_t frames,
                         float yaw,
                         float pitch,
                         float roll) noexcept -> size_t {
        return island_of(ctx).stream->render_output({in, in_len}, {out, out_len}, frames, {yaw, pitch, roll});
    };
    t.reposition_source = [](void* ctx, uint64_t frame, const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] { return done(island_of(ctx).stream->reposition_source(frame), e); });
    };
    t.loop_render_reset = [](void* ctx, uint64_t frame) noexcept { island_of(ctx).stream->loop_render_reset(frame); };
    t.render_failed = [](void* ctx) noexcept -> uint32_t { return island_of(ctx).stream->render_failed() ? 1U : 0U; };
    t.destroy = [](void* ctx) noexcept { delete static_cast<Island*>(ctx); };
    return t;
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
        auto prepared = create_apple_renderer()->prepare(plan, logs);
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
        auto metrics = create_apple_renderer()->render_window(state, plan, progress, logs);
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
        auto renderer = create_apple_renderer();
        const auto& state = reinterpret_cast<const Prepared*>(prepared)->state;
        auto stream = renderer->open_stream(*state, plan, logs);
        if (!stream) {
            return fail(stream.error(), e);
        }
        auto keep = std::make_shared<KeepAlive>(KeepAlive{std::move(renderer), state});
        *output = island_table(std::move(*stream), std::move(keep));
        return 0;
    } catch (const std::exception& error) {
        return fail(Error{static_cast<ErrorCode>(k_thrown), error.what(), {}}, e);
    } catch (...) {
        return fail(Error{static_cast<ErrorCode>(k_thrown), "unknown exception", {}}, e);
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
