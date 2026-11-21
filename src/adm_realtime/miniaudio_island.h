// miniaudio as the Rust output device drives it (mradm-ffi `miniaudio`, 7.3): the miniaudio calls
// and structures stay in miniaudio_island.c; device ids cross as their raw bytes.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the opened device reports: its internal channel count and the channel counts of its native
// data formats (native_count 0 when ma_device_get_info failed).
typedef struct MradmMiniaudioFormats {
    uint32_t internal_channels;
    uint32_t native_count;
    uint32_t native_channels[64];
} MradmMiniaudioFormats;

// init opens a float32 playback device of channels at sample_rate (null_backend: miniaudio's null
// backend; id: a device id of id_len bytes, used only when id_len is sizeof(ma_device_id), else the
// default device) without starting it; data runs on the device's audio thread for each buffer of
// frames * channels interleaved floats. init returns 0 with *device set, 1 when the null context
// failed, 2 when the device failed, 3 when out of memory. start returns 0 or 1. close stops and frees
// the device (its callback has returned); null is ignored. enumerate calls each with every playback
// device (the texts live during the call) and returns 0, or 1 when the context failed.
typedef struct MradmMiniaudioIsland {
    int32_t (*init)(uint32_t null_backend,
                    const uint8_t* id,
                    size_t id_len,
                    uint32_t channels,
                    uint32_t sample_rate,
                    void (*data)(void* user, float* out, uint32_t frames, uint32_t channels),
                    void* user,
                    void** device);
    void (*formats)(void* device, MradmMiniaudioFormats* out);
    int32_t (*start)(void* device);
    void (*close)(void* device);
    int32_t (*enumerate)(
        void* user, void (*each)(void* user, const uint8_t* id, size_t id_len, const char* name, uint32_t is_default));
} MradmMiniaudioIsland;

const MradmMiniaudioIsland* mradm_miniaudio_island(void);

#ifdef __cplusplus
}
#endif
