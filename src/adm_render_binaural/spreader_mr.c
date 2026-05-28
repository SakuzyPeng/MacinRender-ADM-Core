/*
 * Derived from SAF examples spreader.c (copyright 2021 Leo McCormack, ISC license).
 * Modifications:
 *   - Added spreader_init_from_hrtf_grid(): inject pre-loaded HRTF grid data.
 *   - In spreader_initCodec(): skip HRTF loading when useExternalHRIRsFLAG is set.
 *   - Fixed SOFA branch: set pData->Q = sofa.nReceivers before using pData->Q.
 */

#include "spreader_mr_internal.h"

void spreader_create(void** const phSpr) {
    spreader_data* pData = (spreader_data*) malloc1d(sizeof(spreader_data));
    *phSpr = (void*) pData;
    int band, t, src;

    /* user parameters */
    pData->sofa_filepath = NULL;
    pData->nSources = 1;
    pData->procMode = SPREADER_MODE_OM;
    pData->useDefaultHRIRsFLAG = 1;
    pData->covAvgCoeff = 0.85f;
    pData->useExternalHRIRsFLAG = 0; /* extension: disabled by default */
    memset(pData->src_spread, 0, SPREADER_MAX_NUM_SOURCES * sizeof(float));
    memset(pData->src_dirs_deg, 0, SPREADER_MAX_NUM_SOURCES * 2 * sizeof(float));

    /* cone cache */
    pData->cone_ng_flat = NULL;
    memset(pData->n_cone, 0, sizeof(pData->n_cone));
    memset(pData->centre_ind_cache, 0, sizeof(pData->centre_ind_cache));
    memset(pData->cone_cached_az, 0, sizeof(pData->cone_cached_az));
    memset(pData->cone_cached_el, 0, sizeof(pData->cone_cached_el));
    memset(pData->cone_cached_spread, 0, sizeof(pData->cone_cached_spread));
    memset(pData->cone_valid, 0, sizeof(pData->cone_valid));

    /* time-frequency transform + buffers */
    pData->fs = 48000.0f;
    pData->hSTFT = NULL;
    pData->inputFrameTD = (float**) malloc2d(MAX_NUM_INPUTS, SPREADER_FRAME_SIZE, sizeof(float));
    pData->outframeTD = (float**) malloc2d(MAX_NUM_OUTPUTS, SPREADER_FRAME_SIZE, sizeof(float));
    pData->inputframeTF = (float_complex***) malloc3d(HYBRID_BANDS, MAX_NUM_INPUTS, TIME_SLOTS, sizeof(float_complex));
    pData->protoframeTF = (float_complex***) malloc3d(HYBRID_BANDS, MAX_NUM_OUTPUTS, TIME_SLOTS, sizeof(float_complex));
    pData->decorframeTF = (float_complex***) malloc3d(HYBRID_BANDS, MAX_NUM_OUTPUTS, TIME_SLOTS, sizeof(float_complex));
    pData->spreadframeTF =
        (float_complex***) malloc3d(HYBRID_BANDS, MAX_NUM_OUTPUTS, TIME_SLOTS, sizeof(float_complex));
    pData->outputframeTF =
        (float_complex***) malloc3d(HYBRID_BANDS, MAX_NUM_OUTPUTS, TIME_SLOTS, sizeof(float_complex));

    /* internal */
    pData->Q = pData->nGrid = pData->h_len = 0;
    pData->h_fs = 0.0f;
    pData->h_grid = NULL;
    pData->H_grid = NULL;
    for (band = 0; band < HYBRID_BANDS; band++)
        pData->HHH[band] = NULL;
    pData->grid_dirs_deg = NULL;
    pData->grid_dirs_xyz = NULL;
    pData->weights = NULL;
    pData->angles = NULL;
    for (src = 0; src < SPREADER_MAX_NUM_SOURCES; src++) {
        pData->hDecor[src] = NULL;
        pData->Cy[src] = NULL;
        pData->Cproto[src] = NULL;
        pData->prev_M[src] = NULL;
        pData->prev_Mr[src] = NULL;
        pData->dirActive[src] = NULL;
    }
    pData->new_M = NULL;
    pData->new_Mr = NULL;
    pData->interp_M = NULL;
    pData->interp_Mr = NULL;
    pData->interp_Mr_cmplx = NULL;
    for (t = 0; t < TIME_SLOTS; t++) {
        pData->interpolatorFadeIn[t] = ((float) t + 1.0f) / (float) TIME_SLOTS;
        pData->interpolatorFadeOut[t] = 1.0f - ((float) t + 1.0f) / (float) TIME_SLOTS;
    }

    /* hotfix temporaries */
    pData->_tmpFrame = malloc1d(MAX_NUM_CHANNELS * TIME_SLOTS * sizeof(float_complex));
    pData->_H_tmp = malloc1d(MAX_NUM_CHANNELS * sizeof(float_complex));
    pData->_Cy = malloc1d(MAX_NUM_CHANNELS * MAX_NUM_CHANNELS * sizeof(float_complex));
    pData->_E_dir = malloc1d(MAX_NUM_CHANNELS * MAX_NUM_CHANNELS * sizeof(float_complex));
    pData->_V = malloc1d(MAX_NUM_OUTPUTS * MAX_NUM_OUTPUTS * sizeof(float_complex));
    pData->_D = malloc1d(MAX_NUM_OUTPUTS * MAX_NUM_OUTPUTS * sizeof(float_complex));
    pData->_Cproto = malloc1d(MAX_NUM_OUTPUTS * MAX_NUM_OUTPUTS * sizeof(float_complex));

    /* optimal mixing */
    pData->hCdf = NULL;
    pData->hCdf_res = NULL;
    pData->Qmix = NULL;
    pData->Qmix_cmplx = NULL;
    pData->Cr = NULL;
    pData->Cr_cmplx = NULL;

    /* flags/status */
    pData->new_procMode = pData->procMode;
    pData->new_nSources = pData->nSources;
    pData->progressBar0_1 = 0.0f;
    pData->progressBarText = malloc1d(PROGRESSBARTEXT_CHAR_LENGTH * sizeof(char));
    strcpy(pData->progressBarText, "");
    pData->codecStatus = CODEC_STATUS_NOT_INITIALISED;
    pData->procStatus = PROC_STATUS_NOT_ONGOING;
}

void spreader_destroy(void** const phSpr) {
    spreader_data* pData = (spreader_data*) (*phSpr);
    int band, src;

    if (pData != NULL) {
        while (pData->codecStatus == CODEC_STATUS_INITIALISING || pData->procStatus == PROC_STATUS_ONGOING) {
            SAF_SLEEP(10);
        }

        free(pData->sofa_filepath);

        if (pData->hSTFT != NULL)
            afSTFT_destroy(&(pData->hSTFT));
        free(pData->inputFrameTD);
        free(pData->outframeTD);
        free(pData->inputframeTF);
        free(pData->decorframeTF);
        free(pData->spreadframeTF);
        free(pData->outputframeTF);

        free(pData->h_grid);
        free(pData->H_grid);
        for (band = 0; band < HYBRID_BANDS; band++)
            free(pData->HHH[band]);
        free(pData->grid_dirs_deg);
        free(pData->grid_dirs_xyz);
        free(pData->weights);
        free(pData->angles);
        for (src = 0; src < SPREADER_MAX_NUM_SOURCES; src++) {
            latticeDecorrelator_destroy(&(pData->hDecor[src]));
            free(pData->Cy[src]);
            free(pData->Cproto[src]);
            free(pData->prev_M[src]);
            free(pData->prev_Mr[src]);
            free(pData->dirActive[src]);
        }
        free(pData->new_M);
        free(pData->new_Mr);
        free(pData->interp_M);
        free(pData->interp_Mr);
        free(pData->interp_Mr_cmplx);

        free(pData->_tmpFrame);
        free(pData->_H_tmp);
        free(pData->_Cy);
        free(pData->_E_dir);
        free(pData->_V);
        free(pData->_D);
        free(pData->_Cproto);

        cdf4sap_cmplx_destroy(&(pData->hCdf));
        cdf4sap_destroy(&(pData->hCdf_res));
        free(pData->Qmix);
        free(pData->Qmix_cmplx);
        free(pData->Cr);
        free(pData->Cr_cmplx);

        free(pData->progressBarText);
        free(pData->cone_ng_flat);

        free(pData);
        pData = NULL;
        *phSpr = NULL;
    }
}

void spreader_init(void* const hSpr, int sampleRate) {
    spreader_data* pData = (spreader_data*) (hSpr);
    pData->fs = sampleRate;
    afSTFT_getCentreFreqs(pData->hSTFT, (float) sampleRate, HYBRID_BANDS, pData->freqVector);
}

void spreader_initCodec(void* const hSpr) {
    spreader_data* pData = (spreader_data*) (hSpr);
    int q, band, ng, nSources, src;
    float_complex scaleC;
#ifdef SAF_ENABLE_SOFA_READER_MODULE
    saf_sofa_container sofa;
    SAF_SOFA_ERROR_CODES error;
#endif
    SPREADER_PROC_MODES procMode;
    float_complex H_tmp[MAX_NUM_CHANNELS];
    const float_complex calpha = cmplxf(1.0f, 0.0f), cbeta = cmplxf(0.0f, 0.0f);

    if (pData->codecStatus != CODEC_STATUS_NOT_INITIALISED)
        return;
    while (pData->procStatus == PROC_STATUS_ONGOING) {
        pData->codecStatus = CODEC_STATUS_INITIALISING;
        SAF_SLEEP(10);
    }

    nSources = pData->new_nSources;
    procMode = pData->new_procMode;

    pData->codecStatus = CODEC_STATUS_INITIALISING;
    strcpy(pData->progressBarText, "Initialising");
    pData->progressBar0_1 = 0.0f;

    /* Load HRTF measurements, unless the caller has pre-populated the data */
    if (!pData->useExternalHRIRsFLAG) {
#ifndef SAF_ENABLE_SOFA_READER_MODULE
        pData->useDefaultHRIRsFLAG = 1;
#endif
        if (pData->useDefaultHRIRsFLAG) {
            pData->Q = NUM_EARS;
            pData->nGrid = __default_N_hrir_dirs;
            pData->h_len = __default_hrir_len;
            pData->h_fs = (float) __default_hrir_fs;
            pData->h_grid = realloc1d(pData->h_grid, pData->nGrid * (pData->Q) * (pData->h_len) * sizeof(float));
            memcpy(pData->h_grid, (float*) __default_hrirs, pData->nGrid * (pData->Q) * (pData->h_len) * sizeof(float));
            pData->grid_dirs_deg = realloc1d(pData->grid_dirs_deg, pData->nGrid * 2 * sizeof(float));
            memcpy(pData->grid_dirs_deg, (float*) __default_hrir_dirs_deg, pData->nGrid * 2 * sizeof(float));
        }
#ifdef SAF_ENABLE_SOFA_READER_MODULE
        else {
            error = saf_sofa_open(&sofa, pData->sofa_filepath, SAF_SOFA_READER_OPTION_DEFAULT);
            if (error != SAF_SOFA_OK) {
                pData->useDefaultHRIRsFLAG = 1;
                saf_print_warning("Unable to load the specified SOFA file. Using default HRIR data instead");
                spreader_initCodec(hSpr);
                return;
            }
            pData->Q = sofa.nReceivers; /* fix: assign Q from SOFA nReceivers */
            pData->h_fs = sofa.DataSamplingRate;
            pData->h_len = sofa.DataLengthIR;
            pData->nGrid = sofa.nSources;
            pData->h_grid = realloc1d(pData->h_grid, pData->nGrid * (pData->Q) * (pData->h_len) * sizeof(float));
            memcpy(pData->h_grid, sofa.DataIR, pData->nGrid * (pData->Q) * (pData->h_len) * sizeof(float));
            pData->grid_dirs_deg = realloc1d(pData->grid_dirs_deg, pData->nGrid * 2 * sizeof(float));
            cblas_scopy(pData->nGrid, sofa.SourcePosition, 3, pData->grid_dirs_deg, 2);
            cblas_scopy(pData->nGrid, &sofa.SourcePosition[1], 3, &pData->grid_dirs_deg[1], 2);
            saf_sofa_close(&sofa);
        }
#endif
        /* Convert az convention 0..360 → −180..180 and compute Cartesian vectors */
        convert_0_360To_m180_180(pData->grid_dirs_deg, pData->nGrid);
    } else {
        /* External data: dirs may already be in 0..360; convert the same way */
        convert_0_360To_m180_180(pData->grid_dirs_deg, pData->nGrid);
    }

    pData->grid_dirs_xyz = realloc1d(pData->grid_dirs_xyz, pData->nGrid * 3 * sizeof(float));
    unitSph2cart(pData->grid_dirs_deg, pData->nGrid, 1, pData->grid_dirs_xyz);

    /* afSTFT and decorrelators */
    afSTFT_destroy(&(pData->hSTFT));
    afSTFT_create(&(pData->hSTFT), nSources, pData->Q, HOP_SIZE, 0, 1, AFSTFT_BANDS_CH_TIME);
    {
        int orders[4] = {20, 15, 6, 6};
        float freqCutoffs[4] = {900.0f, 6.8e3f, 12e3f, 24e3f};
        const int maxDelay = 12;
        for (src = 0; src < SPREADER_MAX_NUM_SOURCES; src++) {
            latticeDecorrelator_destroy(&(pData->hDecor[src]));
            latticeDecorrelator_create(&(pData->hDecor[src]),
                                       (float) pData->fs,
                                       HOP_SIZE,
                                       pData->freqVector,
                                       HYBRID_BANDS,
                                       pData->Q,
                                       orders,
                                       freqCutoffs,
                                       4,
                                       maxDelay,
                                       0,
                                       0.75f);
        }
    }

    /* Filterbank coefficients and outer products */
    pData->H_grid = realloc1d(pData->H_grid, HYBRID_BANDS * (pData->Q) * pData->nGrid * sizeof(float_complex));
    afSTFT_FIRtoFilterbankCoeffs(pData->h_grid, pData->nGrid, pData->Q, pData->h_len, HOP_SIZE, 0, 1, pData->H_grid);
    pData->weights = realloc1d(pData->weights, pData->nGrid * sizeof(float));
    getVoronoiWeights(pData->grid_dirs_deg, pData->nGrid, 0, pData->weights);
    cblas_sscal(pData->nGrid, 1.0f / FOURPI, pData->weights, 1);
    for (band = 0; band < HYBRID_BANDS; band++) {
        pData->HHH[band] = (float_complex**) realloc2d(
            (void**) pData->HHH[band], pData->nGrid, pData->Q * (pData->Q), sizeof(float_complex));
        for (ng = 0; ng < pData->nGrid; ng++) {
            for (q = 0; q < pData->Q; q++)
                H_tmp[q] = pData->H_grid[band * (pData->Q) * pData->nGrid + q * pData->nGrid + ng];
            cblas_cgemm(CblasRowMajor,
                        CblasNoTrans,
                        CblasConjTrans,
                        pData->Q,
                        pData->Q,
                        1,
                        &calpha,
                        H_tmp,
                        1,
                        H_tmp,
                        1,
                        &cbeta,
                        pData->HHH[band][ng],
                        pData->Q);
            scaleC = cmplxf(pData->weights[ng], 0.0f);
            cblas_cscal(pData->Q * (pData->Q), &scaleC, pData->HHH[band][ng], 1);
        }
    }
    pData->angles = realloc1d(pData->angles, pData->nGrid * sizeof(float));

    /* CDF4SAP / optimal mixing structures */
    cdf4sap_cmplx_destroy(&(pData->hCdf));
    cdf4sap_cmplx_create(&(pData->hCdf), pData->Q, pData->Q);
    cdf4sap_destroy(&(pData->hCdf_res));
    cdf4sap_create(&(pData->hCdf_res), pData->Q, pData->Q);
    pData->Qmix = realloc1d(pData->Qmix, pData->Q * (pData->Q) * sizeof(float));
    memset(pData->Qmix, 0, pData->Q * (pData->Q) * sizeof(float));
    pData->Qmix_cmplx = realloc1d(pData->Qmix_cmplx, pData->Q * (pData->Q) * sizeof(float_complex));
    memset(pData->Qmix_cmplx, 0, pData->Q * (pData->Q) * sizeof(float_complex));
    for (q = 0; q < pData->Q; q++) {
        pData->Qmix[q * (pData->Q) + q] = 1.0f;
        pData->Qmix_cmplx[q * (pData->Q) + q] = cmplxf(1.0f, 0.0f);
    }
    pData->Cr = realloc1d(pData->Cr, pData->Q * (pData->Q) * sizeof(float));
    pData->Cr_cmplx = realloc1d(pData->Cr_cmplx, pData->Q * (pData->Q) * sizeof(float_complex));

    /* mixing matrices and per-source covariance buffers */
    for (src = 0; src < SPREADER_MAX_NUM_SOURCES; src++) {
        pData->Cy[src] = (float_complex**) realloc2d(
            (void**) pData->Cy[src], HYBRID_BANDS, (pData->Q) * (pData->Q), sizeof(float_complex));
        memset(FLATTEN2D(pData->Cy[src]), 0, HYBRID_BANDS * (pData->Q) * (pData->Q) * sizeof(float_complex));
        pData->Cproto[src] = (float_complex**) realloc2d(
            (void**) pData->Cproto[src], HYBRID_BANDS, (pData->Q) * (pData->Q), sizeof(float_complex));
        memset(FLATTEN2D(pData->Cproto[src]), 0, HYBRID_BANDS * (pData->Q) * (pData->Q) * sizeof(float_complex));
        pData->prev_M[src] = (float_complex**) realloc2d(
            (void**) pData->prev_M[src], HYBRID_BANDS, (pData->Q) * (pData->Q), sizeof(float_complex));
        memset(FLATTEN2D(pData->prev_M[src]), 0, HYBRID_BANDS * (pData->Q) * (pData->Q) * sizeof(float_complex));
        pData->prev_Mr[src] =
            (float**) realloc2d((void**) pData->prev_Mr[src], HYBRID_BANDS, (pData->Q) * (pData->Q), sizeof(float));
        memset(FLATTEN2D(pData->prev_Mr[src]), 0, HYBRID_BANDS * (pData->Q) * (pData->Q) * sizeof(float));
        pData->dirActive[src] = realloc1d(pData->dirActive[src], pData->nGrid * sizeof(int));
        memset(pData->dirActive[src], 0, pData->nGrid * sizeof(int));
    }
    pData->new_M = (float_complex**) realloc2d(
        (void**) pData->new_M, HYBRID_BANDS, (pData->Q) * (pData->Q), sizeof(float_complex));
    pData->new_Mr = (float**) realloc2d((void**) pData->new_Mr, HYBRID_BANDS, (pData->Q) * (pData->Q), sizeof(float));
    pData->interp_M = realloc1d(pData->interp_M, (pData->Q) * (pData->Q) * sizeof(float_complex));
    pData->interp_Mr = realloc1d(pData->interp_Mr, (pData->Q) * (pData->Q) * sizeof(float));
    pData->interp_Mr_cmplx = realloc1d(pData->interp_Mr_cmplx, (pData->Q) * (pData->Q) * sizeof(float_complex));
    memset(pData->interp_Mr_cmplx, 0, (pData->Q) * (pData->Q) * sizeof(float_complex));

    pData->nSources = nSources;
    pData->procMode = procMode;

    strcpy(pData->progressBarText, "Done!");
    pData->progressBar0_1 = 1.0f;
    pData->codecStatus = CODEC_STATUS_INITIALISED;
}

void spreader_process(
    void* const hSpr, const float* const* inputs, float* const* outputs, int nInputs, int nOutputs, int nSamples) {
    spreader_data* pData = (spreader_data*) (hSpr);
    int q, src, ng, ch, i, j, k, band, t, nSources, Q, centre_ind, nSpread, nc;
    float trace, Ey, Eproto, Gcomp;
    float src_dirs_deg[SPREADER_MAX_NUM_SOURCES][2];
    float src_dir_xyz[3];
    float CprotoDiag[MAX_NUM_OUTPUTS * MAX_NUM_OUTPUTS];
    float src_spread[MAX_NUM_OUTPUTS];
    float_complex scaleC, tmp;
    SPREADER_PROC_MODES procMode;
    const float_complex calpha = cmplxf(1.0f, 0.0f), cbeta = cmplxf(0.0f, 0.0f);

    procMode = pData->procMode;
    nSources = pData->nSources;
    Q = pData->Q;
    for (i = 0; i < nSources; i++) {
        src_dirs_deg[i][0] = pData->src_dirs_deg[i][0];
        src_dirs_deg[i][1] = pData->src_dirs_deg[i][1];
        src_spread[i] = pData->src_spread[i];
    }

    if ((nSamples == SPREADER_FRAME_SIZE) && (pData->codecStatus == CODEC_STATUS_INITIALISED)) {
        pData->procStatus = PROC_STATUS_ONGOING;

        for (i = 0; i < SAF_MIN(nSources, nInputs); i++)
            utility_svvcopy(inputs[i], SPREADER_FRAME_SIZE, pData->inputFrameTD[i]);
        for (; i < nSources; i++)
            memset(pData->inputFrameTD[i], 0, SPREADER_FRAME_SIZE * sizeof(float));

        afSTFT_forward_knownDimensions(
            pData->hSTFT, pData->inputFrameTD, SPREADER_FRAME_SIZE, MAX_NUM_INPUTS, TIME_SLOTS, pData->inputframeTF);

        for (band = 0; band < HYBRID_BANDS; band++)
            memset(FLATTEN2D(pData->outputframeTF[band]), 0, Q * TIME_SLOTS * sizeof(float_complex));

        for (src = 0; src < nSources; src++) {
            /* Rebuild cone index list only when az/el/spread changes (direction-invariant
             * precompute): avoids iterating all nGrid directions every frame for static sources. */
            {
                float az_s = src_dirs_deg[src][0];
                float el_s = src_dirs_deg[src][1];
                float sp_s = src_spread[src];
                int needs_rebuild = !pData->cone_valid[src] ||
                    az_s != pData->cone_cached_az[src] ||
                    el_s != pData->cone_cached_el[src] ||
                    sp_s != pData->cone_cached_spread[src];
                if (needs_rebuild) {
                    int* cng = pData->cone_ng_flat + (size_t)src * (size_t)pData->nGrid;
                    int nc_build = 0;
                    unitSph2cart(src_dirs_deg[src], 1, 1, src_dir_xyz);
                    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                                pData->nGrid, 1, 3, 1.0f,
                                pData->grid_dirs_xyz, 3,
                                src_dir_xyz, 1, 0.0f,
                                pData->angles, 1);
                    for (i = 0; i < pData->nGrid; i++)
                        pData->angles[i] = acosf(SAF_MIN(pData->angles[i], 0.9999999f)) * 180.0f / SAF_PI;
                    utility_siminv(pData->angles, pData->nGrid, &centre_ind);
                    for (ng = 0; ng < pData->nGrid; ng++) {
                        pData->dirActive[src][ng] = (pData->angles[ng] <= sp_s / 2.0f) ? 1 : 0;
                        if (pData->dirActive[src][ng])
                            cng[nc_build++] = ng;
                    }
                    pData->n_cone[src] = nc_build;
                    pData->centre_ind_cache[src] = centre_ind;
                    pData->cone_cached_az[src] = az_s;
                    pData->cone_cached_el[src] = el_s;
                    pData->cone_cached_spread[src] = sp_s;
                    pData->cone_valid[src] = 1;
                } else {
                    centre_ind = pData->centre_ind_cache[src];
                }
            }

            switch (procMode) {
            case SPREADER_MODE_NAIVE: /* fall through */
            case SPREADER_MODE_OM:
                for (band = 0; band < HYBRID_BANDS; band++) {
                    const int* cng = pData->cone_ng_flat + (size_t)src * (size_t)pData->nGrid;
                    nc = (pData->freqVector[band] < MAX_SPREAD_FREQ) ? pData->n_cone[src] : 0;
                    memset(pData->_H_tmp, 0, Q * sizeof(float_complex));
                    nSpread = nc;
                    for (k = 0; k < nc; k++) {
                        ng = cng[k];
                        for (q = 0; q < Q; q++)
                            pData->_H_tmp[q] =
                                ccaddf(pData->_H_tmp[q],
                                       pData->H_grid[band * Q * pData->nGrid + q * pData->nGrid + ng]);
                    }
                    if (nSpread == 0) {
                        for (q = 0; q < Q; q++)
                            pData->_H_tmp[q] = pData->H_grid[band * Q * pData->nGrid + q * pData->nGrid + centre_ind];
                        nSpread = 1;
                    }
                    cblas_cgemm(CblasRowMajor,
                                CblasNoTrans,
                                CblasNoTrans,
                                Q,
                                TIME_SLOTS,
                                1,
                                &calpha,
                                pData->_H_tmp,
                                1,
                                pData->inputframeTF[band][src],
                                TIME_SLOTS,
                                &cbeta,
                                FLATTEN2D(pData->protoframeTF[band]),
                                TIME_SLOTS);
                    cblas_sscal(
                        2 * Q * TIME_SLOTS, 1.0f / (float) nSpread, (float*) FLATTEN2D(pData->protoframeTF[band]), 1);
                }
                break;

            case SPREADER_MODE_EVD:
                for (band = 0; band < HYBRID_BANDS; band++)
                    for (q = 0; q < Q; q++)
                        memcpy(pData->protoframeTF[band][q],
                               pData->inputframeTF[band][src],
                               TIME_SLOTS * sizeof(float_complex));
                break;
            }

            if (procMode == SPREADER_MODE_NAIVE) {
                for (band = 0; band < HYBRID_BANDS; band++)
                    memcpy(FLATTEN2D(pData->spreadframeTF[band]),
                           FLATTEN2D(pData->protoframeTF[band]),
                           Q * TIME_SLOTS * sizeof(float_complex));
            } else {
                latticeDecorrelator_apply(pData->hDecor[src], pData->protoframeTF, TIME_SLOTS, pData->decorframeTF);

                for (band = 0; band < HYBRID_BANDS; band++) {
                    cblas_cgemm(CblasRowMajor,
                                CblasNoTrans,
                                CblasConjTrans,
                                Q,
                                Q,
                                TIME_SLOTS,
                                &calpha,
                                FLATTEN2D(pData->protoframeTF[band]),
                                TIME_SLOTS,
                                FLATTEN2D(pData->protoframeTF[band]),
                                TIME_SLOTS,
                                &cbeta,
                                pData->_Cproto,
                                Q);
                    cblas_sscal(2 * Q * Q, pData->covAvgCoeff, (float*) pData->Cproto[src][band], 1);
                    cblas_saxpy(2 * Q * Q,
                                1.0f - pData->covAvgCoeff,
                                (float*) pData->_Cproto,
                                1,
                                (float*) pData->Cproto[src][band],
                                1);
                }

                for (band = 0; band < HYBRID_BANDS; band++) {
                    const int* cng = pData->cone_ng_flat + (size_t)src * (size_t)pData->nGrid;
                    nc = (pData->freqVector[band] < MAX_SPREAD_FREQ) ? pData->n_cone[src] : 0;
                    memset(pData->_Cy, 0, Q * Q * sizeof(float_complex));
                    memset(pData->_H_tmp, 0, Q * sizeof(float_complex));
                    nSpread = nc;
                    for (k = 0; k < nc; k++) {
                        ng = cng[k];
                        cblas_caxpy(Q * Q, &calpha, pData->HHH[band][ng], 1, pData->_Cy, 1);
                        for (q = 0; q < Q; q++)
                            pData->_H_tmp[q] =
                                ccaddf(pData->_H_tmp[q],
                                       pData->H_grid[band * Q * pData->nGrid + q * pData->nGrid + ng]);
                    }
                    if (nSpread == 0) {
                        cblas_caxpy(Q * Q, &calpha, pData->HHH[band][centre_ind], 1, pData->_Cy, 1);
                        for (q = 0; q < Q; q++)
                            pData->_H_tmp[q] = pData->H_grid[band * Q * pData->nGrid + q * pData->nGrid + centre_ind];
                        nSpread++;
                    }

                    if (procMode == SPREADER_MODE_OM && pData->freqVector[band] < MAX_SPREAD_FREQ) {
                        trace = 0.0f;
                        for (q = 0; q < Q; q++)
                            trace += crealf(pData->_Cy[q * Q + q]);
                        cblas_sscal(2 * Q * Q, 1.0f / (trace + 2.23e-9f), (float*) pData->_Cy, 1);

                        for (q = 0; q < Q; q++)
                            pData->_H_tmp[q] = pData->H_grid[band * Q * pData->nGrid + q * pData->nGrid + centre_ind];
                        cblas_cgemm(CblasRowMajor,
                                    CblasNoTrans,
                                    CblasNoTrans,
                                    Q,
                                    TIME_SLOTS,
                                    1,
                                    &calpha,
                                    pData->_H_tmp,
                                    1,
                                    pData->inputframeTF[band][src],
                                    TIME_SLOTS,
                                    &cbeta,
                                    pData->_tmpFrame,
                                    TIME_SLOTS);
                        cblas_cgemm(CblasRowMajor,
                                    CblasNoTrans,
                                    CblasConjTrans,
                                    Q,
                                    Q,
                                    TIME_SLOTS,
                                    &calpha,
                                    pData->_tmpFrame,
                                    TIME_SLOTS,
                                    pData->_tmpFrame,
                                    TIME_SLOTS,
                                    &cbeta,
                                    pData->_E_dir,
                                    Q);
                        trace = 0.0f;
                        for (q = 0; q < Q; q++)
                            trace += crealf(pData->_E_dir[q * Q + q]);
                        cblas_sscal(2 * Q * Q, trace, (float*) pData->_Cy, 1);
                    }

                    cblas_sscal(2 * Q * Q, pData->covAvgCoeff, (float*) pData->Cy[src][band], 1);
                    cblas_saxpy(
                        2 * Q * Q, 1.0f - pData->covAvgCoeff, (float*) pData->_Cy, 1, (float*) pData->Cy[src][band], 1);
                }

                for (band = 0; band < HYBRID_BANDS; band++) {
                    switch (procMode) {
                    case SPREADER_MODE_NAIVE:
                        saf_print_error("Shouldn't have gotten this far?");
                        break;
                    case SPREADER_MODE_EVD:
                        Ey = Eproto = 0.0f;
                        for (band = 0; band < HYBRID_BANDS; band++) {
                            for (i = 0; i < Q; i++) {
                                Ey += crealf(pData->Cy[src][band][i * Q + i]);
                                Eproto += crealf(pData->Cproto[src][band][i * Q + i]) + 0.000001f;
                            }
                        }
                        Gcomp = sqrtf(Eproto / (Ey + 2.23e-9f));
                        for (band = 0; band < HYBRID_BANDS; band++) {
                            memcpy(pData->_Cy, pData->Cy[src][band], Q * Q * sizeof(float_complex));
                            cblas_sscal(2 * Q * Q, Gcomp, (float*) pData->_Cy, 1);
                            utility_cseig(NULL, pData->_Cy, Q, 1, pData->_V, pData->_D, NULL);
                            for (i = 0; i < Q; i++)
                                for (j = 0; j < Q; j++)
                                    pData->_D[i * Q + j] = (i == j) ? csqrtf(pData->_D[i * Q + j]) : cmplxf(0.0f, 0.0f);
                            cblas_cgemm(CblasRowMajor,
                                        CblasNoTrans,
                                        CblasNoTrans,
                                        Q,
                                        Q,
                                        Q,
                                        &calpha,
                                        pData->_V,
                                        Q,
                                        pData->_D,
                                        Q,
                                        &cbeta,
                                        pData->new_M[band],
                                        Q);
                        }
                        break;
                    case SPREADER_MODE_OM:
                        for (band = 0; band < HYBRID_BANDS; band++) {
                            if (pData->freqVector[band] < MAX_SPREAD_FREQ) {
                                cblas_ccopy(Q * Q, pData->Cproto[src][band], 1, pData->_Cproto, 1);
                                for (i = 0; i < Q; i++) {
                                    for (j = 0; j < Q; j++) {
                                        if (i == j)
                                            pData->_Cproto[i * Q + i] = craddf(pData->_Cproto[i * Q + i], 0.00001f);
                                        CprotoDiag[i * Q + j] = (i == j) ? crealf(pData->_Cproto[i * Q + i]) : 0.0f;
                                    }
                                }
                                formulate_M_and_Cr_cmplx(pData->hCdf,
                                                         pData->_Cproto,
                                                         pData->Cy[src][band],
                                                         pData->Qmix_cmplx,
                                                         0,
                                                         0.2f,
                                                         pData->new_M[band],
                                                         pData->Cr_cmplx);
                                for (i = 0; i < Q * Q; i++)
                                    pData->Cr[i] = crealf(pData->Cr_cmplx[i]);
                                formulate_M_and_Cr(pData->hCdf_res,
                                                   CprotoDiag,
                                                   pData->Cr,
                                                   pData->Qmix,
                                                   0,
                                                   0.2f,
                                                   pData->new_Mr[band],
                                                   NULL);
                            } else {
                                memcpy(pData->new_M[band], pData->Qmix_cmplx, Q * Q * sizeof(float_complex));
                                memset(pData->new_Mr[band], 0, Q * Q * sizeof(float));
                            }
                        }
                        break;
                    }
                }

                for (band = 0; band < HYBRID_BANDS; band++) {
                    for (t = 0; t < TIME_SLOTS; t++) {
                        scaleC = cmplxf(pData->interpolatorFadeIn[t], 0.0f);
                        utility_cvsmul(pData->new_M[band], &scaleC, Q * Q, pData->interp_M);
                        cblas_saxpy(2 * Q * Q,
                                    pData->interpolatorFadeOut[t],
                                    (float*) pData->prev_M[src][band],
                                    1,
                                    (float*) pData->interp_M,
                                    1);
                        for (i = 0; i < Q; i++) {
                            cblas_cdotu_sub(Q,
                                            (float_complex*) (&(pData->interp_M[i * Q])),
                                            1,
                                            FLATTEN2D((procMode == SPREADER_MODE_EVD ? pData->decorframeTF[band]
                                                                                     : pData->protoframeTF[band])) +
                                                t,
                                            TIME_SLOTS,
                                            &(pData->spreadframeTF[band][i][t]));
                        }
                    }

                    if (procMode == SPREADER_MODE_OM) {
                        if (pData->freqVector[band] < MAX_SPREAD_FREQ) {
                            for (t = 0; t < TIME_SLOTS; t++) {
                                utility_svsmul(
                                    pData->new_Mr[band], &(pData->interpolatorFadeIn[t]), Q * Q, pData->interp_Mr);
                                cblas_saxpy(Q * Q,
                                            pData->interpolatorFadeOut[t],
                                            pData->prev_Mr[src][band],
                                            1,
                                            pData->interp_Mr,
                                            1);
                                cblas_scopy(Q * Q, pData->interp_Mr, 1, (float*) pData->interp_Mr_cmplx, 2);
                                for (i = 0; i < Q; i++) {
                                    cblas_cdotu_sub(Q,
                                                    (float_complex*) (&(pData->interp_Mr_cmplx[i * Q])),
                                                    1,
                                                    FLATTEN2D(pData->decorframeTF[band]) + t,
                                                    TIME_SLOTS,
                                                    &tmp);
                                    pData->spreadframeTF[band][i][t] = ccaddf(pData->spreadframeTF[band][i][t], tmp);
                                }
                            }
                        }
                    }
                }
            }

            for (band = 0; band < HYBRID_BANDS; band++)
                cblas_saxpy(2 * Q * TIME_SLOTS,
                            1.0f,
                            (float*) FLATTEN2D(pData->spreadframeTF[band]),
                            1,
                            (float*) FLATTEN2D(pData->outputframeTF[band]),
                            1);

            cblas_ccopy(HYBRID_BANDS * Q * Q, FLATTEN2D(pData->new_M), 1, FLATTEN2D(pData->prev_M[src]), 1);
            cblas_scopy(HYBRID_BANDS * Q * Q, FLATTEN2D(pData->new_Mr), 1, FLATTEN2D(pData->prev_Mr[src]), 1);
        }

        afSTFT_backward_knownDimensions(
            pData->hSTFT, pData->outputframeTF, SPREADER_FRAME_SIZE, MAX_NUM_OUTPUTS, TIME_SLOTS, pData->outframeTD);

        for (ch = 0; ch < SAF_MIN(Q, nOutputs); ch++)
            utility_svvcopy(pData->outframeTD[ch], SPREADER_FRAME_SIZE, outputs[ch]);
        for (; ch < nOutputs; ch++)
            memset(outputs[ch], 0, SPREADER_FRAME_SIZE * sizeof(float));
    } else {
        for (ch = 0; ch < nOutputs; ch++)
            memset(outputs[ch], 0, SPREADER_FRAME_SIZE * sizeof(float));
    }

    pData->procStatus = PROC_STATUS_NOT_ONGOING;
}

/* ── Set/Get functions (unchanged from original) ───────────────────── */

void spreader_setCodecStatus(void* const hSpr, CODEC_STATUS newStatus) {
    spreader_data* pData = (spreader_data*) (hSpr);
    if (newStatus == CODEC_STATUS_NOT_INITIALISED) {
        while (pData->procStatus == PROC_STATUS_ONGOING)
            SAF_SLEEP(10);
    }
    pData->codecStatus = newStatus;
}

void spreader_refreshSettings(void* const hSpr) {
    spreader_setCodecStatus(hSpr, CODEC_STATUS_NOT_INITIALISED);
}

void spreader_setSpreadingMode(void* const hSpr, int newMode) {
    spreader_data* pData = (spreader_data*) (hSpr);
    pData->new_procMode = newMode;
    spreader_setCodecStatus(hSpr, CODEC_STATUS_NOT_INITIALISED);
}

void spreader_setAveragingCoeff(void* const hSpr, float newValue) {
    spreader_data* pData = (spreader_data*) (hSpr);
    pData->covAvgCoeff = newValue;
}

void spreader_setSourceAzi_deg(void* const hSpr, int index, float newAzi_deg) {
    spreader_data* pData = (spreader_data*) (hSpr);
    saf_assert(index < SPREADER_MAX_NUM_SOURCES, "index exceeds the maximum number of sources");
    if (newAzi_deg > 180.0f)
        newAzi_deg = -360.0f + newAzi_deg;
    newAzi_deg = SAF_MAX(newAzi_deg, -180.0f);
    newAzi_deg = SAF_MIN(newAzi_deg, 180.0f);
    pData->src_dirs_deg[index][0] = newAzi_deg;
}

void spreader_setSourceElev_deg(void* const hSpr, int index, float newElev_deg) {
    spreader_data* pData = (spreader_data*) (hSpr);
    saf_assert(index < SPREADER_MAX_NUM_SOURCES, "index exceeds the maximum number of sources");
    newElev_deg = SAF_MAX(newElev_deg, -90.0f);
    newElev_deg = SAF_MIN(newElev_deg, 90.0f);
    pData->src_dirs_deg[index][1] = newElev_deg;
}

void spreader_setSourceSpread_deg(void* const hSpr, int index, float newSpread_deg) {
    spreader_data* pData = (spreader_data*) (hSpr);
    saf_assert(index < SPREADER_MAX_NUM_SOURCES, "index exceeds the maximum number of sources");
    newSpread_deg = SAF_MAX(newSpread_deg, 0.0f);
    newSpread_deg = SAF_MIN(newSpread_deg, 360.0f);
    pData->src_spread[index] = newSpread_deg;
}

void spreader_setNumSources(void* const hSpr, int new_nSources) {
    spreader_data* pData = (spreader_data*) (hSpr);
    pData->new_nSources = SAF_CLAMP(new_nSources, 1, SPREADER_MAX_NUM_SOURCES);
    spreader_setCodecStatus(hSpr, CODEC_STATUS_NOT_INITIALISED);
}

void spreader_setUseDefaultHRIRsflag(void* const hSpr, int newState) {
    spreader_data* pData = (spreader_data*) (hSpr);
    if ((!pData->useDefaultHRIRsFLAG) && newState) {
        pData->useDefaultHRIRsFLAG = newState;
        spreader_refreshSettings(hSpr);
    }
}

void spreader_setSofaFilePath(void* const hSpr, const char* path) {
    spreader_data* pData = (spreader_data*) (hSpr);
    pData->sofa_filepath = realloc1d(pData->sofa_filepath, strlen(path) + 1);
    strcpy(pData->sofa_filepath, path);
    pData->useDefaultHRIRsFLAG = 0;
    spreader_refreshSettings(hSpr);
}

int spreader_getFrameSize(void) {
    return SPREADER_FRAME_SIZE;
}
CODEC_STATUS spreader_getCodecStatus(void* const hSpr) {
    return ((spreader_data*) hSpr)->codecStatus;
}
float spreader_getProgressBar0_1(void* const hSpr) {
    return ((spreader_data*) hSpr)->progressBar0_1;
}
void spreader_getProgressBarText(void* const hSpr, char* text) {
    memcpy(text, ((spreader_data*) hSpr)->progressBarText, PROGRESSBARTEXT_CHAR_LENGTH * sizeof(char));
}
int* spreader_getDirectionActivePtr(void* const hSpr, int index) {
    return ((spreader_data*) hSpr)->dirActive[index];
}
int spreader_getSpreadingMode(void* const hSpr) {
    return ((spreader_data*) hSpr)->new_procMode;
}
float spreader_getAveragingCoeff(void* const hSpr) {
    return ((spreader_data*) hSpr)->covAvgCoeff;
}
float spreader_getSourceAzi_deg(void* const hSpr, int index) {
    return ((spreader_data*) hSpr)->src_dirs_deg[index][0];
}
float spreader_getSourceElev_deg(void* const hSpr, int index) {
    return ((spreader_data*) hSpr)->src_dirs_deg[index][1];
}
float spreader_getSourceSpread_deg(void* const hSpr, int index) {
    return ((spreader_data*) hSpr)->src_spread[index];
}
int spreader_getNumSources(void* const hSpr) {
    return ((spreader_data*) hSpr)->new_nSources;
}
int spreader_getMaxNumSources(void) {
    return SPREADER_MAX_NUM_SOURCES;
}
int spreader_getNumOutputs(void* const hSpr) {
    return ((spreader_data*) hSpr)->Q;
}
int spreader_getNDirs(void* const hSpr) {
    return ((spreader_data*) hSpr)->nGrid;
}
float spreader_getIRAzi_deg(void* const hSpr, int index) {
    spreader_data* p = (spreader_data*) hSpr;
    return (p->codecStatus == CODEC_STATUS_INITIALISED && p->grid_dirs_deg) ? p->grid_dirs_deg[index * 2 + 0] : 0.0f;
}
float spreader_getIRElev_deg(void* const hSpr, int index) {
    spreader_data* p = (spreader_data*) hSpr;
    return (p->codecStatus == CODEC_STATUS_INITIALISED && p->grid_dirs_deg) ? p->grid_dirs_deg[index * 2 + 1] : 0.0f;
}
int spreader_getIRlength(void* const hSpr) {
    return ((spreader_data*) hSpr)->h_len;
}
int spreader_getIRsamplerate(void* const hSpr) {
    return (int) ((spreader_data*) hSpr)->h_fs;
}
int spreader_getUseDefaultHRIRsflag(void* const hSpr) {
    return ((spreader_data*) hSpr)->useDefaultHRIRsFLAG;
}
char* spreader_getSofaFilePath(void* const hSpr) {
    spreader_data* p = (spreader_data*) hSpr;
    return p->sofa_filepath ? p->sofa_filepath : "no_file";
}
int spreader_getDAWsamplerate(void* const hSpr) {
    return ((spreader_data*) hSpr)->fs;
}
int spreader_getProcessingDelay(void) {
    return 12 * HOP_SIZE;
}

/* ── Extension: init from pre-loaded HRTF grid ──────────────────────── */

void spreader_init_from_hrtf_grid(void* const hSpr,
                                  const float* h_grid_flat,
                                  const float* grid_dirs_deg_flat,
                                  int nGrid,
                                  int Q,
                                  int h_len,
                                  int sample_rate) {
    spreader_data* pData = (spreader_data*) (hSpr);

    pData->Q = Q;
    pData->nGrid = nGrid;
    pData->h_len = h_len;
    pData->h_fs = (float) sample_rate;

    pData->h_grid = realloc1d(pData->h_grid, nGrid * Q * h_len * sizeof(float));
    memcpy(pData->h_grid, h_grid_flat, nGrid * Q * h_len * sizeof(float));

    pData->grid_dirs_deg = realloc1d(pData->grid_dirs_deg, nGrid * 2 * sizeof(float));
    memcpy(pData->grid_dirs_deg, grid_dirs_deg_flat, nGrid * 2 * sizeof(float));

    pData->useExternalHRIRsFLAG = 1;
    pData->codecStatus = CODEC_STATUS_NOT_INITIALISED;

    /* (re)allocate cone index cache for the new grid size */
    pData->cone_ng_flat = realloc1d(pData->cone_ng_flat, (size_t)nGrid * SPREADER_MAX_NUM_SOURCES * sizeof(int));
    memset(pData->cone_valid, 0, sizeof(pData->cone_valid));
}
