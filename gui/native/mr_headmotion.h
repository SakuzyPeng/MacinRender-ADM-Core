#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mr_headmotion_t mr_headmotion_t;
/* poll and destroy belong to one control thread; no sensor callbacks enter it.
 * Raw CoreMotion attitude is w,x,y,z. State: 0 unavailable, 1 waiting, 2 active,
 * 3 denied/restricted, 4 disconnected/stale, 5 missing usage description. */
typedef struct mr_headmotion_sample_t {
    uint32_t struct_size;
    int32_t state;
    uint64_t sequence;
    double timestamp;
    double w;
    double x;
    double y;
    double z;
} mr_headmotion_sample_t;

mr_headmotion_t* mr_headmotion_create(void);
int mr_headmotion_poll(mr_headmotion_t* handle, mr_headmotion_sample_t* out);
void mr_headmotion_destroy(mr_headmotion_t* handle);

#ifdef __cplusplus
}
#endif
