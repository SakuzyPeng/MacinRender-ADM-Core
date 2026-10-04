/*
 * Derived from SAF examples spreader.h (copyright 2021 Leo McCormack, ISC license).
 * Extensions: spreader_init_from_hrtf_grid() for injecting pre-loaded HRTF data.
 */

#ifndef MRADM_SPREADER_MR_H
#define MRADM_SPREADER_MR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "_common.h" /* CODEC_STATUS, PROC_STATUS, _Atomic_* types */

#define SPREADER_MAX_NUM_SOURCES (8)

typedef enum { SPREADER_MODE_NAIVE = 1, SPREADER_MODE_OM, SPREADER_MODE_EVD } SPREADER_PROC_MODES;

void spreader_create(void** const phSpr);
void spreader_destroy(void** const phSpr);
void spreader_init(void* const hSpr, int samplerate);
void spreader_initCodec(void* const hSpr);
void spreader_process(
    void* const hSpr, const float* const* inputs, float* const* outputs, int nInputs, int nOutputs, int nSamples);

void spreader_refreshSettings(void* const hSpr);
void spreader_setSpreadingMode(void* const hSpr, int newMode);
void spreader_setAveragingCoeff(void* const hSpr, float newValue);
void spreader_setSourceAzi_deg(void* const hSpr, int index, float newAzi_deg);
void spreader_setSourceElev_deg(void* const hSpr, int index, float newElev_deg);
void spreader_setSourceSpread_deg(void* const hSpr, int index, float newSpread_deg);
void spreader_setNumSources(void* const hSpr, int new_nSources);
void spreader_setUseDefaultHRIRsflag(void* const hSpr, int newState);
void spreader_setSofaFilePath(void* const hSpr, const char* path);

int spreader_getFrameSize(void);
CODEC_STATUS spreader_getCodecStatus(void* const hSpr);
float spreader_getProgressBar0_1(void* const hSpr);
void spreader_getProgressBarText(void* const hSpr, char* text);
int* spreader_getDirectionActivePtr(void* const hSpr, int index);
int spreader_getSpreadingMode(void* const hSpr);
float spreader_getAveragingCoeff(void* const hSpr);
float spreader_getSourceAzi_deg(void* const hSpr, int index);
float spreader_getSourceElev_deg(void* const hSpr, int index);
float spreader_getSourceSpread_deg(void* const hSpr, int index);
int spreader_getNumSources(void* const hSpr);
int spreader_getMaxNumSources(void);
int spreader_getNumOutputs(void* const hSpr);
int spreader_getNDirs(void* const hSpr);
float spreader_getIRAzi_deg(void* const hSpr, int index);
float spreader_getIRElev_deg(void* const hSpr, int index);
int spreader_getIRlength(void* const hSpr);
int spreader_getIRsamplerate(void* const hSpr);
int spreader_getUseDefaultHRIRsflag(void* const hSpr);
char* spreader_getSofaFilePath(void* const hSpr);
int spreader_getDAWsamplerate(void* const hSpr);
int spreader_getProcessingDelay(void);

/**
 * Initialise the spreader HRTF grid from a pre-loaded dataset, bypassing the
 * default-HRIR and SOFA loading branches in spreader_initCodec().
 *
 * Call order: spreader_create() → spreader_init() →
 *             spreader_init_from_hrtf_grid() → spreader_initCodec()
 *
 * @param h_grid_flat        FLAT: nGrid × Q × h_len (float, row-major)
 * @param grid_dirs_deg_flat FLAT: nGrid × 2, az/el in degrees (SAF 0..360 / −90..90 convention)
 * @param nGrid              Number of HRIR measurement directions
 * @param Q                  Number of output channels (2 for binaural)
 * @param h_len              Length of each HRIR in samples
 * @param sample_rate        HRIR measurement sample rate
 */
void spreader_init_from_hrtf_grid(void* const hSpr,
                                  const float* h_grid_flat,
                                  const float* grid_dirs_deg_flat,
                                  int nGrid,
                                  int Q,
                                  int h_len,
                                  int sample_rate);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MRADM_SPREADER_MR_H */
