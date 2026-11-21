# The C/C++ islands and the third-party C libraries (libFLAC, libopus, miniaudio and the
# optional IAMF bridge) that the Rust library links (6.6). Built standalone by the mradm-islands-sys
# build script (through the cmake crate), or as a subdirectory of the top-level test tree. Either way
# it writes mradm_native_link-<config>.txt: the archives in link order, then the platform libraries,
# one `kind:value` line each, which the build script turns into Cargo link instructions. The islands
# call Rust only through the services table (rust_services_ffi.h), so their archives follow the Rust
# library on any link line. They are always static: the C ABI is the only shared library.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.24)
    project(MacinRenderADMIslands DESCRIPTION "MacinRender ADM Core islands" LANGUAGES C CXX)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
    include(MRProject)
endif()

# ── adm_core ──────────────────────────────────────────────────────────────────
# The C++ domain headers the islands still use (errors, options, progress, logs, render records).
# Header-only.

add_library(mr_adm_core INTERFACE)
add_library(MacinRender::ADMCore ALIAS mr_adm_core)

# The C++ types the islands share (errors, options, progress and logging, capability and render
# records, the audio island API): private to the build since 6.5 C, not installed; only c_api.h
# stays in include/adm.
target_sources(
    mr_adm_core
    INTERFACE
        FILE_SET HEADERS
        BASE_DIRS adm_core
        FILES
            adm_core/adm/capability.h
            adm_core/adm/direct_speakers_matrix.h
            adm_core/adm/errors.h
            adm_core/adm/live_override.h
            adm_core/adm/logging.h
            adm_core/adm/options.h
            adm_core/adm/progress.h
            adm_core/adm/render.h
            adm_core/adm/room.h
            adm_core/adm/screen.h
)

target_include_directories(mr_adm_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/adm_core")
if(MSVC)
    # Shared headers and their consuming sources use UTF-8, regardless of the Windows code page.
    target_compile_options(mr_adm_core INTERFACE /utf-8)
endif()

# The Rust services table the islands call (6.6 A): Rust binds it (mradm_islands_bind) before it first
# calls an island. The islands call Rust only through it, so no island archive needs the Rust library.
add_library(mr_adm_rust_services STATIC adm_core/rust_services.cpp)
target_link_libraries(mr_adm_rust_services PUBLIC MacinRender::ADMCore)
mr_adm_core_apply_warnings(mr_adm_rust_services)
mr_adm_core_apply_static_analysis(mr_adm_rust_services)

# The wrappers of the Rust services the islands call (gain.h, meter.h, scene_math.h).
add_library(mr_adm_dsp INTERFACE)
add_library(MacinRender::ADMDsp ALIAS mr_adm_dsp)
target_include_directories(mr_adm_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/adm_dsp")
target_link_libraries(mr_adm_dsp INTERFACE mr_adm_rust_services)

# ── adm_render_common ─────────────────────────────────────────────────────────
# Backend-agnostic object preprocessing and block timeline helpers used by
# speaker renderers. No libear/SAF/Apple types may appear in this target.

add_library(mr_adm_render_common STATIC)
add_library(MacinRender::ADMRenderCommon ALIAS mr_adm_render_common)

target_sources(
    mr_adm_render_common
    PRIVATE
        adm_render_common/engine_records.cpp
        adm_render_common/render_common.cpp
        adm_render_common/speaker_layouts.cpp
)
target_include_directories(mr_adm_render_common PUBLIC adm_render_common)
target_link_libraries(mr_adm_render_common PUBLIC MacinRender::ADMCore Threads::Threads
    PRIVATE MacinRender::ADMAudio MacinRender::ADMDsp)
mr_adm_core_apply_warnings(mr_adm_render_common)
mr_adm_core_apply_static_analysis(mr_adm_render_common)

if(MR_ADM_CONSISTENCY_DIAGNOSTICS)
    target_compile_definitions(mr_adm_render_common PRIVATE MR_ADM_CONSISTENCY_DIAGNOSTICS=1)
endif()

# ── adm_audio ─────────────────────────────────────────────────────────────────
# Float32 WAV/CAF writers and APAC. WAVE sample I/O goes through Rust mradm-wav (wav_backend.h);
# FLAC, Opus MKA and IAMF output, the MP4 packager and the file metadata are in Rust (mradm-ffi,
# 7.2 A–C), which calls libFLAC, libopus and the IAMF bridge directly (the codec libraries below).

add_library(mr_adm_audio STATIC)
add_library(MacinRender::ADMAudio ALIAS mr_adm_audio)

target_sources(
    mr_adm_audio
    PRIVATE
        adm_audio/apac_io.cpp
        adm_audio/apac_worker_island.cpp
        adm_audio/audio_handles.cpp
        adm_audio/island.cpp
        adm_audio/wav_io.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS adm_core
        FILES
            adm_core/adm/audio_io.h
)

target_link_libraries(mr_adm_audio
    PUBLIC MacinRender::ADMCore
    PRIVATE MacinRender::ADMDsp
)
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
    add_library(mr_adm_apple STATIC)
    add_library(MacinRender::ADMRenderApple ALIAS mr_adm_apple)

    find_library(MR_ADM_AVFOUNDATION_FW AVFoundation REQUIRED)
    find_library(MR_ADM_COREMEDIA_FW CoreMedia REQUIRED)
    find_library(MR_ADM_FOUNDATION_FW Foundation REQUIRED)

    target_sources(
        mr_adm_apple
        PRIVATE
            adm_apple/spatial_mixer_renderer.cpp
            adm_apple/apple_island.cpp
            adm_apple/avsamplebuffer_device.mm
        PUBLIC
            FILE_SET HEADERS
            BASE_DIRS adm_core
            FILES
                adm_core/adm/render_apple.h
    )
    # ARC for the Objective-C++ AVSampleBufferAudioRenderer sink (manages the ObjC lifetimes).
    set_source_files_properties(adm_apple/avsamplebuffer_device.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

    target_link_libraries(mr_adm_apple
        PUBLIC MacinRender::ADMCore
        PRIVATE MacinRender::ADMDsp MacinRender::ADMAudio MacinRender::ADMRenderCommon
                ${MR_ADM_AUDIOTOOLBOX_FW} ${MR_ADM_AVFOUNDATION_FW} ${MR_ADM_COREMEDIA_FW} ${MR_ADM_FOUNDATION_FW}
    )
    # adm_apple consumes the realtime IAudioOutputDevice interface (third-party-free header) to
    # provide the AVSampleBufferAudioRenderer monitor sink. Include-only, no link dependency —
    # the device implements the pure interface; this keeps Apple frameworks confined here.
    target_include_directories(mr_adm_apple PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/adm_realtime)
    mr_adm_core_apply_warnings(mr_adm_apple)
    mr_adm_core_apply_static_analysis(mr_adm_apple)
endif()

# ── adm_windows ───────────────────────────────────────────────────────────────
# Windows-only system-spatial monitor sink (ISpatialAudioClient). Windows COM / SpatialAudio
# types are confined here; the factory returns the third-party-free IAudioOutputDevice.

if(WIN32)
    add_library(mr_adm_windows STATIC)
    add_library(MacinRender::ADMRenderWindows ALIAS mr_adm_windows)

    target_sources(mr_adm_windows PRIVATE
        adm_windows/spatialaudioclient_device.cpp
    )
    target_link_libraries(mr_adm_windows
        PUBLIC MacinRender::ADMCore
        PRIVATE Ole32 MacinRender::ADMRenderCommon
    )
    # adm_windows consumes the realtime IAudioOutputDevice interface (third-party-free header) to
    # provide the ISpatialAudioClient monitor sink. Include-only, no link dependency — the device
    # implements the pure interface; this keeps Windows COM headers confined here.
    target_include_directories(mr_adm_windows PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/adm_realtime)
    mr_adm_core_apply_warnings(mr_adm_windows)
    mr_adm_core_apply_static_analysis(mr_adm_windows)
endif()

# ── islands ───────────────────────────────────────────────────────────────────
# The C islands the Rust library calls by name besides the audio and Apple islands: the platform
# facts (mradm_platform_islands) and the output devices (mradm_device_islands: miniaudio here, the
# system-spatial sinks in adm_apple / adm_windows).

add_library(mr_adm_islands STATIC)
add_library(MacinRender::ADMIslands ALIAS mr_adm_islands)
target_sources(mr_adm_islands PRIVATE
    adm_engine/platform_island.cpp
    adm_realtime/device_island.cpp
    adm_realtime/miniaudio_device.cpp
)
target_include_directories(mr_adm_islands PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/adm_realtime")
# miniaudio is PRIVATE: its types never cross the IAudioOutputDevice boundary (ADR 0003).
target_link_libraries(mr_adm_islands
    PUBLIC MacinRender::ADMCore
    PRIVATE Threads::Threads miniaudio MacinRender::ADMAudio MacinRender::ADMDsp MacinRender::ADMRenderCommon
)
if(APPLE)
    target_link_libraries(mr_adm_islands PRIVATE MacinRender::ADMRenderApple)
endif()
if(WIN32)
    # The ISpatialAudioClient sink and its layouts (make_spatialaudioclient_device /
    # system_spatial_layouts) live in mr_adm_windows.
    target_link_libraries(mr_adm_islands PRIVATE MacinRender::ADMRenderWindows)
endif()
mr_adm_core_apply_warnings(mr_adm_islands)
mr_adm_core_apply_static_analysis(mr_adm_islands)

if(MR_ADM_CONSISTENCY_DIAGNOSTICS)
    foreach(_mr_trace_target IN ITEMS mr_adm_audio mr_adm_render_common)
        target_compile_definitions(${_mr_trace_target} PRIVATE MR_ADM_CONSISTENCY_DIAGNOSTICS=1)
    endforeach()
endif()

# ── C ABI progress trampolines ────────────────────────────────────────────────
# The caller's progress callbacks run from C++ so that an exception a C++ caller throws from one ends
# that render with ADM_ERROR_INTERNAL instead of unwinding into Rust (mradm-capi calls them by name).

add_library(mr_adm_capi_progress STATIC adm_c_api/adm_c_api.cpp)
target_link_libraries(mr_adm_capi_progress PRIVATE MacinRender::ADMCore)
mr_adm_core_apply_warnings(mr_adm_capi_progress)
mr_adm_core_apply_static_analysis(mr_adm_capi_progress)

# The capture or loopback input of the manual room device probe (mradm-ffi example
# room_scene_benchmark), over the islands' miniaudio implementation. Only the probe references it.
add_library(mr_adm_probe_capture STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../tests/manual/capture_tap.c")
target_link_libraries(mr_adm_probe_capture PRIVATE miniaudio)

# ── Link manifest ─────────────────────────────────────────────────────────────
# Every archive after the archives that call it (single-pass linkers), the platform libraries last.
# mr_adm_native carries the same order for the C++ tests of the top-level tree.

set(MR_ADM_NATIVE_LIBRARIES mr_adm_capi_progress mr_adm_probe_capture mr_adm_islands)
if(APPLE)
    list(APPEND MR_ADM_NATIVE_LIBRARIES mr_adm_apple)
elseif(WIN32)
    list(APPEND MR_ADM_NATIVE_LIBRARIES mr_adm_windows)
endif()
list(APPEND MR_ADM_NATIVE_LIBRARIES mr_adm_render_common mr_adm_audio mr_adm_rust_services)
set(_mr_codec_libraries Opus::opus FLAC::FLAC)
set(_mr_native_lines)
if(MR_ADM_ENABLE_IAMF)
    # Rust calls the bridge (mradm-ffi `iamf`, cfg(mradm_iamf) when the manifest names the feature).
    list(APPEND _mr_codec_libraries MacinRender::IamfAomBridge)
    string(APPEND _mr_native_lines "feature:iamf\n")
endif()
foreach(_mr_library IN LISTS MR_ADM_NATIVE_LIBRARIES)
    string(APPEND _mr_native_lines "file:$<TARGET_LINKER_FILE:${_mr_library}>\n")
endforeach()
include(MRNativeLink)
mr_adm_codec_link_manifest(_mr_codec_lines ${_mr_codec_libraries})
string(APPEND _mr_native_lines "${_mr_codec_lines}")
if(APPLE)
    foreach(_mr_framework AudioToolbox AudioUnit CoreAudio CoreFoundation AVFoundation CoreMedia Foundation)
        string(APPEND _mr_native_lines "framework:${_mr_framework}\n")
    endforeach()
    string(APPEND _mr_native_lines "system:c++\nsystem:objc\n")
elseif(WIN32)
    string(APPEND _mr_native_lines "system:Ole32\n")
    if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
        string(APPEND _mr_native_lines "msvc-runtime:${CMAKE_MSVC_RUNTIME_LIBRARY}\n")
    endif()
else()
    string(APPEND _mr_native_lines "system:stdc++\nsystem:pthread\nsystem:dl\nsystem:m\n")
endif()
set(MR_ADM_NATIVE_LINK_FILE "${CMAKE_CURRENT_BINARY_DIR}/mradm_native_link-$<CONFIG>.txt")
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(MR_ADM_NATIVE_LINK_FILE "${MR_ADM_NATIVE_LINK_FILE}" PARENT_SCOPE)
endif()
file(GENERATE OUTPUT "${MR_ADM_NATIVE_LINK_FILE}" CONTENT "${_mr_native_lines}")

add_library(mr_adm_native INTERFACE)
target_link_libraries(mr_adm_native INTERFACE ${MR_ADM_NATIVE_LIBRARIES} ${_mr_codec_libraries})
# The target the build script builds, and the one every Cargo invocation of the top-level tree depends on.
# Cargo links the codecs directly, so their targets must be built before Rust uses the manifest.
add_custom_target(mradm_native ALL)
add_dependencies(mradm_native ${MR_ADM_NATIVE_LIBRARIES} ${_mr_codec_libraries})
