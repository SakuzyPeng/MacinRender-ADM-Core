cmake_minimum_required(VERSION 3.24)

project(
    MacinRenderADMCore
    VERSION 1.8.0
    DESCRIPTION "Cross-platform ADM rendering core for MacinRender"
    LANGUAGES C CXX
)

option(MR_ADM_CORE_BUILD_CLI "Build the mradm command-line interface" ON)
option(MR_ADM_CORE_BUILD_TESTS "Build tests" ON)
option(MR_ADM_CORE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(MR_ADM_CORE_FETCH_DEPS "Fetch CLI/fmt/spdlog dependencies when not found" ON)
option(MR_ADM_CORE_USE_INSTALLED_DEPS "Use installed CMake package configs before FetchContent dependencies" ON)
option(MR_ADM_CORE_ENABLE_CLANG_TIDY "Run clang-tidy as part of compilation when available" OFF)
option(MR_ADM_CORE_ENABLE_CPPCHECK "Run cppcheck as part of compilation when available" OFF)
option(MR_ADM_ENABLE_SOFA "Enable user SOFA HRIR loading for the binaural renderer" ON)
option(MR_ADM_ENABLE_IAMF "Enable IAMF encoding via the official AOM iamf-tools prebuilt bridge" OFF)
set(MR_ADM_IAMF_AOM_ROOT "" CACHE PATH "Root directory of the prebuilt AOM iamf-tools bridge SDK")
set(MR_ADM_FLAC_PROVIDER "AUTO" CACHE STRING "libFLAC provider: AUTO, SYSTEM, or VENDORED")
set_property(CACHE MR_ADM_FLAC_PROVIDER PROPERTY STRINGS AUTO SYSTEM VENDORED)
option(MR_ADM_USE_SYSTEM_FLAC "Shortcut for MR_ADM_FLAC_PROVIDER=SYSTEM" OFF)
set(MR_ADM_OPUS_PROVIDER "AUTO" CACHE STRING "libopus provider: AUTO, SYSTEM, or VENDORED")
set_property(CACHE MR_ADM_OPUS_PROVIDER PROPERTY STRINGS AUTO SYSTEM VENDORED)
option(MR_ADM_USE_SYSTEM_OPUS "Shortcut for MR_ADM_OPUS_PROVIDER=SYSTEM" OFF)

# 编译缓存：探测到 ccache/sccache 就接为编译启动器（找不到则优雅降级，不影响无缓存环境/CI）。
# 显著加速全量与切分支重编；不改变编译产物。
if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(MR_ADM_COMPILER_CACHE NAMES ccache sccache)
    if(MR_ADM_COMPILER_CACHE)
        set(CMAKE_C_COMPILER_LAUNCHER "${MR_ADM_COMPILER_CACHE}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${MR_ADM_COMPILER_CACHE}")
        message(STATUS "Compiler cache enabled: ${MR_ADM_COMPILER_CACHE}")
    endif()
endif()

function(mr_adm_core_enable_fast_debug_flags)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()

    foreach(lang IN ITEMS C CXX)
        set(flags_var "CMAKE_${lang}_FLAGS_DEBUG")
        set(flags "${${flags_var}}")

        if(MSVC)
            foreach(pattern IN ITEMS
                    "(^| )[/-]Od( |$)"
                    "(^| )[/-]RTC[^ ]*( |$)"
                    "(^| )[/-]O[0-2dx]( |$)"
                    "(^| )[/-]Ob[0-2]( |$)"
                    "(^| )[/-]Z[7iI]( |$)"
                    "(^| )-g( |$)"
                    "(^| )-Og( |$)")
                string(REGEX REPLACE "${pattern}" " " flags "${flags}")
            endforeach()
            # sccache/ccache 无法缓存 /Zi 产生的独立 PDB；当启用编译器启动器时改用 /Z7，
            # 把调试信息内嵌进 .obj，使对象文件可被缓存（主要面向 CI 的增量构建提速）。
            if(CMAKE_${lang}_COMPILER_LAUNCHER MATCHES "ccache|sccache")
                string(APPEND flags " /Z7 /O1 /Ob1")
            else()
                string(APPEND flags " /Zi /O1 /Ob1")
            endif()
        elseif("${CMAKE_${lang}_COMPILER_ID}" MATCHES "Clang|GNU")
            string(REGEX REPLACE "(^| )-Og( |$)" " " flags "${flags}")
            string(APPEND flags " -Og")
        endif()

        string(REGEX REPLACE " +" " " flags "${flags}")
        string(STRIP "${flags}" flags)
        set(${flags_var} "${flags}" CACHE STRING "Flags used by the ${lang} compiler during Debug builds." FORCE)
    endforeach()
endfunction()
mr_adm_core_enable_fast_debug_flags()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(NOT DEFINED MR_ADM_FLAC_AUTO_BUILD_TYPE)
    set(MR_ADM_FLAC_AUTO_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE INTERNAL "Initial build type for FLAC AUTO provider")
endif()
if(NOT DEFINED MR_ADM_OPUS_AUTO_BUILD_TYPE)
    set(MR_ADM_OPUS_AUTO_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE INTERNAL "Initial build type for Opus AUTO provider")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(MRWarnings)
include(MRDependencies)
include(MRStaticAnalysis)
find_package(Threads REQUIRED)
if(MR_ADM_ENABLE_IAMF)
    include(MRIamfAomBridge)
    mr_adm_core_find_iamf_aom_bridge()
endif()

# ── adm_core ──────────────────────────────────────────────────────────────────
# Domain types, error handling, abstract IRenderer interface.
# No third-party ADM/renderer types leak into this target's public headers.

add_library(mr_adm_core)
add_library(MacinRender::ADMCore ALIAS mr_adm_core)

target_sources(
    mr_adm_core
    PRIVATE
        src/adm_core/semantic_policy.cpp
        src/adm_core/version.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/capability.h
            include/adm/errors.h
            include/adm/live_override.h
            include/adm/logging.h
            include/adm/monitor.h
            include/adm/options.h
            include/adm/progress.h
            include/adm/render.h
            include/adm/semantic_policy.h
            include/adm/version.h
)

target_include_directories(
    mr_adm_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(mr_adm_core PUBLIC tl::expected fmt::fmt)
target_include_directories(mr_adm_core SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
)
mr_adm_core_apply_warnings(mr_adm_core)
mr_adm_core_apply_static_analysis(mr_adm_core)

# ── adm_io ────────────────────────────────────────────────────────────────────

add_library(mr_adm_io)
add_library(MacinRender::ADMIo ALIAS mr_adm_io)

target_sources(
    mr_adm_io
    PRIVATE
        src/adm_io/scene_importer.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/io.h
            include/adm/scene.h
)

target_link_libraries(mr_adm_io
    PUBLIC MacinRender::ADMCore
    PRIVATE libbw64 adm
)
mr_adm_core_apply_warnings(mr_adm_io)
mr_adm_core_apply_static_analysis(mr_adm_io)

# ── adm_render_common ─────────────────────────────────────────────────────────
# Backend-agnostic object preprocessing and block timeline helpers used by
# speaker renderers. No libear/SAF/Apple types may appear in this target.

add_library(mr_adm_render_common)
add_library(MacinRender::ADMRenderCommon ALIAS mr_adm_render_common)

target_sources(
    mr_adm_render_common
    PRIVATE
        src/adm_render_common/render_common.cpp
        src/adm_render_common/speaker_layouts.cpp
)
target_include_directories(mr_adm_render_common PUBLIC src/adm_render_common)
target_link_libraries(mr_adm_render_common PUBLIC MacinRender::ADMCore Threads::Threads)
mr_adm_core_apply_warnings(mr_adm_render_common)
mr_adm_core_apply_static_analysis(mr_adm_render_common)

# ── adm_render_ear ────────────────────────────────────────────────────────────
# libear-based Objects renderer. libear types confined to this target.

add_library(mr_adm_render_ear)
add_library(MacinRender::ADMRenderEar ALIAS mr_adm_render_ear)

target_sources(
    mr_adm_render_ear
    PRIVATE
        src/adm_render_ear/ear_renderer.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/render_ear.h
)

target_link_libraries(mr_adm_render_ear
    PUBLIC MacinRender::ADMCore
    PRIVATE libbw64 adm ear saf ebur128 MacinRender::ADMAudio MacinRender::ADMRenderCommon
)
target_include_directories(mr_adm_render_ear
    SYSTEM PRIVATE
        "${saf_SOURCE_DIR}/modules/saf_utilities"
        "${EBUR128_INCLUDE_DIR}"
)
mr_adm_core_apply_warnings(mr_adm_render_ear)
mr_adm_core_apply_static_analysis(mr_adm_render_ear)

# ── adm_render_vbap ───────────────────────────────────────────────────────────
# SAF-backed VBAP renderer. SAF types are confined to this target.
# M7 scope: Objects-to-loudspeaker rendering through SAF VBAP/MDAP.

add_library(mr_adm_render_vbap)
add_library(MacinRender::ADMRenderVBAP ALIAS mr_adm_render_vbap)

target_sources(
    mr_adm_render_vbap
    PRIVATE
        src/adm_render_vbap/vbap_renderer.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/render_vbap.h
)

target_link_libraries(mr_adm_render_vbap
    PUBLIC MacinRender::ADMCore
    PRIVATE libbw64 saf ebur128 MacinRender::ADMAudio MacinRender::ADMRenderCommon
)
target_include_directories(mr_adm_render_vbap
    SYSTEM PRIVATE
        "${saf_SOURCE_DIR}/modules/saf_vbap"
        "${EBUR128_INCLUDE_DIR}"
)
mr_adm_core_apply_warnings(mr_adm_render_vbap)
mr_adm_core_apply_static_analysis(mr_adm_render_vbap)

# ── adm_render_hoa ────────────────────────────────────────────────────────────
# HOA encode renderer: Objects → HOA bed (ACN/SN3D).
# Output order is selected via output_layout ("hoa3" = 3rd order, 16ch).

add_library(mr_adm_render_hoa)
add_library(MacinRender::ADMRenderHOA ALIAS mr_adm_render_hoa)

target_sources(
    mr_adm_render_hoa
    PRIVATE
        src/adm_render_hoa/hoa_renderer.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/render_hoa.h
)

target_link_libraries(mr_adm_render_hoa
    PUBLIC MacinRender::ADMCore
    PRIVATE libbw64 saf ebur128 MacinRender::ADMAudio MacinRender::ADMRenderCommon
)
target_include_directories(mr_adm_render_hoa
    SYSTEM PRIVATE
        "${saf_SOURCE_DIR}/modules/saf_hoa"
        "${saf_SOURCE_DIR}/modules/saf_utilities"
        "${EBUR128_INCLUDE_DIR}"
)
mr_adm_core_apply_warnings(mr_adm_render_hoa)
mr_adm_core_apply_static_analysis(mr_adm_render_hoa)

# ── adm_render_binaural ───────────────────────────────────────────────────────
# SAF HRTF binaural renderer: Objects/DirectSpeakers → 2ch via VBAP-interpolated
# KEMAR or user SOFA HRTF convolution.  HOA not supported.

add_library(mr_adm_render_binaural)
add_library(MacinRender::ADMRenderBinaural ALIAS mr_adm_render_binaural)

target_sources(
    mr_adm_render_binaural
    PRIVATE
        src/adm_render_binaural/binaural_renderer.cpp
        src/adm_render_binaural/binaural_spreader.cpp
        src/adm_render_binaural/spreader_mr.c
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/render_binaural.h
)

target_link_libraries(mr_adm_render_binaural
    PUBLIC MacinRender::ADMCore
    PRIVATE libbw64 saf ebur128 MacinRender::ADMAudio MacinRender::ADMRenderCommon
)
target_include_directories(mr_adm_render_binaural
    SYSTEM PRIVATE
        "${saf_SOURCE_DIR}/modules/saf_hrir"
        "${saf_SOURCE_DIR}/modules/saf_sofa_reader"
        "${saf_SOURCE_DIR}/modules/saf_vbap"
        "${saf_SOURCE_DIR}/modules/saf_utilities"
        "${EBUR128_INCLUDE_DIR}"
        "${saf_SOURCE_DIR}/../examples/include"
)
mr_adm_core_apply_warnings(mr_adm_render_binaural)
mr_adm_core_apply_static_analysis(mr_adm_render_binaural)

# ── adm_audio ─────────────────────────────────────────────────────────────────
# Float32 WAV/CAF/FLAC, Opus MKA, APAC, and metadata I/O.
# DR_WAV_IMPLEMENTATION is defined in wav_io.cpp only; DR_FLAC_IMPLEMENTATION is
# defined in flac_io.cpp only. FLAC encoding uses libFLAC; decoding uses dr_flac.

add_library(mr_adm_audio)
add_library(MacinRender::ADMAudio ALIAS mr_adm_audio)

target_sources(
    mr_adm_audio
    PRIVATE
        src/adm_audio/apac_io.cpp
        src/adm_audio/audio_handles.cpp
        src/adm_audio/caf_io.cpp
        src/adm_audio/flac_io.cpp
        src/adm_audio/iamf_io.cpp
        src/adm_audio/metadata_io.cpp
        src/adm_audio/opus_mka_io.cpp
        src/adm_audio/wav_io.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/audio_io.h
)

target_link_libraries(mr_adm_audio
    PUBLIC MacinRender::ADMCore
    PRIVATE dr_wav::dr_wav dr_flac::dr_flac FLAC::FLAC Opus::opus libbw64
)
target_compile_definitions(mr_adm_audio PRIVATE MR_ADM_ENABLE_IAMF=$<BOOL:${MR_ADM_ENABLE_IAMF}>)
if(MR_ADM_ENABLE_IAMF)
    target_link_libraries(mr_adm_audio PRIVATE MacinRender::IamfAomBridge)
endif()
if(APPLE)
    find_library(MR_ADM_AUDIOTOOLBOX_FW AudioToolbox REQUIRED)
    find_library(MR_ADM_COREFOUNDATION_FW CoreFoundation REQUIRED)
    target_link_libraries(mr_adm_audio PRIVATE
        ${MR_ADM_AUDIOTOOLBOX_FW}
        ${MR_ADM_COREFOUNDATION_FW}
    )
endif()
mr_adm_core_apply_warnings(mr_adm_audio)
mr_adm_core_apply_static_analysis(mr_adm_audio)

# ── adm_apple ─────────────────────────────────────────────────────────────────
# macOS-only AUSpatialMixer backend. Apple framework types are confined here.

if(APPLE)
    add_library(mr_adm_apple)
    add_library(MacinRender::ADMRenderApple ALIAS mr_adm_apple)

    find_library(MR_ADM_AVFOUNDATION_FW AVFoundation REQUIRED)
    find_library(MR_ADM_COREMEDIA_FW CoreMedia REQUIRED)
    find_library(MR_ADM_FOUNDATION_FW Foundation REQUIRED)

    target_sources(
        mr_adm_apple
        PRIVATE
            src/adm_apple/spatial_mixer_renderer.cpp
            src/adm_apple/avsamplebuffer_device.mm
        PUBLIC
            FILE_SET HEADERS
            BASE_DIRS include
            FILES
                include/adm/render_apple.h
    )
    # ARC for the Objective-C++ AVSampleBufferAudioRenderer sink (manages the ObjC lifetimes).
    set_source_files_properties(src/adm_apple/avsamplebuffer_device.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

    target_link_libraries(mr_adm_apple
        PUBLIC MacinRender::ADMCore
        PRIVATE libbw64 ebur128 MacinRender::ADMAudio MacinRender::ADMRenderCommon
                ${MR_ADM_AUDIOTOOLBOX_FW} ${MR_ADM_AVFOUNDATION_FW} ${MR_ADM_COREMEDIA_FW} ${MR_ADM_FOUNDATION_FW}
    )
    # adm_apple consumes the realtime IAudioOutputDevice interface (third-party-free header) to
    # provide the AVSampleBufferAudioRenderer monitor sink. Include-only, no link dependency —
    # the device implements the pure interface; this keeps Apple frameworks confined here.
    target_include_directories(mr_adm_apple PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/adm_realtime)
    target_include_directories(mr_adm_apple SYSTEM PRIVATE "${EBUR128_INCLUDE_DIR}")
    mr_adm_core_apply_warnings(mr_adm_apple)
    mr_adm_core_apply_static_analysis(mr_adm_apple)
endif()

# ── adm_windows ───────────────────────────────────────────────────────────────
# Windows-only system-spatial monitor sink (ISpatialAudioClient). Windows COM / SpatialAudio
# types are confined here; the factory returns the third-party-free IAudioOutputDevice.

if(WIN32)
    add_library(mr_adm_windows)
    add_library(MacinRender::ADMRenderWindows ALIAS mr_adm_windows)

    target_sources(mr_adm_windows PRIVATE
        src/adm_windows/spatialaudioclient_device.cpp
    )
    target_link_libraries(mr_adm_windows
        PUBLIC MacinRender::ADMCore
        PRIVATE Ole32
    )
    # adm_windows consumes the realtime IAudioOutputDevice interface (third-party-free header) to
    # provide the ISpatialAudioClient monitor sink. Include-only, no link dependency — the device
    # implements the pure interface; this keeps Windows COM headers confined here.
    target_include_directories(mr_adm_windows PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/adm_realtime)
    mr_adm_core_apply_warnings(mr_adm_windows)
    mr_adm_core_apply_static_analysis(mr_adm_windows)
endif()

# ── adm_peak ──────────────────────────────────────────────────────────────────
# True Peak measurement (ITU-R BS.1770-4) via libebur128 + global gain rewrite.

add_library(mr_adm_peak)
add_library(MacinRender::ADMPeak ALIAS mr_adm_peak)

target_sources(
    mr_adm_peak
    PRIVATE
        src/adm_peak/peak_limiter.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/peak.h
)

target_link_libraries(mr_adm_peak
    PUBLIC MacinRender::ADMCore
    PRIVATE ebur128 MacinRender::ADMAudio
)
# libebur128 uses legacy include_directories() that does not propagate via the
# target; use the EBUR128_INCLUDE_DIR cache variable it exports instead.
target_include_directories(mr_adm_peak SYSTEM PRIVATE "${EBUR128_INCLUDE_DIR}")
mr_adm_core_apply_warnings(mr_adm_peak)
mr_adm_core_apply_static_analysis(mr_adm_peak)

# ── adm_loudness ──────────────────────────────────────────────────────────────
# Integrated loudness normalisation (EBU R128 / ITU-R BS.1770-4).

add_library(mr_adm_loudness)
add_library(MacinRender::ADMLoudness ALIAS mr_adm_loudness)

target_sources(
    mr_adm_loudness
    PRIVATE
        src/adm_loudness/loudness_normalizer.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/loudness.h
)

target_link_libraries(mr_adm_loudness
    PUBLIC MacinRender::ADMCore
    PRIVATE ebur128 MacinRender::ADMAudio
)
target_include_directories(mr_adm_loudness SYSTEM PRIVATE "${EBUR128_INCLUDE_DIR}")
mr_adm_core_apply_warnings(mr_adm_loudness)
mr_adm_core_apply_static_analysis(mr_adm_loudness)

# ── adm_renderer_factory ──────────────────────────────────────────────────────
# Internal backend-resolution helper shared by ADMEngine (offline RenderService) and
# ADMRealtime (realtime MonitorEngine), so the two never diverge on backend selection.
# Knows every backend (create_*_renderer); kept OUT of ADMCore / public headers so the
# core domain layer is not a backend registry (ADR 0003). Private internal library.

add_library(mr_adm_renderer_factory)
add_library(MacinRender::ADMRendererFactory ALIAS mr_adm_renderer_factory)
target_sources(mr_adm_renderer_factory PRIVATE src/adm_renderer_factory/renderer_factory.cpp)
target_include_directories(mr_adm_renderer_factory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/adm_renderer_factory)
target_link_libraries(mr_adm_renderer_factory
    PUBLIC MacinRender::ADMCore
    PRIVATE MacinRender::ADMRenderEar MacinRender::ADMRenderVBAP MacinRender::ADMRenderHOA MacinRender::ADMRenderBinaural
)
if(APPLE)
    target_link_libraries(mr_adm_renderer_factory PRIVATE MacinRender::ADMRenderApple)
endif()
mr_adm_core_apply_warnings(mr_adm_renderer_factory)
mr_adm_core_apply_static_analysis(mr_adm_renderer_factory)

# ── adm_engine ────────────────────────────────────────────────────────────────
# Orchestration layer: implements RenderService, selects backends.

add_library(mr_adm_engine)
add_library(MacinRender::ADMEngine ALIAS mr_adm_engine)

target_sources(
    mr_adm_engine
    PRIVATE
        src/adm_engine/render_service.cpp
        src/adm_engine/monitor_session.cpp
        src/adm_engine/scene_json.cpp
        src/adm_engine/capability_json.cpp
        src/adm_engine/layout_table.cpp
        src/adm_engine/format_table.cpp
        src/adm_engine/support_matrix.cpp
)

target_link_libraries(mr_adm_engine
    PUBLIC MacinRender::ADMCore
    PRIVATE MacinRender::ADMIo MacinRender::ADMRendererFactory MacinRender::ADMRealtime MacinRender::ADMRenderCommon MacinRender::ADMRenderEar MacinRender::ADMRenderVBAP MacinRender::ADMRenderHOA MacinRender::ADMRenderBinaural MacinRender::ADMPeak MacinRender::ADMLoudness MacinRender::ADMAudio
)
if(APPLE)
    target_link_libraries(mr_adm_engine PRIVATE MacinRender::ADMRenderApple)
endif()
if(WIN32)
    # monitor_session selects the ISpatialAudioClient sink (make_spatialaudioclient_device) for
    # Windows system-spatial monitoring; the symbol lives in mr_adm_windows.
    target_link_libraries(mr_adm_engine PRIVATE MacinRender::ADMRenderWindows)
endif()
# nlohmann/json is header-only and used solely inside scene_json.cpp; keep it PRIVATE
# so no third-party type leaks across the module boundary (ADR 0003 / 0004).
target_include_directories(mr_adm_engine SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
)
# ADMAudio-internal headers (audio_io_internal.h): the engine orchestrates the
# audio layer and reaches its internal subprocess worker (run_apac_encode_child).
# PRIVATE so this stays an engine-only coupling and never leaks to the CLI / C ABI.
target_include_directories(mr_adm_engine PRIVATE src/adm_audio)
mr_adm_core_apply_warnings(mr_adm_engine)
mr_adm_core_apply_static_analysis(mr_adm_engine)

# ── adm_realtime ──────────────────────────────────────────────────────────────
# Realtime monitor engine: streaming render session (IRenderStream) pulled by a worker
# into an SPSC ring, drained by an injected audio device. Sits beside ADMEngine. Deps grow
# in later slices (ADMRendererFactory for the real stream factory, miniaudio for the device).
add_library(mr_adm_realtime)
add_library(MacinRender::ADMRealtime ALIAS mr_adm_realtime)
target_sources(mr_adm_realtime PRIVATE
    src/adm_realtime/monitor_engine.cpp
    src/adm_realtime/miniaudio_device.cpp
)
target_include_directories(mr_adm_realtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/adm_realtime)
# miniaudio is PRIVATE: its types never cross the IAudioOutputDevice boundary (ADR 0003).
# ebur128 is PRIVATE too: the realtime LUFS meter lives inside monitor_engine.cpp only.
target_link_libraries(mr_adm_realtime PUBLIC MacinRender::ADMCore PRIVATE Threads::Threads miniaudio ebur128)
# libebur128 uses legacy include_directories() that does not propagate via the target.
target_include_directories(mr_adm_realtime SYSTEM PRIVATE "${EBUR128_INCLUDE_DIR}")
mr_adm_core_apply_warnings(mr_adm_realtime)
mr_adm_core_apply_static_analysis(mr_adm_realtime)

# ── adm_c_api ─────────────────────────────────────────────────────────────────
# C ABI wrapper. Links to ADMEngine to get RenderService::render() symbol.

add_library(mr_adm_c_api)
add_library(MacinRender::ADMCAPI ALIAS mr_adm_c_api)

target_sources(
    mr_adm_c_api
    PRIVATE
        src/adm_c_api/adm_c_api.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/c_api.h
)

target_link_libraries(mr_adm_c_api PUBLIC MacinRender::ADMEngine)
set_target_properties(mr_adm_c_api PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)
mr_adm_core_apply_warnings(mr_adm_c_api)
mr_adm_core_apply_static_analysis(mr_adm_c_api)

# ── adm_c_api bundle (self-contained shared lib for GUI / P-Invoke) ────────────
# 默认 OFF,不影响常规构建。开启后产出单个自包含 libmradm_capi.dylib/.so/.dll,
# 把 c_api 与其全部静态依赖(engine → renderers → 第三方)whole-archive 进同一动态库,
# 供托管侧(Avalonia GUI)经 [LibraryImport("mradm_capi")] 直接加载。
# core 源码完全不动:这里只是多一个可选的链接目标。
option(MR_ADM_BUILD_CAPI_BUNDLE "Build self-contained libmradm_capi shared lib for GUI/P-Invoke" OFF)

if(MR_ADM_BUILD_CAPI_BUNDLE)
    # 直接把 C ABI 源文件编进 SHARED 目标,让 Windows 导出扫描能看到 adm_* 入口。
    add_library(mradm_capi_bundle SHARED src/adm_c_api/adm_c_api.cpp)
    set_target_properties(mradm_capi_bundle PROPERTIES
        OUTPUT_NAME mradm_capi
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    target_link_libraries(mradm_capi_bundle PRIVATE MacinRender::ADMEngine)
endif()

# ── CLI ───────────────────────────────────────────────────────────────────────

if(MR_ADM_CORE_BUILD_CLI)
    add_executable(mradm_exe)
    set_target_properties(mradm_exe PROPERTIES OUTPUT_NAME mradm)
    target_sources(mradm_exe
        PRIVATE
            src/adm_cli/apac_encode_command.cpp
            src/adm_cli/backends_command.cpp
            src/adm_cli/export_command.cpp
            src/adm_cli/formats_command.cpp
            src/adm_cli/inspect_command.cpp
            src/adm_cli/layouts_command.cpp
            src/adm_cli/main.cpp
            src/adm_cli/render_command.cpp
    )
    target_link_libraries(
        mradm_exe
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMIo
            MacinRender::ADMRenderEar
            MacinRender::ADMRenderVBAP
            MacinRender::ADMRenderHOA
            MacinRender::ADMRenderBinaural
            CLI11::CLI11
            spdlog::spdlog
    )
    target_include_directories(mradm_exe PRIVATE src/adm_audio)
    if(APPLE)
        target_link_libraries(mradm_exe PRIVATE MacinRender::ADMRenderApple)
    endif()
    mr_adm_core_apply_warnings(mradm_exe)
    mr_adm_core_apply_static_analysis(mradm_exe)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

if(MR_ADM_CORE_BUILD_TESTS)
    enable_testing()

    # core_smoke_test exercises RenderService (empty-input validation); link engine.
    add_executable(mr_adm_core_unit_tests tests/unit/core_smoke_test.cpp)
    target_link_libraries(mr_adm_core_unit_tests PRIVATE MacinRender::ADMEngine)
    mr_adm_core_apply_warnings(mr_adm_core_unit_tests)
    mr_adm_core_apply_static_analysis(mr_adm_core_unit_tests)
    add_test(NAME mr_adm_core_unit_tests COMMAND mr_adm_core_unit_tests)

    add_executable(mr_adm_io_smoke_tests tests/unit/adm_io_smoke_test.cpp)
    target_link_libraries(mr_adm_io_smoke_tests PRIVATE MacinRender::ADMIo)
    mr_adm_core_apply_warnings(mr_adm_io_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_io_smoke_tests)
    add_test(NAME mr_adm_io_smoke_tests COMMAND mr_adm_io_smoke_tests)

    # Contract test for the stable C ABI. Links only the C ABI target (consumer
    # view); libbw64/adm are pulled in solely to build the runtime fixture.
    add_executable(mr_adm_c_api_tests tests/unit/c_api_fixture_test.cpp)
    target_link_libraries(mr_adm_c_api_tests
        PRIVATE
            MacinRender::ADMCAPI
            libbw64
            adm
    )
    target_compile_definitions(mr_adm_c_api_tests PRIVATE MR_ADM_ENABLE_IAMF=$<BOOL:${MR_ADM_ENABLE_IAMF}>)
    mr_adm_core_apply_warnings(mr_adm_c_api_tests)
    mr_adm_core_apply_static_analysis(mr_adm_c_api_tests)
    add_test(NAME mr_adm_c_api_tests COMMAND mr_adm_c_api_tests)

    add_executable(mr_adm_c_api_header_smoke tests/unit/c_api_header_smoke.c)
    target_include_directories(mr_adm_c_api_header_smoke PRIVATE include)
    mr_adm_core_apply_warnings(mr_adm_c_api_header_smoke)
    mr_adm_core_apply_static_analysis(mr_adm_c_api_header_smoke)
    add_test(NAME mr_adm_c_api_header_smoke COMMAND mr_adm_c_api_header_smoke)

    add_executable(mr_adm_semantic_policy_tests tests/unit/semantic_policy_test.cpp)
    target_link_libraries(mr_adm_semantic_policy_tests PRIVATE MacinRender::ADMCore)
    mr_adm_core_apply_warnings(mr_adm_semantic_policy_tests)
    mr_adm_core_apply_static_analysis(mr_adm_semantic_policy_tests)
    add_test(NAME mr_adm_semantic_policy_tests COMMAND mr_adm_semantic_policy_tests)

    add_executable(mr_adm_io_fixture_tests tests/unit/adm_io_fixture_test.cpp)
    target_link_libraries(mr_adm_io_fixture_tests
        PRIVATE
            MacinRender::ADMIo
            libbw64
            adm
    )
    mr_adm_core_apply_warnings(mr_adm_io_fixture_tests)
    mr_adm_core_apply_static_analysis(mr_adm_io_fixture_tests)
    add_test(NAME mr_adm_io_fixture_tests COMMAND mr_adm_io_fixture_tests)

    add_executable(mr_adm_scene_export_tests tests/unit/scene_export_fixture_test.cpp)
    target_link_libraries(mr_adm_scene_export_tests
        PRIVATE
            MacinRender::ADMIo
            libbw64
            adm
    )
    mr_adm_core_apply_warnings(mr_adm_scene_export_tests)
    mr_adm_core_apply_static_analysis(mr_adm_scene_export_tests)
    add_test(NAME mr_adm_scene_export_tests COMMAND mr_adm_scene_export_tests)

    # Realtime engine scaffolding: SPSC ring + IRenderStream/device/factory abstractions,
    # exercised with a deterministic PatternStream (no real backend). Cross-platform.
    add_executable(mr_adm_realtime_tests tests/unit/realtime_stream_fixture_test.cpp)
    target_link_libraries(mr_adm_realtime_tests PRIVATE MacinRender::ADMRealtime Threads::Threads)
    mr_adm_core_apply_warnings(mr_adm_realtime_tests)
    mr_adm_core_apply_static_analysis(mr_adm_realtime_tests)
    add_test(NAME mr_adm_realtime_tests COMMAND mr_adm_realtime_tests)

    add_executable(mr_adm_vbap_smoke_tests tests/unit/vbap_smoke_test.cpp)
    target_link_libraries(mr_adm_vbap_smoke_tests
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMRenderVBAP
            MacinRender::ADMIo
            MacinRender::ADMAudio
            libbw64
            adm
    )
    mr_adm_core_apply_warnings(mr_adm_vbap_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_vbap_smoke_tests)
    add_test(NAME mr_adm_vbap_smoke_tests COMMAND mr_adm_vbap_smoke_tests)

    # Smoke test: non-existent file → io_error via full engine stack.
    add_executable(mr_adm_ear_smoke_tests tests/unit/ear_render_smoke_test.cpp)
    target_link_libraries(mr_adm_ear_smoke_tests PRIVATE MacinRender::ADMEngine)
    mr_adm_core_apply_warnings(mr_adm_ear_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_ear_smoke_tests)
    add_test(NAME mr_adm_ear_smoke_tests COMMAND mr_adm_ear_smoke_tests)

    # Fixture test: build a real BW64/ADM Objects file, render it, verify output.
    add_executable(mr_adm_ear_fixture_tests tests/unit/ear_render_fixture_test.cpp)
    target_link_libraries(mr_adm_ear_fixture_tests
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMRenderEar
            MacinRender::ADMRenderCommon
            MacinRender::ADMIo
            MacinRender::ADMAudio
            libbw64
            adm
    )
    mr_adm_core_apply_warnings(mr_adm_ear_fixture_tests)
    mr_adm_core_apply_static_analysis(mr_adm_ear_fixture_tests)
    add_test(NAME mr_adm_ear_fixture_tests COMMAND mr_adm_ear_fixture_tests)

    # Fixture test: True Peak limiting — renders a near-full-scale Objects file and
    # verifies the limiter brings output below the target dBTP.
    add_executable(mr_adm_peak_fixture_tests tests/unit/peak_limiter_fixture_test.cpp)
    target_link_libraries(mr_adm_peak_fixture_tests
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMPeak
            MacinRender::ADMAudio
            libbw64
            adm
    )
    mr_adm_core_apply_warnings(mr_adm_peak_fixture_tests)
    mr_adm_core_apply_static_analysis(mr_adm_peak_fixture_tests)
    add_test(NAME mr_adm_peak_fixture_tests COMMAND mr_adm_peak_fixture_tests)

    # Fixture test: Objects ADM → HOA3 (16ch, ACN/SN3D) encode.
    add_executable(mr_adm_hoa_encode_fixture_tests tests/unit/hoa_encode_fixture_test.cpp)
    target_link_libraries(mr_adm_hoa_encode_fixture_tests
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMRenderHOA
            MacinRender::ADMIo
            MacinRender::ADMAudio
            libbw64
            adm
    )
    mr_adm_core_apply_warnings(mr_adm_hoa_encode_fixture_tests)
    mr_adm_core_apply_static_analysis(mr_adm_hoa_encode_fixture_tests)
    add_test(NAME mr_adm_hoa_encode_fixture_tests COMMAND mr_adm_hoa_encode_fixture_tests)

    # Fixture test: HOA ADM (typeDefinition=HOA) → loudspeaker decode via EAR.
    add_executable(mr_adm_hoa_decode_fixture_tests tests/unit/hoa_decode_fixture_test.cpp)
    target_link_libraries(mr_adm_hoa_decode_fixture_tests
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMAudio
            libbw64
            adm
    )
    mr_adm_core_apply_warnings(mr_adm_hoa_decode_fixture_tests)
    mr_adm_core_apply_static_analysis(mr_adm_hoa_decode_fixture_tests)
    add_test(NAME mr_adm_hoa_decode_fixture_tests COMMAND mr_adm_hoa_decode_fixture_tests)

    # Fixture test: SAF KEMAR HRTF binaural renderer timing and fixed stereo output layout.
    # The interpolation invariant probe is its own test-only translation unit (binaural_test_probe.cpp),
    # compiled here and NOT into the production ADMRenderBinaural library.
    add_executable(mr_adm_binaural_fixture_tests
        tests/unit/binaural_render_fixture_test.cpp
        src/adm_render_binaural/binaural_test_probe.cpp)
    target_link_libraries(mr_adm_binaural_fixture_tests
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMRenderBinaural
            MacinRender::ADMIo
            MacinRender::ADMAudio
            libbw64
            adm
    )
    # Test-only access to the non-installed binaural internal/probe headers + SAF's float_complex.
    target_include_directories(mr_adm_binaural_fixture_tests SYSTEM PRIVATE
        "${saf_SOURCE_DIR}/modules/saf_utilities")
    target_include_directories(mr_adm_binaural_fixture_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/adm_render_binaural")
    mr_adm_core_apply_warnings(mr_adm_binaural_fixture_tests)
    mr_adm_core_apply_static_analysis(mr_adm_binaural_fixture_tests)
    add_test(NAME mr_adm_binaural_fixture_tests COMMAND mr_adm_binaural_fixture_tests)

    # Fixture test: integrated loudness normalization — loud sine wave → -23 LUFS target.
    add_executable(mr_adm_loudness_fixture_tests tests/unit/loudness_fixture_test.cpp)
    target_link_libraries(mr_adm_loudness_fixture_tests
        PRIVATE
            MacinRender::ADMLoudness
            MacinRender::ADMAudio
            ebur128
    )
    target_include_directories(mr_adm_loudness_fixture_tests SYSTEM PRIVATE "${EBUR128_INCLUDE_DIR}")
    mr_adm_core_apply_warnings(mr_adm_loudness_fixture_tests)
    mr_adm_core_apply_static_analysis(mr_adm_loudness_fixture_tests)
    add_test(NAME mr_adm_loudness_fixture_tests COMMAND mr_adm_loudness_fixture_tests)

    # Smoke test: FloatFlacWriter/Reader round-trip, apply_gain_to_file(.flac),
    # Vorbis Comment tags, single VC block, WAVEFORMATEXTENSIBLE_CHANNEL_MASK value.
    add_executable(mr_adm_flac_io_smoke_tests tests/unit/flac_io_smoke_test.cpp)
    target_link_libraries(mr_adm_flac_io_smoke_tests
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMAudio
            libbw64
            adm
            FLAC::FLAC
    )
    mr_adm_core_apply_warnings(mr_adm_flac_io_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_flac_io_smoke_tests)
    add_test(NAME mr_adm_flac_io_smoke_tests COMMAND mr_adm_flac_io_smoke_tests)

    add_executable(mr_adm_trim_file_smoke_tests tests/unit/trim_file_smoke_test.cpp)
    target_link_libraries(mr_adm_trim_file_smoke_tests PRIVATE MacinRender::ADMAudio)
    mr_adm_core_apply_warnings(mr_adm_trim_file_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_trim_file_smoke_tests)
    add_test(NAME mr_adm_trim_file_smoke_tests COMMAND mr_adm_trim_file_smoke_tests)

    # End-to-end output trim: --start/--end map to rendered frame ranges.
    add_executable(mr_adm_render_trim_fixture_tests tests/unit/render_trim_fixture_test.cpp)
    target_link_libraries(mr_adm_render_trim_fixture_tests
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMAudio
            libbw64
            adm
    )
    mr_adm_core_apply_warnings(mr_adm_render_trim_fixture_tests)
    mr_adm_core_apply_static_analysis(mr_adm_render_trim_fixture_tests)
    add_test(NAME mr_adm_render_trim_fixture_tests COMMAND mr_adm_render_trim_fixture_tests)

    add_executable(mr_adm_opus_mka_smoke_tests tests/unit/opus_mka_smoke_test.cpp)
    target_link_libraries(mr_adm_opus_mka_smoke_tests PRIVATE MacinRender::ADMAudio)
    mr_adm_core_apply_warnings(mr_adm_opus_mka_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_opus_mka_smoke_tests)
    add_test(NAME mr_adm_opus_mka_smoke_tests COMMAND mr_adm_opus_mka_smoke_tests)

    add_executable(mr_adm_iamf_smoke_tests tests/unit/iamf_smoke_test.cpp)
    target_link_libraries(mr_adm_iamf_smoke_tests PRIVATE MacinRender::ADMAudio)
    target_compile_definitions(mr_adm_iamf_smoke_tests PRIVATE MR_ADM_ENABLE_IAMF=$<BOOL:${MR_ADM_ENABLE_IAMF}>)
    mr_adm_core_apply_warnings(mr_adm_iamf_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_iamf_smoke_tests)
    add_test(NAME mr_adm_iamf_smoke_tests COMMAND mr_adm_iamf_smoke_tests)

    add_executable(mr_adm_apac_smoke_tests tests/unit/apac_smoke_test.cpp)
    target_link_libraries(mr_adm_apac_smoke_tests PRIVATE MacinRender::ADMAudio)
    # Pin the APAC stall-watchdog test to the real mradm subprocess helper (and
    # order the build after it). Skipped when the CLI is not built.
    if(TARGET mradm_exe)
        target_compile_definitions(mr_adm_apac_smoke_tests
            PRIVATE MRADM_EXE_PATH="$<TARGET_FILE:mradm_exe>")
        add_dependencies(mr_adm_apac_smoke_tests mradm_exe)
    endif()
    mr_adm_core_apply_warnings(mr_adm_apac_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_apac_smoke_tests)
    add_test(NAME mr_adm_apac_smoke_tests COMMAND mr_adm_apac_smoke_tests)

    if(APPLE)
        # Manual real-device check for the system-spatial monitor (AVSampleBufferAudioRenderer):
        # drives a live monitor with monitor_system_spatial=on. NOT a ctest — needs AirPods + a
        # human turning their head. Run by hand: mr_monitor_spatial_check <input> [layout] [secs].
        add_executable(mr_monitor_spatial_check tests/manual/system_spatial_monitor_check.cpp)
        target_link_libraries(mr_monitor_spatial_check PRIVATE MacinRender::ADMCAPI)
        mr_adm_core_apply_warnings(mr_monitor_spatial_check)

        add_executable(mr_adm_apple_smoke_tests tests/unit/apple_renderer_smoke_test.cpp)
        target_link_libraries(mr_adm_apple_smoke_tests
            PRIVATE
                MacinRender::ADMEngine
                MacinRender::ADMIo
                MacinRender::ADMRenderApple
                MacinRender::ADMRenderCommon
                MacinRender::ADMAudio
                libbw64
                adm
        )
        mr_adm_core_apply_warnings(mr_adm_apple_smoke_tests)
        mr_adm_core_apply_static_analysis(mr_adm_apple_smoke_tests)
        add_test(NAME mr_adm_apple_smoke_tests COMMAND mr_adm_apple_smoke_tests)
    endif()

    if(MR_ADM_CORE_BUILD_CLI)
        add_executable(mr_adm_cli_smoke_tests tests/unit/adm_cli_smoke_test.cpp)
        target_compile_definitions(mr_adm_cli_smoke_tests PRIVATE
            "MRADM_EXE_PATH=\"$<TARGET_FILE:mradm_exe>\""
        )
        target_link_libraries(mr_adm_cli_smoke_tests
            PRIVATE
                MacinRender::ADMIo
                libbw64
                adm
        )
        mr_adm_core_apply_warnings(mr_adm_cli_smoke_tests)
        mr_adm_core_apply_static_analysis(mr_adm_cli_smoke_tests)
        add_test(NAME mr_adm_cli_smoke_tests COMMAND mr_adm_cli_smoke_tests)
    endif()
endif()

# ── Install ───────────────────────────────────────────────────────────────────

# Only mr_adm_core is export-safe: its public deps (fmt, tl::expected) have
# installed CMake configs; it does not directly link FetchContent ADM targets.
install(
    TARGETS mr_adm_core
    EXPORT MacinRenderADMCoreTargets
    FILE_SET HEADERS
)

# mr_adm_c_api and all targets that depend on FetchContent ADM/rendering
# libraries (libbw64, libadm, libear, SAF) cannot be in the export set until those
# libraries provide proper installed CMake configs. Install binaries and headers
# without export; re-integrate post-M3.
install(
    TARGETS mr_adm_c_api mr_adm_io mr_adm_render_ear mr_adm_render_vbap mr_adm_render_hoa mr_adm_render_binaural mr_adm_peak mr_adm_loudness mr_adm_engine mr_adm_audio
    FILE_SET HEADERS
)

if(APPLE)
    install(
        TARGETS mr_adm_apple
        FILE_SET HEADERS
    )
endif()

install(
    EXPORT MacinRenderADMCoreTargets
    NAMESPACE MacinRender::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MacinRenderADMCore"
)

# Optional untracked local targets (developer scratch / experiments). The `local/`
# directory is git-ignored; this hook lets it reuse the main build (deps already
# resolved) without polluting any tracked target. No-op when the directory is absent.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/local/CMakeLists.txt")
    add_subdirectory(local)
endif()
