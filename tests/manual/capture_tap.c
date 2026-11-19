// The capture or loopback input of the manual room device probe (mr_adm_room_scene_benchmark),
// over the miniaudio implementation of the device islands (src/adm_realtime/miniaudio_device.cpp).
// Manual probe only: the Rust example calls these three functions; miniaudio types stay here.
#include <miniaudio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef void (*mr_probe_capture_fn)(void* user, const float* input, uint32_t frames);

typedef struct mr_probe_capture {
    ma_context context;
    ma_device device;
    ma_device_id id;
    mr_probe_capture_fn callback;
    void* user;
} mr_probe_capture;

// The lowercase hex of the opaque id, as the device islands name output devices.
static void token(const ma_device_id* id, char* out) {
    static const char digits[] = "0123456789abcdef";
    const unsigned char* bytes = (const unsigned char*)id;
    for (size_t i = 0; i < sizeof(*id); ++i) {
        out[i * 2] = digits[bytes[i] >> 4U];
        out[(i * 2) + 1] = digits[bytes[i] & 15U];
    }
    out[sizeof(*id) * 2] = '\0';
}

static void data(ma_device* device, void* output, const void* input, ma_uint32 frames) {
    (void)output;
    if (input != NULL) {
        const mr_probe_capture* self = (const mr_probe_capture*)device->pUserData;
        self->callback(self->user, (const float*)input, frames);
    }
}

// Opens the stereo float capture device named exactly `name`, or with a non-empty
// `loopback_token` the loopback of the output device with that id, at `rate`, and starts it.
// `callback` runs on the capture thread. The selected device's name is copied (NUL-terminated,
// truncated) to `device_name`. 0 on success; 1 context, 2 enumeration, 3 not exactly one match,
// 4 open, 5 start, 6 out of memory.
int mr_probe_capture_open(const char* name,
                          const char* loopback_token,
                          uint32_t rate,
                          mr_probe_capture_fn callback,
                          void* user,
                          mr_probe_capture** out,
                          char* device_name,
                          size_t device_name_len) {
    *out = NULL;
    mr_probe_capture* self = (mr_probe_capture*)calloc(1, sizeof(mr_probe_capture));
    if (self == NULL) {
        return 6;
    }
    self->callback = callback;
    self->user = user;
    if (ma_context_init(NULL, 0, NULL, &self->context) != MA_SUCCESS) {
        free(self);
        return 1;
    }
    const int loopback = loopback_token != NULL && loopback_token[0] != '\0';
    ma_device_info* devices = NULL;
    ma_uint32 count = 0;
    const ma_result listed = loopback ? ma_context_get_devices(&self->context, &devices, &count, NULL, NULL)
                                      : ma_context_get_devices(&self->context, NULL, NULL, &devices, &count);
    if (listed != MA_SUCCESS) {
        ma_context_uninit(&self->context);
        free(self);
        return 2;
    }
    size_t matches = 0;
    char hex[(sizeof(ma_device_id) * 2) + 1];
    for (ma_uint32 i = 0; i < count; ++i) {
        token(&devices[i].id, hex);
        if ((!loopback && strcmp(name, devices[i].name) == 0) || (loopback && strcmp(hex, loopback_token) == 0)) {
            self->id = devices[i].id;
            if (device_name_len != 0) {
                strncpy(device_name, devices[i].name, device_name_len - 1);
                device_name[device_name_len - 1] = '\0';
            }
            ++matches;
        }
    }
    if (matches != 1) {
        ma_context_uninit(&self->context);
        free(self);
        return 3;
    }
    ma_device_config config = ma_device_config_init(loopback ? ma_device_type_loopback : ma_device_type_capture);
    config.capture.pDeviceID = &self->id;
    config.capture.format = ma_format_f32;
    config.capture.channels = 2;
    config.sampleRate = rate;
    config.pUserData = self;
    config.dataCallback = data;
    if (ma_device_init(&self->context, &config, &self->device) != MA_SUCCESS) {
        ma_context_uninit(&self->context);
        free(self);
        return 4;
    }
    if (ma_device_start(&self->device) != MA_SUCCESS) {
        ma_device_uninit(&self->device);
        ma_context_uninit(&self->context);
        free(self);
        return 5;
    }
    *out = self;
    return 0;
}

// Stops the device (its callback has returned when this returns) and frees it; null is ignored.
void mr_probe_capture_close(mr_probe_capture* self) {
    if (self != NULL) {
        ma_device_uninit(&self->device);
        ma_context_uninit(&self->context);
        free(self);
    }
}

// Calls `each` with every capture device's name and whether it is the default; 0 on success.
int mr_probe_capture_inputs(void (*each)(void* user, const char* name, int is_default), void* user) {
    ma_context context;
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
        return 1;
    }
    ma_device_info* devices = NULL;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, NULL, NULL, &devices, &count) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            each(user, devices[i].name, devices[i].isDefault != MA_FALSE);
        }
    }
    ma_context_uninit(&context);
    return 0;
}
