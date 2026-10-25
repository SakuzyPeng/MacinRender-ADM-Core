#pragma once
#include <stddef.h>
#include <stdint.h>
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
    uint32_t reflection_update_frames;
} MradmRoomPreparedInfo;
// cppcheck-suppress-end unusedStructMember
int mradm_room_polar(float azimuth, float elevation, float distance, float* out);
int mradm_room_validate_screens(const MradmRoomScreens* screens, size_t count, char* message, size_t capacity);
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
                               double metres,
                               void** out,
                               char* message,
                               size_t capacity);
void mradm_room_prepared_destroy(void* handle);
const char* mradm_room_prepared_json(const void* handle);
int mradm_room_prepared_info(const void* handle, MradmRoomPreparedInfo* out);
#ifdef __cplusplus
}
#endif
