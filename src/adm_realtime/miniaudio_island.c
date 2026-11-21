// The miniaudio implementation (single-header library) and the calls the Rust output device makes
// (miniaudio_island.h). miniaudio types stay in this file (ADR 0003).
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "miniaudio_island.h"

#include <miniaudio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*mradm_miniaudio_data_fn)(void* user, float* out, uint32_t frames, uint32_t channels);

typedef struct Device {
    ma_context context;
    ma_device device;
    ma_device_id id; // referenced by the config during ma_device_init
    int have_context;
    mradm_miniaudio_data_fn data;
    void* user;
} Device;

// miniaudio's ma_device_data_proc fixes the non-const device parameter.
// cppcheck-suppress constParameterCallback
static void data(ma_device* device, void* output, const void* input, ma_uint32 frames) {
    (void) input;
    const Device* self = (const Device*) device->pUserData;
    self->data(self->user, (float*) output, frames, device->playback.channels);
}

static int32_t init(uint32_t null_backend,
                    const uint8_t* id,
                    size_t id_len,
                    uint32_t channels,
                    uint32_t sample_rate,
                    mradm_miniaudio_data_fn callback,
                    void* user,
                    void** out) {
    *out = NULL;
    Device* self = (Device*) calloc(1, sizeof(Device));
    if (self == NULL) {
        return 3;
    }
    self->data = callback;
    self->user = user;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = channels;
    // Expose the native device width instead of letting WASAPI/ALSA hide a stereo fold.
    config.wasapi.noAutoConvertSRC = (ma_bool8) (channels > 2U);
    config.alsa.noAutoChannels = (ma_bool32) (channels > 2U);
    config.sampleRate = sample_rate;
    config.dataCallback = data;
    config.pUserData = self;
    if (id != NULL && id_len == sizeof(ma_device_id)) {
        memcpy(&self->id, id, sizeof(ma_device_id));
        config.playback.pDeviceID = &self->id;
    }
    // The null backend gives a hardware-free, timer-driven device for headless / CI.
    if (null_backend != 0U) {
        const ma_backend backends[1] = {ma_backend_null};
        ma_context_config context_config = ma_context_config_init();
        if (ma_context_init(backends, 1, &context_config, &self->context) != MA_SUCCESS) {
            free(self);
            return 1;
        }
        self->have_context = 1;
    }
    if (ma_device_init(self->have_context ? &self->context : NULL, &config, &self->device) != MA_SUCCESS) {
        if (self->have_context) {
            ma_context_uninit(&self->context);
        }
        free(self);
        return 2;
    }
    *out = self;
    return 0;
}

static void formats(void* device, MradmMiniaudioFormats* out) {
    Device* self = (Device*) device;
    memset(out, 0, sizeof(*out));
    out->internal_channels = self->device.playback.internalChannels;
    ma_device_info info;
    memset(&info, 0, sizeof(info));
    if (ma_device_get_info(&self->device, ma_device_type_playback, &info) == MA_SUCCESS) {
        const uint32_t count = info.nativeDataFormatCount < 64U ? info.nativeDataFormatCount : 64U;
        out->native_count = count;
        for (uint32_t i = 0; i < count; ++i) {
            out->native_channels[i] = info.nativeDataFormats[i].channels;
        }
    }
}

static int32_t start(void* device) {
    return ma_device_start(&((Device*) device)->device) == MA_SUCCESS ? 0 : 1;
}

static void close_device(void* device) {
    Device* self = (Device*) device;
    if (self != NULL) {
        ma_device_uninit(&self->device);
        if (self->have_context) {
            ma_context_uninit(&self->context);
        }
        free(self);
    }
}

static int32_t
enumerate(void* user,
          void (*each)(void* user, const uint8_t* id, size_t id_len, const char* name, uint32_t is_default)) {
    ma_context context;
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
        return 1;
    }
    ma_device_info* playback = NULL;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, &playback, &count, NULL, NULL) == MA_SUCCESS && playback != NULL) {
        for (ma_uint32 i = 0; i < count; ++i) {
            each(user,
                 (const uint8_t*) &playback[i].id,
                 sizeof(ma_device_id),
                 playback[i].name,
                 playback[i].isDefault != MA_FALSE ? 1U : 0U);
        }
    }
    ma_context_uninit(&context);
    return 0;
}

static const MradmMiniaudioIsland k_island = {init, formats, start, close_device, enumerate};

const MradmMiniaudioIsland* mradm_miniaudio_island(void) {
    return &k_island;
}
