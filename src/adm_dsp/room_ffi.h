#pragma once
#include "live_binaural_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmRoomScreenGeometry {
    uint32_t cartesian;
    uint32_t reserved;
    double aspect_ratio;
    double centre[3];
    double width;
} MradmRoomScreenGeometry;
typedef struct MradmRoomScreens {
    uint32_t enabled;
    uint32_t reserved;
    MradmRoomScreenGeometry reference;
    MradmRoomScreenGeometry reproduction;
} MradmRoomScreens;
typedef struct MradmRoomDescription {
    uint64_t id;
    uint64_t generation;
    uint32_t role;
    uint32_t reserved;
    float position[3];
    uint32_t padding;
    MradmRoomScreens screens;
} MradmRoomDescription;
typedef struct MradmRoomOverride {
    float gain_db;
    uint32_t mute;
    float extent[3];
    float diffuse;
    float divergence;
    uint32_t has_head;
    uint32_t head;
    uint32_t reserved;
} MradmRoomOverride;
typedef struct MradmRoomCommand {
    uint32_t element;
    uint32_t offset;
    uint32_t duration;
    uint32_t reserved;
    uint64_t changed;
    uint64_t cleared;
    MradmLiveBinauralState state;
} MradmRoomCommand;
typedef struct MradmRoomPlane {
    const float* samples;
    size_t length;
    uint32_t has_signal;
    uint32_t reserved;
} MradmRoomPlane;
typedef struct MradmRoomSourceInfo {
    uint64_t id;
    double position[3];
    double direction[3];
    double distance;
    uint32_t constrained;
    uint32_t present;
} MradmRoomSourceInfo;
typedef struct MradmRoomPreparedInfo {
    uint64_t latency_frames;
    uint64_t tail_frames;
    uint64_t preroll_frames;
    uint64_t material_guard_frames;
    uint32_t sample_rate;
    uint32_t reserved;
} MradmRoomPreparedInfo;
typedef struct MradmRoomInfo {
    uint64_t input_frames;
    uint64_t latency_frames;
    uint64_t tail_frames;
    uint64_t material_guard_frames;
    uint32_t source_count;
    uint32_t source_capacity;
    uint32_t constrained_sources;
    uint32_t phase;
    uint64_t applied_request;
    uint64_t transition_sample;
    uint64_t pending_request;
} MradmRoomInfo;
// cppcheck-suppress-end unusedStructMember
int mradm_room_polar(float azimuth, float elevation, float distance, float* out);
int mradm_room_validate_screens(const MradmRoomScreens* screens, size_t count, char* message, size_t capacity);
int mradm_room_validate_overrides(const MradmRoomOverride* values, size_t count, char* message, size_t capacity);
int mradm_room_session_overrides(
    void* handle, const MradmRoomOverride* values, size_t count, char* message, size_t capacity);
int mradm_room_profile_load(const char* path, size_t length, void** out, char* message, size_t capacity);
void mradm_room_profile_destroy(void* handle);
const char* mradm_room_profile_json(const void* handle);
int mradm_room_prepare(const void* profile,
                       const char* hrir,
                       size_t hrir_length,
                       const char* sofa,
                       size_t sofa_length,
                       const void* reuse,
                       void** out,
                       char* message,
                       size_t capacity);
int mradm_room_prepare_quality(const void* profile,
                               const char* hrir,
                               size_t hrir_length,
                               const char* sofa,
                               size_t sofa_length,
                               const void* reuse,
                               uint32_t quality,
                               void** out,
                               char* message,
                               size_t capacity);
void mradm_room_prepared_destroy(void* handle);
const char* mradm_room_prepared_json(const void* handle);
int mradm_room_prepared_info(const void* handle, MradmRoomPreparedInfo* out);
int mradm_room_session_create(
    const void* prepared, double metres, uint32_t scene, uint32_t spread, void** out, char* message, size_t capacity);
void mradm_room_session_destroy(void* handle);
int mradm_room_session_configure(
    void* handle, const MradmRoomDescription* descriptions, size_t count, char* message, size_t capacity);
int mradm_room_session_reset(void* handle, uint64_t sample, uint32_t clear, char* message, size_t capacity);
int mradm_room_session_prime(
    void* handle, const MradmRoomCommand* command, uint64_t sample, char* message, size_t capacity);
int mradm_room_session_pose(void* handle, const float* pose, size_t length);
int mradm_room_session_end(void* handle);
int mradm_room_session_info(const void* handle, MradmRoomInfo* out);
int mradm_room_session_source(const void* handle, uint64_t id, MradmRoomSourceInfo* out);
int mradm_room_session_process(void* handle,
                               size_t frames,
                               const MradmRoomPlane* planes,
                               size_t count,
                               const MradmRoomCommand* initial,
                               size_t initial_count,
                               const MradmRoomCommand* events,
                               size_t event_count,
                               float* pcm,
                               size_t pcm_length,
                               MradmRoomInfo* info,
                               char* message,
                               size_t capacity);
int mradm_room_instance_create(const void* prepared, size_t sources, void** out, char* message, size_t capacity);
void mradm_room_instance_destroy(void* handle);
// Preparation-thread only; grow a candidate before handing it to the session.
int mradm_room_instance_reserve(void* handle, size_t sources, char* message, size_t capacity);
// Worker notification, independent of whether the latest preparation succeeds.
int mradm_room_session_request(void* handle, uint64_t request);
int mradm_room_session_queue(
    void* handle, void* next, uint64_t request, void** displaced, size_t count, char* message, size_t capacity);
int mradm_room_session_retired(void* handle, void** out);
#ifdef __cplusplus
}
#endif
