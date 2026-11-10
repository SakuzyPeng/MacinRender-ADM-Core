#include "island_stream.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>

namespace mradm::realtime {

namespace {

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

// A C++ stream behind the island table. The stop source is replaced per cancellable seek.
struct Island {
    std::shared_ptr<void> keep; // declared first: outlives the stream that borrows it
    std::unique_ptr<IRenderStream> stream;
    std::mutex cancel_mutex;
    std::stop_source cancel;
};

Island& island(void* ctx) {
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

} // namespace

int32_t island_error(const Error& error, const MradmEngineIslandError* e) noexcept {
    return fail(error, e);
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
        return guarded(e, [&] { return produce(island(ctx).stream->process({out, len}, frames), produced, e); });
    };
    t.seek = [](void* ctx, uint64_t frame, const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] { return done(island(ctx).stream->seek(frame), e); });
    };
    t.cancel_begin = [](void* ctx) noexcept {
        auto& i = island(ctx);
        const std::lock_guard lock(i.cancel_mutex);
        i.cancel = std::stop_source{};
    };
    t.cancel_request = [](void* ctx) noexcept {
        auto& i = island(ctx);
        const std::lock_guard lock(i.cancel_mutex);
        i.cancel.request_stop();
    };
    t.seek_cancellable = [](void* ctx, uint64_t frame, const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] {
            auto& i = island(ctx);
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
        return guarded(
            e, [&] { return done(island(ctx).stream->validate_overrides(overrides_of(values, count, revision)), e); });
    };
    t.set_overrides = [](void* ctx, const MradmRenderLiveOverride* values, size_t count, uint64_t revision) noexcept {
        island(ctx).stream->set_overrides(overrides_of(values, count, revision));
    };
    t.set_orientation = [](void* ctx, float yaw, float pitch, float roll) noexcept {
        island(ctx).stream->set_listener_orientation({yaw, pitch, roll});
    };
    t.produce_intermediate = [](void* ctx,
                                float* out,
                                size_t len,
                                size_t frames,
                                size_t* produced,
                                const MradmEngineIslandError* e) noexcept {
        return guarded(
            e, [&] { return produce(island(ctx).stream->produce_intermediate({out, len}, frames), produced, e); });
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
        return island(ctx).stream->render_output({in, in_len}, {out, out_len}, frames, {yaw, pitch, roll});
    };
    t.reposition_source = [](void* ctx, uint64_t frame, const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] { return done(island(ctx).stream->reposition_source(frame), e); });
    };
    t.loop_render_reset = [](void* ctx, uint64_t frame) noexcept { island(ctx).stream->loop_render_reset(frame); };
    t.render_failed = [](void* ctx) noexcept -> uint32_t { return island(ctx).stream->render_failed() ? 1U : 0U; };
    t.destroy = [](void* ctx) noexcept { delete static_cast<Island*>(ctx); };
    return t;
}

} // namespace mradm::realtime
