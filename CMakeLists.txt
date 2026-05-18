cmake_minimum_required(VERSION 3.24)

project(
    MacinRenderADMCore
    VERSION 1.0.0
    DESCRIPTION "Cross-platform ADM rendering core for MacinRender"
    LANGUAGES C CXX
)

option(MR_ADM_CORE_BUILD_CLI "Build the adm command-line interface" ON)
option(MR_ADM_CORE_BUILD_TESTS "Build tests" ON)
option(MR_ADM_CORE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(MR_ADM_CORE_FETCH_DEPS "Fetch CLI/fmt/spdlog dependencies when not found" ON)
option(MR_ADM_CORE_ENABLE_CLANG_TIDY "Run clang-tidy as part of compilation when available" OFF)
option(MR_ADM_CORE_ENABLE_CPPCHECK "Run cppcheck as part of compilation when available" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(MRWarnings)
include(MRDependencies)
include(MRStaticAnalysis)

# ── adm_core ──────────────────────────────────────────────────────────────────
# Domain types, error handling, abstract IRenderer interface.
# No third-party ADM/renderer types leak into this target's public headers.

add_library(mr_adm_core)
add_library(MacinRender::ADMCore ALIAS mr_adm_core)

target_sources(
    mr_adm_core
    PRIVATE
        src/adm_core/version.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/capability.h
            include/adm/errors.h
            include/adm/logging.h
            include/adm/options.h
            include/adm/progress.h
            include/adm/render.h
            include/adm/version.h
)

target_include_directories(
    mr_adm_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(mr_adm_core PUBLIC tl::expected fmt::fmt)
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
    PRIVATE libbw64 adm ear MacinRender::ADMAudio
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
    PRIVATE libbw64 saf MacinRender::ADMAudio
)
target_include_directories(mr_adm_render_vbap
    SYSTEM PRIVATE
        "${saf_SOURCE_DIR}/modules/saf_vbap"
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
    PRIVATE libbw64 MacinRender::ADMAudio
)
mr_adm_core_apply_warnings(mr_adm_render_hoa)
mr_adm_core_apply_static_analysis(mr_adm_render_hoa)

# ── adm_audio ─────────────────────────────────────────────────────────────────
# Float32 WAV I/O via dr_wav. DR_WAV_IMPLEMENTATION defined here only.

add_library(mr_adm_audio)
add_library(MacinRender::ADMAudio ALIAS mr_adm_audio)

target_sources(
    mr_adm_audio
    PRIVATE
        src/adm_audio/audio_io.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/adm/audio_io.h
)

target_link_libraries(mr_adm_audio
    PUBLIC MacinRender::ADMCore
    PRIVATE dr_wav::dr_wav libbw64
)
mr_adm_core_apply_warnings(mr_adm_audio)
mr_adm_core_apply_static_analysis(mr_adm_audio)

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

# ── adm_engine ────────────────────────────────────────────────────────────────
# Orchestration layer: implements RenderService, selects backends.

add_library(mr_adm_engine)
add_library(MacinRender::ADMEngine ALIAS mr_adm_engine)

target_sources(
    mr_adm_engine
    PRIVATE
        src/adm_engine/render_service.cpp
)

target_link_libraries(mr_adm_engine
    PUBLIC MacinRender::ADMCore
    PRIVATE MacinRender::ADMIo MacinRender::ADMRenderEar MacinRender::ADMRenderVBAP MacinRender::ADMRenderHOA MacinRender::ADMPeak MacinRender::ADMAudio
)
mr_adm_core_apply_warnings(mr_adm_engine)
mr_adm_core_apply_static_analysis(mr_adm_engine)

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

# ── CLI ───────────────────────────────────────────────────────────────────────

if(MR_ADM_CORE_BUILD_CLI)
    add_executable(adm_exe src/adm_cli/main.cpp)
    set_target_properties(adm_exe PROPERTIES OUTPUT_NAME adm)
    target_link_libraries(
        adm_exe
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMIo
            MacinRender::ADMRenderEar
            MacinRender::ADMRenderVBAP
            MacinRender::ADMRenderHOA
            CLI11::CLI11
            spdlog::spdlog
    )
    mr_adm_core_apply_warnings(adm_exe)
    mr_adm_core_apply_static_analysis(adm_exe)
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

    add_executable(mr_adm_vbap_smoke_tests tests/unit/vbap_smoke_test.cpp)
    target_link_libraries(mr_adm_vbap_smoke_tests
        PRIVATE
            MacinRender::ADMEngine
            MacinRender::ADMRenderVBAP
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
            MacinRender::ADMAudio
            libbw64
            adm
    )
    mr_adm_core_apply_warnings(mr_adm_hoa_encode_fixture_tests)
    mr_adm_core_apply_static_analysis(mr_adm_hoa_encode_fixture_tests)
    add_test(NAME mr_adm_hoa_encode_fixture_tests COMMAND mr_adm_hoa_encode_fixture_tests)

    if(MR_ADM_CORE_BUILD_CLI)
        add_executable(mr_adm_cli_smoke_tests tests/unit/adm_cli_smoke_test.cpp)
        target_compile_definitions(mr_adm_cli_smoke_tests PRIVATE
            "ADM_EXE_PATH=\"$<TARGET_FILE:adm_exe>\""
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
    TARGETS mr_adm_c_api mr_adm_io mr_adm_render_ear mr_adm_render_vbap mr_adm_render_hoa mr_adm_peak mr_adm_engine mr_adm_audio
    FILE_SET HEADERS
)

install(
    EXPORT MacinRenderADMCoreTargets
    NAMESPACE MacinRender::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MacinRenderADMCore"
)
