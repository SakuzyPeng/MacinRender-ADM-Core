#pragma once

// Keep SAF's external BLAS/LAPACK C headers out of the C++ probe: the prebuilt
// Windows OpenBLAS package uses MSVC C complex types in those headers.
#ifdef __cplusplus
extern "C" {
#endif

void mr_adm_vbap_probe_cross(float a[3], float b[3], float result[3]);
void mr_adm_vbap_probe_rotate(const float rotation[9], const float direction[3], float result[3]);
float mr_adm_vbap_probe_dummy_limit(void);

#ifdef __cplusplus
}
#endif
