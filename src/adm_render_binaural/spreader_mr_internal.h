/*
 * Derived from SAF examples spreader_internal.h (copyright 2021 Leo McCormack, ISC license).
 * Additions:
 *   - int useExternalHRIRsFLAG field in spreader_data
 */

#ifndef MRADM_SPREADER_MR_INTERNAL_H
#define MRADM_SPREADER_MR_INTERNAL_H

#include "saf.h"           /* main SAF header */
#include "saf_externals.h" /* cblas, lapacke, etc. */
#include "spreader_mr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Internal parameters ─────────────────────────────────────────────── */

#if !defined(SPREADER_FRAME_SIZE)
#if defined(FRAME_SIZE)
#define SPREADER_FRAME_SIZE (FRAME_SIZE)
#else
#define SPREADER_FRAME_SIZE (512)
#endif
#endif
#define MAX_SPREAD_FREQ (16e3f)
#define HOP_SIZE (128)
#define HYBRID_BANDS (HOP_SIZE + 5)
#define TIME_SLOTS (SPREADER_FRAME_SIZE / HOP_SIZE)

#if (SPREADER_FRAME_SIZE % HOP_SIZE != 0)
#error "SPREADER_FRAME_SIZE must be an integer multiple of HOP_SIZE"
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
typedef _Atomic SPREADER_PROC_MODES _Atomic_SPREADER_PROC_MODES;
#else
typedef SPREADER_PROC_MODES _Atomic_SPREADER_PROC_MODES;
#endif

/* ── spreader_data struct ────────────────────────────────────────────── */

typedef struct _spreader {
    /* audio buffers and time-frequency transform */
    float** inputFrameTD;
    float** outframeTD;
    float_complex*** inputframeTF;
    float_complex*** protoframeTF;
    float_complex*** decorframeTF;
    float_complex*** spreadframeTF;
    float_complex*** outputframeTF;
    int fs;
    float freqVector[HYBRID_BANDS];
    void* hSTFT;

    /* internal */
    _Atomic_INT32 Q;
    _Atomic_INT32 nGrid;
    _Atomic_INT32 h_len;
    _Atomic_FLOAT32 h_fs;
    float* h_grid;                     /* FLAT: nGrid x Q x h_len */
    float_complex* H_grid;             /* FLAT: HYBRID_BANDS x Q x nGrid */
    float_complex** HHH[HYBRID_BANDS]; /* HYBRID_BANDS x nGrid x FLAT:(Q x Q) */
    float* grid_dirs_deg;              /* FLAT: nGrid x 2 */
    float* grid_dirs_xyz;              /* FLAT: nGrid x 3 */
    float* weights;
    void* hDecor[SPREADER_MAX_NUM_SOURCES];
    float* angles;
    float_complex** Cproto[SPREADER_MAX_NUM_SOURCES];
    float_complex** Cy[SPREADER_MAX_NUM_SOURCES];
    float_complex** prev_M[SPREADER_MAX_NUM_SOURCES];
    float** prev_Mr[SPREADER_MAX_NUM_SOURCES];
    float_complex** new_M;
    float** new_Mr;
    float_complex* interp_M;
    float* interp_Mr;
    float_complex* interp_Mr_cmplx;
    float interpolatorFadeIn[TIME_SLOTS];
    float interpolatorFadeOut[TIME_SLOTS];

    /* for visualisation */
    int* dirActive[SPREADER_MAX_NUM_SOURCES];

    /* optimal mixing solution */
    void* hCdf;
    void* hCdf_res;
    float* Qmix;
    float_complex* Qmix_cmplx;
    float* Cr;
    float_complex* Cr_cmplx;

    /* hotfix temporaries */
    float_complex* _tmpFrame;
    float_complex* _H_tmp;
    float_complex* _Cy;
    float_complex* _E_dir;
    float_complex* _V;
    float_complex* _D;
    float_complex* _Cproto;

    /* flags/status */
    _Atomic_CODEC_STATUS codecStatus;
    _Atomic_FLOAT32 progressBar0_1;
    char* progressBarText;
    _Atomic_PROC_STATUS procStatus;
    _Atomic_INT32 new_nSources;
    _Atomic_SPREADER_PROC_MODES new_procMode;

    /* user parameters */
    _Atomic_SPREADER_PROC_MODES procMode;
    char* sofa_filepath;
    _Atomic_INT32 nSources;
    _Atomic_FLOAT32 src_spread[SPREADER_MAX_NUM_SOURCES];
    _Atomic_FLOAT32 src_dirs_deg[SPREADER_MAX_NUM_SOURCES][2];
    _Atomic_INT32 useDefaultHRIRsFLAG;
    _Atomic_FLOAT32 covAvgCoeff;

    /* extension: if 1, h_grid/grid_dirs_deg/Q/nGrid/h_len/h_fs are pre-populated
     * by spreader_init_from_hrtf_grid(); skip HRTF loading in initCodec */
    int useExternalHRIRsFLAG;

} spreader_data;

/* ── internal function ───────────────────────────────────────────────── */

void spreader_setCodecStatus(void* const hSpr, CODEC_STATUS newStatus);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MRADM_SPREADER_MR_INTERNAL_H */
