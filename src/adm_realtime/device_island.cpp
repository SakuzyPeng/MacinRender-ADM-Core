#include "device_island.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "adm/errors.h"
#include "adm/options.h"

namespace mradm::realtime {
namespace {

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

IAudioOutputDevice& device_of(void* ctx) {
    return *static_cast<IAudioOutputDevice*>(ctx);
}

std::string text(MradmAdmText t) {
    return t.data == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(t.data), t.len};
}

MradmAdmText view(std::string_view s) noexcept {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// The device controls never threw before (an exception there ended the process).
template <class F> void control(const F& f) noexcept {
    try {
        f();
    } catch (...) {
        std::terminate();
    }
}

int32_t open(uint32_t kind,
             MradmAdmText layout,
             uint32_t geometry,
             MradmAdmText device_id,
             MradmDeviceTable* output,
             const MradmEngineIslandError* e) noexcept {
    if (output == nullptr || e == nullptr) {
        return 1;
    }
    try {
        if (kind == 0U) {
#ifdef __APPLE__
            (void) geometry;
            *output = device_table(make_avsamplebuffer_device(text(layout)));
            return 0;
#elif defined(_WIN32)
            *output =
                device_table(make_spatialaudioclient_device(text(layout), static_cast<SpeakerGeometry>(geometry)));
            return 0;
#else
            (void) layout;
            (void) geometry;
            return fail({ErrorCode::unsupported, "System spatial output is unavailable on this platform", {}}, e);
#endif
        }
        *output = device_table(make_miniaudio_device(kind == 2U, text(device_id)));
        return 0;
    } catch (const std::exception& error) {
        return fail(Error{static_cast<ErrorCode>(k_thrown), error.what(), {}}, e);
    } catch (...) {
        return fail(Error{static_cast<ErrorCode>(k_thrown), "unknown exception", {}}, e);
    }
}

void enumerate(const MradmDeviceEnumeration* devices) noexcept {
    if (devices == nullptr || devices->each == nullptr) {
        return;
    }
    control([&] {
        for (const auto& d : enumerate_output_devices()) {
            devices->each(devices->user, view(d.id), view(d.name), d.is_default ? 1U : 0U);
        }
    });
}

constexpr MradmDeviceIslands k_devices{
#if defined(__APPLE__) || defined(_WIN32)
    1U,
#else
    0U,
#endif
    &open,
    &enumerate};

} // namespace

MradmDeviceTable device_table(std::unique_ptr<IAudioOutputDevice> device) {
    MradmDeviceTable t{};
    t.ctx = device.release();
    t.start = [](void* ctx,
                 uint32_t channels,
                 uint32_t rate,
                 const MradmDevicePull* pull,
                 const MradmEngineIslandError* e) noexcept -> int32_t {
        try {
            const MradmDevicePull p = *pull;
            auto started = device_of(ctx).start(channels, rate, [p](std::span<float> out, std::size_t frames) {
                return p.pull(p.user, out.data(), out.size(), frames);
            });
            return started ? 0 : fail(started.error(), e);
        } catch (const std::exception& error) {
            return fail(Error{static_cast<ErrorCode>(k_thrown), error.what(), {}}, e);
        } catch (...) {
            return fail(Error{static_cast<ErrorCode>(k_thrown), "unknown exception", {}}, e);
        }
    };
    t.stop = [](void* ctx) noexcept { control([&] { device_of(ctx).stop(); }); };
    t.flush = [](void* ctx) noexcept { control([&] { device_of(ctx).flush(); }); };
    t.pause = [](void* ctx) noexcept { control([&] { device_of(ctx).pause(); }); };
    t.resume = [](void* ctx) noexcept { control([&] { device_of(ctx).resume(); }); };
    t.mark_end = [](void* ctx) noexcept { control([&] { device_of(ctx).mark_end(); }); };
    t.realtime = [](void* ctx) noexcept -> uint32_t { return device_of(ctx).pull_is_realtime_playback() ? 1U : 0U; };
    t.has_volume = [](void* ctx) noexcept -> uint32_t { return device_of(ctx).has_device_volume() ? 1U : 0U; };
    t.set_volume = [](void* ctx, float gain) noexcept { control([&] { device_of(ctx).set_volume(gain); }); };
    t.progress = [](void* ctx, MradmEngineSceneDeviceProgress* out) noexcept {
        control([&] {
            const auto p = device_of(ctx).progress();
            *out = {p.presented_frames,
                    p.has_media_clock ? 1U : 0U,
                    p.failed ? 1U : 0U,
                    p.recovering ? 1U : 0U,
                    p.underruns};
        });
    };
    t.destroy = [](void* ctx) noexcept { delete &device_of(ctx); };
    return t;
}

} // namespace mradm::realtime

extern "C" const MradmDeviceIslands* mradm_device_islands(void) {
    return &mradm::realtime::k_devices;
}
