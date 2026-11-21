// Private boundary of the islands Rust calls (mradm-engine): the backend capability records the
// platform facts return, render callbacks and replies, the Apple backend's table
// and the output devices. Backend records are process-lifetime Rust data and are never freed. The
// entries only tests and tools call are in tests/support/ffi/engine_test_ffi.h.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../adm_core/config_ffi.h"
#include "../adm_metadata/adm_ffi.h"
#include "../adm_metadata/scene_ffi.h"
#include "routing_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

// flags: 1 3D, 2 supports spread, 4 binaural.
typedef struct MradmEngineLayout {
    MradmAdmText id;
    MradmAdmText display_name;
    uint16_t channel_count;
    uint16_t lfe_count;
    uint32_t flags;
} MradmEngineLayout;

typedef struct MradmEngineMode {
    MradmAdmText id;
    MradmAdmText display_name;
    const MradmAdmText* layouts;
    size_t layouts_len;
    const uint32_t* sample_rates;
    size_t sample_rates_len;
    uint32_t supports_realtime;
} MradmEngineMode;

// flags: 1 objects, 2 DirectSpeakers, 4 HOA, 8 channelLock, 16 divergence, 32 screenRef,
// 64 diffuse, 128 render window.
typedef struct MradmEngineBackend {
    MradmAdmText renderer;
    MradmAdmText name;
    MradmAdmText version;
    const MradmEngineLayout* layouts;
    size_t layouts_len;
    const MradmEngineMode* modes;
    size_t modes_len;
    const MradmEngineMode* qualities;
    size_t qualities_len;
    const MradmAdmText* hrtf_sources;
    size_t hrtf_sources_len;
    uint32_t flags;
} MradmEngineBackend;

typedef struct MradmEngineSpatialLayout {
    MradmAdmText id;
    MradmAdmText display_name;
    uint16_t channel_count;
    uint16_t reserved;
} MradmEngineSpatialLayout;

// Apple backend capabilities, the LFE self-test and system-spatial layouts. Entries may be NULL.
typedef struct MradmPlatformIslands {
    const MradmEngineBackend* (*apple)(void);
    int32_t (*apple_lfe_routing_ok)(void);
    void (*system_spatial)(const MradmEngineSpatialLayout** layouts, size_t* len);
} MradmPlatformIslands;

// ── The offline render service ────────────────────────────────────────────────────────────────

typedef struct MradmEngineProgress {
    uint32_t stage;
    uint32_t operation;
    double fraction;
    double stage_fraction;
    uint64_t current;
    uint64_t total;
    MradmAdmText message;
} MradmEngineProgress;

// Error or metrics returned by the Apple backend.
typedef struct MradmEngineIslandReply {
    int32_t code;
    MradmAdmText message;
    MradmAdmText context;
    uint32_t has_lufs;
    uint32_t has_peak;
    double lufs;
    double peak_dbtp;
} MradmEngineIslandReply;
// The caller's side of an island run inside mradm_engine_render_next: cancelled polls its stop token
// (possibly from another thread), progress receives an event mapped into the render's range, log a
// line the island writes (level: adm::LogLevel).
typedef struct MradmIslandContext {
    void* user;
    uint32_t (*cancelled)(void* user);
    void (*progress)(void* user, const MradmEngineProgress* event);
    void (*log)(void* user, uint32_t level, MradmAdmText module, MradmAdmText message);
} MradmIslandContext;
typedef struct MradmAppleIslands MradmAppleIslands;

// The error buffers of one island call: *_len holds the capacity in and the length out.
typedef struct MradmEngineIslandError {
    uint8_t* message;
    size_t* message_len;
    uint8_t* context;
    size_t* context_len;
} MradmEngineIslandError;

// A C++ stream (the Apple backend). validate and cancel_request may run on any thread, render_output and
// loop_render_reset on the device callback, everything else on the worker. cancel_begin starts a
// fresh stop source for the next seek_cancellable. intermediate_channels is nonzero for an
// output-stage stream. destroy releases the stream.
typedef struct MradmEngineIslandStream {
    void* ctx;
    uint32_t out_channels;
    uint32_t sample_rate;
    uint32_t requires_preroll;
    uint32_t intermediate_channels;
    uint32_t renders_orientation; // renders_orientation_at_output, compared by a backend switch
    int32_t (*process)(void*, float*, size_t, size_t, size_t*, const MradmEngineIslandError*);
    int32_t (*seek)(void*, uint64_t, const MradmEngineIslandError*);
    void (*cancel_begin)(void*);
    void (*cancel_request)(void*);
    int32_t (*seek_cancellable)(void*, uint64_t, const MradmEngineIslandError*);
    int32_t (*validate)(void*, const MradmRenderLiveOverride*, size_t, uint64_t, const MradmEngineIslandError*);
    void (*set_overrides)(void*, const MradmRenderLiveOverride*, size_t, uint64_t);
    void (*set_orientation)(void*, float, float, float);
    int32_t (*produce_intermediate)(void*, float*, size_t, size_t, size_t*, const MradmEngineIslandError*);
    size_t (*render_output)(void*, const float*, size_t, float*, size_t, size_t, float, float, float);
    int32_t (*reposition_source)(void*, uint64_t, const MradmEngineIslandError*);
    void (*loop_render_reset)(void*, uint64_t);
    uint32_t (*render_failed)(void*);
    void (*destroy)(void*);
} MradmEngineIslandStream;

// The RenderPlan fields the Apple backend reads; the values are those of MradmEngineRenderRequest.
typedef struct MradmAppleSettings {
    MradmAdmText input;
    MradmAdmText layout;
    uint32_t routing;
    uint32_t has_matrix;
    MradmDirectMatrixView matrix;
    uint32_t split_power;
    uint32_t speaker_spread;
    uint32_t binaural_spread;
    uint32_t spatial_preset;
    uint32_t speaker_rendering_flags;
    float orientation[3];
} MradmAppleSettings;
typedef struct MradmApplePrepared MradmApplePrepared;
// The Apple backend (src/adm_apple). prepare takes scene (destroyed either way) and returns 0 with
// any failure in reply, *output set on success; render writes output over the window, metrics in
// reply. Reply texts live until the next call on the thread. open_stream returns the stream table
// (holding the preparation) or an error code, 100 for an exception. The context's cancelled and
// progress may be null where unused.
struct MradmAppleIslands {
    int32_t (*prepare)(const MradmAppleSettings* settings,
                       MradmSceneHandle* scene,
                       const MradmIslandContext* context,
                       MradmApplePrepared** output,
                       MradmEngineIslandReply* reply);
    int32_t (*render)(const MradmApplePrepared* prepared,
                      const MradmAppleSettings* settings,
                      MradmAdmText output,
                      uint32_t has_window,
                      uint64_t window_start,
                      uint64_t window_count,
                      const MradmIslandContext* context,
                      MradmEngineIslandReply* reply);
    int32_t (*open_stream)(const MradmApplePrepared* prepared,
                           const MradmAppleSettings* settings,
                           const MradmIslandContext* context,
                           MradmEngineIslandStream* output,
                           const MradmEngineIslandError* error);
    void (*destroy)(MradmApplePrepared* prepared);
};

// A device's pull: fills out (len floats) with up to frames frames and returns how many, on the
// device's audio thread, never blocking or allocating; the device's mark_end may be called inside.
typedef struct MradmDevicePull {
    void* user;
    size_t (*pull)(void* user, float* out, size_t len, size_t frames);
} MradmDevicePull;
// An opened output device (miniaudio from Rust, or a system-spatial sink of
// src/adm_realtime/system_spatial_island.h). start begins pulling (the pull record is copied) and
// returns 0 or an error code (100: the device threw, its message the exception's). An absent entry is
// a no-op (realtime: false, has_volume: false). destroy releases the device (after stop).
typedef struct MradmDeviceTable {
    void* ctx;
    int32_t (*start)(void* ctx,
                     uint32_t channels,
                     uint32_t sample_rate,
                     const MradmDevicePull* pull,
                     const MradmEngineIslandError* error);
    void (*stop)(void* ctx);
    void (*flush)(void* ctx);
    void (*pause)(void* ctx);
    void (*resume)(void* ctx);
    void (*mark_end)(void* ctx);
    uint32_t (*realtime)(void* ctx);
    uint32_t (*has_volume)(void* ctx);
    void (*set_volume)(void* ctx, float gain);
    void (*progress)(void* ctx, MradmEngineSceneDeviceProgress* out);
    void (*destroy)(void* ctx);
} MradmDeviceTable;

// The platform's system-spatial sink (macOS and Windows only): open returns 0 with *output set over
// layout (geometry: SpeakerGeometry) or an error code (100: the factory threw, its message the
// exception's).
typedef struct MradmSystemSpatialIsland {
    int32_t (*open)(MradmAdmText layout,
                    uint32_t geometry,
                    MradmDeviceTable* output,
                    const MradmEngineIslandError* error);
} MradmSystemSpatialIsland;

#ifdef __cplusplus
}
#endif
