// The macOS AVSampleBufferAudioRenderer sink as a MradmDeviceTable.
#pragma once
#include "../adm_render_common/engine_ffi.h"

#ifdef __cplusplus
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "adm/errors.h"

extern "C" {
#endif

// Built on macOS only.
const MradmSystemSpatialIsland* mradm_system_spatial_island(void);

#ifdef __cplusplus
}

namespace mradm::realtime {

struct AudioDeviceProgress {
    std::uint64_t presented_frames{0};
    bool has_media_clock{false};
    bool failed{false};
    bool recovering{false};
    std::uint64_t underruns{0};
};

// The engine's pull: fills `out` (interleaved, `frames * channels` floats) with up to `frames`
// frames and returns how many, on the sink's thread; it never blocks or allocates.
using PullFn = std::function<std::size_t(std::span<float> out, std::size_t frames)>;

namespace sink {

constexpr int32_t k_thrown = 100;

inline void write(std::string_view text, uint8_t* out, size_t* len) {
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

inline int32_t fail(const Error& error, const MradmEngineIslandError* e) noexcept {
    write(error.message, e->message, e->message_len);
    write(error.context, e->context, e->context_len);
    return static_cast<int32_t>(error.code);
}

// Code 100 with the exception's message, for an exception caught in a catch block.
inline int32_t thrown(const MradmEngineIslandError* e) noexcept {
    try {
        throw;
    } catch (const std::exception& error) {
        return fail(Error{static_cast<ErrorCode>(k_thrown), error.what(), {}}, e);
    } catch (...) {
        return fail(Error{static_cast<ErrorCode>(k_thrown), "unknown exception", {}}, e);
    }
}

// The device controls never threw before (an exception there ended the process).
template <class F> void control(const F& f) noexcept {
    try {
        f();
    } catch (...) {
        std::terminate();
    }
}

} // namespace sink

// The table of a sink (the engine takes it over). Sink has start and stop; flush, pause, resume,
// mark_end, set_volume and progress default to no-ops, has_device_volume to false and
// pull_is_realtime_playback to true.
template <class Sink> MradmDeviceTable device_table(std::unique_ptr<Sink> device) {
    static constexpr auto of = [](void* ctx) -> Sink& { return *static_cast<Sink*>(ctx); };
    MradmDeviceTable t{};
    t.ctx = device.release();
    t.start = [](void* ctx,
                 uint32_t channels,
                 uint32_t rate,
                 const MradmDevicePull* pull,
                 const MradmEngineIslandError* e) noexcept -> int32_t {
        try {
            const MradmDevicePull p = *pull;
            auto started = of(ctx).start(channels, rate, [p](std::span<float> out, std::size_t frames) {
                return p.pull(p.user, out.data(), out.size(), frames);
            });
            return started ? 0 : sink::fail(started.error(), e);
        } catch (...) {
            return sink::thrown(e);
        }
    };
    t.stop = [](void* ctx) noexcept { sink::control([&] { of(ctx).stop(); }); };
    if constexpr (requires(Sink& s) { s.flush(); }) {
        t.flush = [](void* ctx) noexcept { sink::control([&] { of(ctx).flush(); }); };
    }
    if constexpr (requires(Sink& s) { s.pause(); }) {
        t.pause = [](void* ctx) noexcept { sink::control([&] { of(ctx).pause(); }); };
    }
    if constexpr (requires(Sink& s) { s.resume(); }) {
        t.resume = [](void* ctx) noexcept { sink::control([&] { of(ctx).resume(); }); };
    }
    if constexpr (requires(Sink& s) { s.mark_end(); }) {
        t.mark_end = [](void* ctx) noexcept { sink::control([&] { of(ctx).mark_end(); }); };
    }
    t.realtime = [](void* ctx) noexcept -> uint32_t {
        if constexpr (requires(Sink& s) { s.pull_is_realtime_playback(); }) {
            return of(ctx).pull_is_realtime_playback() ? 1U : 0U;
        } else {
            return 1U;
        }
    };
    if constexpr (requires(Sink& s) { s.has_device_volume(); }) {
        t.has_volume = [](void* ctx) noexcept -> uint32_t { return of(ctx).has_device_volume() ? 1U : 0U; };
    }
    if constexpr (requires(Sink& s) { s.set_volume(1.0F); }) {
        t.set_volume = [](void* ctx, float gain) noexcept { sink::control([&] { of(ctx).set_volume(gain); }); };
    }
    if constexpr (requires(Sink& s) { s.progress(); }) {
        t.progress = [](void* ctx, MradmEngineSceneDeviceProgress* out) noexcept {
            sink::control([&] {
                const AudioDeviceProgress p = of(ctx).progress();
                *out = {p.presented_frames,
                        p.has_media_clock ? 1U : 0U,
                        p.failed ? 1U : 0U,
                        p.recovering ? 1U : 0U,
                        p.underruns};
            });
        };
    }
    t.destroy = [](void* ctx) noexcept { delete &of(ctx); };
    return t;
}

#if defined(__APPLE__)
// Enqueues the multichannel bed of `layout_id` (e.g. "4+7+0") into the system media stack, which
// spatializes it with dynamic head tracking (adm_apple/avsamplebuffer_device.mm).
[[nodiscard]] MradmDeviceTable make_avsamplebuffer_device(std::string layout_id);
#endif

} // namespace mradm::realtime
#endif
