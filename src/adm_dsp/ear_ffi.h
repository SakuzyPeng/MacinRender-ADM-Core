#pragma once
#include <stddef.h>
#include <stdint.h>
// Preparation-only private ABI. Valid, aligned pointers and exclusive handles are required.
// Descriptors are copied; all writable buffers must be disjoint from the handle and message.
#ifdef __cplusplus
extern "C" {
#endif
// C-compatible typedefs are intentional at this boundary.
// NOLINTBEGIN(modernize-use-using)
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmEarChannel {
    uint8_t name[32];
    double position[3];
    double nominal[3];
    double azimuth_range[2];
    double elevation_range[2];
    uint32_t lfe;
} MradmEarChannel;
typedef struct MradmEarObject {
    double position[3];
    double width;
    double height;
    double depth;
    double gain;
    double diffuse;
} MradmEarObject;
typedef struct MradmEarLabel {
    const uint8_t* value;
    size_t length;
} MradmEarLabel;
typedef struct MradmEarDirect {
    double position[3];
    double bounds[6];
    double low_pass;
    double high_pass;
    uint32_t present;
    MradmEarLabel pack;
} MradmEarDirect;
int mradm_ear_layout(const uint8_t* name,
                     size_t name_len,
                     MradmEarChannel* channels,
                     size_t capacity,
                     size_t* count,
                     uint8_t* message,
                     size_t message_len);
int mradm_ear_create(const uint8_t* name,
                     size_t name_len,
                     const MradmEarChannel* channels,
                     size_t count,
                     void** output,
                     uint8_t* message,
                     size_t message_len);
void mradm_ear_destroy(void* h);
int mradm_ear_objects(void* h,
                      const MradmEarObject* metadata,
                      double* direct,
                      double* diffuse,
                      size_t count,
                      uint8_t* message,
                      size_t message_len);
int mradm_ear_direct(void* h,
                     const MradmEarDirect* metadata,
                     const MradmEarLabel* labels,
                     size_t label_count,
                     double* gains,
                     size_t count,
                     uint8_t* message,
                     size_t message_len);
int mradm_ear_hoa(void* h,
                  const int32_t* orders,
                  const int32_t* degrees,
                  size_t inputs,
                  uint32_t normalization,
                  double* matrix,
                  size_t length,
                  uint8_t* message,
                  size_t message_len);
int mradm_ear_filters(const void* h, float* filters, size_t length, uint8_t* message, size_t message_len);
// cppcheck-suppress-end unusedStructMember
// NOLINTEND(modernize-use-using)
#ifdef __cplusplus
}
#endif
