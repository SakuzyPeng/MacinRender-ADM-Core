// Private render-orchestration boundary (mradm-engine, PCM consistency 5.2 A): the Rust backends'
// capability records and the discovery JSON documents. Backend records are process-lifetime Rust
// data and are never freed; a document is owned by its MradmEngineText handle.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../adm_metadata/adm_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmEngineText MradmEngineText;

// flags: 1 3D, 2 supports spread, 4 binaural.
typedef struct MradmEngineLayout {
    MradmAdmText id;
    MradmAdmText display_name;
    uint16_t channel_count;
    uint16_t lfe_count;
    uint32_t flags;
} MradmEngineLayout;

typedef struct MradmEngineMode {
    MradmAdmText id;
    MradmAdmText display_name;
    const MradmAdmText* layouts;
    size_t layouts_len;
    const uint32_t* sample_rates;
    size_t sample_rates_len;
    uint32_t supports_realtime;
} MradmEngineMode;

// flags: 1 objects, 2 DirectSpeakers, 4 HOA, 8 channelLock, 16 divergence, 32 screenRef,
// 64 diffuse, 128 render window.
typedef struct MradmEngineBackend {
    MradmAdmText renderer;
    MradmAdmText name;
    MradmAdmText version;
    const MradmEngineLayout* layouts;
    size_t layouts_len;
    const MradmEngineMode* modes;
    size_t modes_len;
    const MradmEngineMode* qualities;
    size_t qualities_len;
    const MradmAdmText* hrtf_sources;
    size_t hrtf_sources_len;
    uint32_t flags;
} MradmEngineBackend;

typedef struct MradmEngineSpatialLayout {
    MradmAdmText id;
    MradmAdmText display_name;
    uint16_t channel_count;
    uint16_t reserved;
} MradmEngineSpatialLayout;

// Island facts: apple_lfe_routing_ok is -1 when absent; apple may be NULL.
typedef struct MradmEnginePlatform {
    uint32_t apac;
    uint32_t iamf;
    uint32_t iamf_mp4_packager;
    int32_t apple_lfe_routing_ok;
    const MradmEngineBackend* apple;
    const MradmEngineSpatialLayout* system_spatial;
    size_t system_spatial_len;
} MradmEnginePlatform;

// index: 0 ear, 1 saf, 2 triple-balance, 3 hoa, 4 saf-binaural, 5 room-binaural.
int32_t mradm_engine_backend(uint32_t index, const MradmEngineBackend** out);
// kind: 0 capabilities, 1 layouts, 2 output formats, 3 input layouts, 4 render support matrix.
int32_t mradm_engine_document(uint32_t kind,
                              const MradmEnginePlatform* platform,
                              MradmEngineText** out,
                              MradmAdmText* document);
void mradm_engine_text_destroy(MradmEngineText* text);

#ifdef __cplusplus
}
#endif
