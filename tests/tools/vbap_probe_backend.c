#include "vbap_probe_backend.h"

#include <saf_vbap_internal.h>

void mr_adm_vbap_probe_cross(float a[3], float b[3], float result[3]) {
    ccross(a, b, result);
}

void mr_adm_vbap_probe_rotate(const float rotation[9], const float direction[3], float result[3]) {
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, 3, 1, 3, 1.0F, rotation, 3, direction, 1, 0.0F, result, 1);
}

float mr_adm_vbap_probe_dummy_limit(void) {
    return ADD_DUMMY_LIMIT;
}
