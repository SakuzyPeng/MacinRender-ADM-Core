cmake_minimum_required(VERSION 3.24)

project(
    MacinRenderADMCore
    VERSION 0.1.0
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

add_library(mr_adm_core)
add_library(MacinRender::ADMCore ALIAS mr_adm_core)

target_sources(
    mr_adm_core
    PRIVATE
        src/adm_core/version.cpp
        src/adm_core/render_service.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
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

# ── adm_c_api ─────────────────────────────────────────────────────────────────

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

target_link_libraries(mr_adm_c_api PUBLIC MacinRender::ADMCore)
mr_adm_core_apply_warnings(mr_adm_c_api)
mr_adm_core_apply_static_analysis(mr_adm_c_api)

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

# ── CLI ───────────────────────────────────────────────────────────────────────

if(MR_ADM_CORE_BUILD_CLI)
    add_executable(adm_exe src/adm_cli/main.cpp)
    set_target_properties(adm_exe PROPERTIES OUTPUT_NAME adm)
    # ADMCore is a transitive dep of ADMIo; list it only once via ADMIo
    target_link_libraries(
        adm_exe
        PRIVATE
            MacinRender::ADMIo
            CLI11::CLI11
            spdlog::spdlog
    )
    mr_adm_core_apply_warnings(adm_exe)
    mr_adm_core_apply_static_analysis(adm_exe)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

if(MR_ADM_CORE_BUILD_TESTS)
    enable_testing()

    add_executable(mr_adm_core_unit_tests tests/unit/core_smoke_test.cpp)
    target_link_libraries(mr_adm_core_unit_tests PRIVATE MacinRender::ADMCore)
    mr_adm_core_apply_warnings(mr_adm_core_unit_tests)
    mr_adm_core_apply_static_analysis(mr_adm_core_unit_tests)
    add_test(NAME mr_adm_core_unit_tests COMMAND mr_adm_core_unit_tests)

    add_executable(mr_adm_io_smoke_tests tests/unit/adm_io_smoke_test.cpp)
    target_link_libraries(mr_adm_io_smoke_tests PRIVATE MacinRender::ADMIo)
    mr_adm_core_apply_warnings(mr_adm_io_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_io_smoke_tests)
    add_test(NAME mr_adm_io_smoke_tests COMMAND mr_adm_io_smoke_tests)

    # Fixture test uses libadm+libbw64 write APIs to build a real BW64/ADM file,
    # then verifies import_scene round-trips it correctly.
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
endif()

# ── Install ───────────────────────────────────────────────────────────────────

install(
    TARGETS mr_adm_core mr_adm_c_api
    EXPORT MacinRenderADMCoreTargets
    FILE_SET HEADERS
)

# mr_adm_io depends on FetchContent targets (libbw64, libadm) that are not
# IMPORTED and cannot be included in the export set yet.
# Re-add to the export once libbw64/libadm are properly find_package-installed (post-M3).
install(TARGETS mr_adm_io FILE_SET HEADERS)

install(
    EXPORT MacinRenderADMCoreTargets
    NAMESPACE MacinRender::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MacinRenderADMCore"
)
