include(FetchContent)

function(mr_adm_core_find_or_fetch package_name target_name)
    if(TARGET ${target_name})
        return()
    endif()

    if(MR_ADM_CORE_USE_INSTALLED_DEPS AND NOT package_name STREQUAL "FLAC" AND NOT package_name STREQUAL "Opus")
        find_package(${package_name} CONFIG QUIET)
    endif()
    if(TARGET ${target_name})
        return()
    endif()

    if(NOT package_name STREQUAL "FLAC" AND NOT package_name STREQUAL "Opus" AND NOT MR_ADM_CORE_FETCH_DEPS)
        message(FATAL_ERROR "${package_name} was not found and MR_ADM_CORE_FETCH_DEPS is OFF")
    endif()

    if(package_name STREQUAL "CLI11")
        FetchContent_Declare(
            CLI11
            GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
            GIT_TAG v2.5.0
            GIT_SHALLOW TRUE
        )
    elseif(package_name STREQUAL "nlohmann_json")
        set(JSON_BuildTests OFF CACHE INTERNAL "")
        FetchContent_Declare(
            nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.12.0
            GIT_SHALLOW TRUE
        )
    elseif(package_name STREQUAL "fmt")
        FetchContent_Declare(
            fmt
            GIT_REPOSITORY https://github.com/fmtlib/fmt.git
            GIT_TAG 11.2.0
            GIT_SHALLOW TRUE
        )
    elseif(package_name STREQUAL "spdlog")
        set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use external fmt with spdlog" FORCE)
        FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.15.3
            GIT_SHALLOW TRUE
        )
    elseif(package_name STREQUAL "tl-expected")
        set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            expected
            GIT_REPOSITORY https://github.com/TartanLlama/expected.git
            GIT_TAG v1.1.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(expected)
        # FetchContent creates target 'expected'; installed config exports 'tl::expected'
        if(NOT TARGET tl::expected)
            add_library(tl::expected ALIAS expected)
        endif()
        return()
    elseif(package_name STREQUAL "libebur128")
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(_mr_adm_core_restore_shared_ebur TRUE)
        if(DEFINED BUILD_SHARED_LIBS)
            set(_mr_adm_core_had_shared_ebur TRUE)
            set(_mr_adm_core_saved_shared_ebur "${BUILD_SHARED_LIBS}")
        else()
            set(_mr_adm_core_had_shared_ebur FALSE)
        endif()
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            libebur128
            GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
            GIT_TAG v1.2.6
            GIT_SHALLOW TRUE
        )
        # libebur128 v1.2.6 declares cmake_minimum_required(VERSION 2.x), which
        # CMake 4.0 rejects without this policy fallback.
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
        FetchContent_GetProperties(libebur128)
        if(NOT libebur128_POPULATED)
            cmake_policy(PUSH)
            if(POLICY CMP0169)
                cmake_policy(SET CMP0169 OLD)
            endif()
            FetchContent_Populate(libebur128)
            cmake_policy(POP)
            add_subdirectory("${libebur128_SOURCE_DIR}" "${libebur128_BINARY_DIR}" EXCLUDE_FROM_ALL)
        endif()
        if(_mr_adm_core_had_shared_ebur)
            set(BUILD_SHARED_LIBS "${_mr_adm_core_saved_shared_ebur}" CACHE BOOL "" FORCE)
        else()
            unset(BUILD_SHARED_LIBS CACHE)
            unset(BUILD_SHARED_LIBS)
        endif()
        return()
    elseif(package_name STREQUAL "dr_libs")
        FetchContent_Declare(
            dr_libs
            GIT_REPOSITORY https://github.com/mackron/dr_libs.git
            GIT_TAG 47a4f08e777faddf59a8955c4ea84f69f41020d5
        )
        FetchContent_GetProperties(dr_libs)
        if(NOT dr_libs_POPULATED)
            cmake_policy(PUSH)
            if(POLICY CMP0169)
                cmake_policy(SET CMP0169 OLD)
            endif()
            FetchContent_Populate(dr_libs)
            cmake_policy(POP)
        endif()
        if(NOT TARGET dr_wav::dr_wav)
            add_library(mr_dr_wav INTERFACE)
            add_library(dr_wav::dr_wav ALIAS mr_dr_wav)
            target_include_directories(mr_dr_wav SYSTEM INTERFACE "${dr_libs_SOURCE_DIR}")
        endif()
        if(NOT TARGET dr_flac::dr_flac)
            add_library(mr_dr_flac INTERFACE)
            add_library(dr_flac::dr_flac ALIAS mr_dr_flac)
            target_include_directories(mr_dr_flac SYSTEM INTERFACE "${dr_libs_SOURCE_DIR}")
        endif()
        return()
    elseif(package_name STREQUAL "FLAC")
        string(TOUPPER "${MR_ADM_FLAC_PROVIDER}" _mr_flac_requested_provider)
        if(MR_ADM_USE_SYSTEM_FLAC)
            set(_mr_flac_requested_provider SYSTEM)
        endif()
        if(NOT _mr_flac_requested_provider MATCHES "^(AUTO|SYSTEM|VENDORED)$")
            message(FATAL_ERROR "MR_ADM_FLAC_PROVIDER must be AUTO, SYSTEM, or VENDORED")
        endif()

        set(_mr_flac_provider "${_mr_flac_requested_provider}")
        if(_mr_flac_provider STREQUAL "AUTO")
            if(CMAKE_CONFIGURATION_TYPES OR MR_ADM_FLAC_AUTO_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
                set(_mr_flac_provider VENDORED)
            else()
                set(_mr_flac_provider SYSTEM)
            endif()
            message(STATUS "libFLAC provider AUTO resolved to ${_mr_flac_provider}")
        endif()

        if(_mr_flac_provider STREQUAL "SYSTEM")
            find_package(FLAC CONFIG QUIET)
            if(TARGET FLAC::FLAC)
                message(STATUS "Using system libFLAC via find_package")
                return()
            endif()

            # libFLAC does not ship CMake config files in typical brew/system installs.
            # Search common prefix paths before falling back in AUTO developer builds.
            find_library(MR_FLAC_LIB NAMES FLAC
                HINTS /opt/homebrew/lib /usr/local/lib /usr/lib
                NO_CACHE)
            find_path(MR_FLAC_INCLUDE NAMES FLAC/stream_encoder.h
                HINTS /opt/homebrew/include /usr/local/include /usr/include
                NO_CACHE)
            if(MR_FLAC_LIB AND MR_FLAC_INCLUDE)
                add_library(FLAC::FLAC UNKNOWN IMPORTED GLOBAL)
                set_target_properties(FLAC::FLAC PROPERTIES
                    IMPORTED_LOCATION "${MR_FLAC_LIB}"
                    INTERFACE_INCLUDE_DIRECTORIES "${MR_FLAC_INCLUDE}"
                )
                message(STATUS "Using system libFLAC: ${MR_FLAC_LIB}")
                return()
            endif()

            if(NOT _mr_flac_requested_provider STREQUAL "AUTO")
                message(FATAL_ERROR "System libFLAC was requested but not found")
            endif()
            if(NOT MR_ADM_CORE_FETCH_DEPS)
                message(FATAL_ERROR "libFLAC was not found and MR_ADM_CORE_FETCH_DEPS is OFF")
            endif()
            set(_mr_flac_provider VENDORED)
            message(STATUS "System libFLAC not found; falling back to vendored static libFLAC")
        endif()

        if(NOT MR_ADM_CORE_FETCH_DEPS)
            message(FATAL_ERROR "Vendored libFLAC requires MR_ADM_CORE_FETCH_DEPS=ON")
        endif()

        if(DEFINED BUILD_SHARED_LIBS)
            set(_mr_adm_core_had_shared_flac TRUE)
            set(_mr_adm_core_saved_shared_flac "${BUILD_SHARED_LIBS}")
        else()
            set(_mr_adm_core_had_shared_flac FALSE)
        endif()
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        set(WITH_OGG OFF CACHE BOOL "" FORCE)
        set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            FLAC
            GIT_REPOSITORY https://github.com/xiph/flac.git
            GIT_TAG 1.5.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(FLAC)
        if(_mr_adm_core_had_shared_flac)
            set(BUILD_SHARED_LIBS "${_mr_adm_core_saved_shared_flac}" CACHE BOOL "" FORCE)
        else()
            unset(BUILD_SHARED_LIBS CACHE)
            unset(BUILD_SHARED_LIBS)
        endif()
        if(TARGET FLAC AND NOT TARGET FLAC::FLAC)
            add_library(FLAC::FLAC ALIAS FLAC)
        endif()
        if(NOT TARGET FLAC::FLAC)
            message(FATAL_ERROR "Vendored libFLAC did not provide FLAC::FLAC")
        endif()
        message(STATUS "Using vendored static libFLAC")
        return()
    elseif(package_name STREQUAL "libbw64")
        set(EXAMPLES OFF CACHE BOOL "" FORCE)
        set(UNIT_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            libbw64
            GIT_REPOSITORY https://github.com/ebu/libbw64.git
            GIT_TAG 0.10.0
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(libbw64)
        if(NOT libbw64_POPULATED)
            cmake_policy(PUSH)
            if(POLICY CMP0169)
                cmake_policy(SET CMP0169 OLD)
            endif()
            FetchContent_Populate(libbw64)
            cmake_policy(POP)
            set(_mr_libbw64_cmake "${libbw64_SOURCE_DIR}/src/CMakeLists.txt")
            file(READ "${_mr_libbw64_cmake}" _mr_libbw64_src_cmake)
            string(REPLACE "\${CMAKE_SOURCE_DIR}/config/libbw64Config.cmake.in"
                           "\${PROJECT_SOURCE_DIR}/config/libbw64Config.cmake.in"
                           _mr_libbw64_src_cmake "${_mr_libbw64_src_cmake}")
            string(REPLACE "\${CMAKE_BINARY_DIR}/libbw64Config"
                           "\${PROJECT_BINARY_DIR}/libbw64Config"
                           _mr_libbw64_src_cmake "${_mr_libbw64_src_cmake}")
            file(WRITE "${_mr_libbw64_cmake}" "${_mr_libbw64_src_cmake}")
            add_subdirectory("${libbw64_SOURCE_DIR}" "${libbw64_BINARY_DIR}" EXCLUDE_FROM_ALL)
        endif()
        return()
    elseif(package_name STREQUAL "libadm")
        set(ADM_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(ADM_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            libadm
            GIT_REPOSITORY https://github.com/ebu/libadm.git
            GIT_TAG 0.14.0
            GIT_SHALLOW TRUE
        )
    elseif(package_name STREQUAL "libear")
        # libear has no official release tags; pin to a known-good commit (2026-04-09).
        # libear vendors Eigen and xsimd by default; only Boost headers are required externally.
        set(EAR_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(EAR_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(EAR_USE_INTERNAL_EIGEN ON CACHE BOOL "" FORCE)
        set(EAR_USE_INTERNAL_XSIMD ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
            libear
            GIT_REPOSITORY https://github.com/ebu/libear.git
            GIT_TAG 2db69f8fcea0bc5db8a78e14a9c2ae6ed4283c15
            GIT_SHALLOW FALSE
        )
    elseif(package_name STREQUAL "Spatial_Audio_Framework")
        # SAF exposes target 'saf'. Keep GPL modules off; SOFA uses libmysofa/zlib
        # when MR_ADM_ENABLE_SOFA is ON, and NetCDF stays disabled.
        set(_mr_adm_core_restore_build_shared_libs TRUE)
        if(DEFINED BUILD_SHARED_LIBS)
            set(_mr_adm_core_had_build_shared_libs TRUE)
            set(_mr_adm_core_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
        else()
            set(_mr_adm_core_had_build_shared_libs FALSE)
        endif()
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_SOFA_READER_MODULE ${MR_ADM_ENABLE_SOFA} CACHE BOOL "" FORCE)
        set(SAF_ENABLE_TRACKER_MODULE OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_HADES_MODULE OFF CACHE BOOL "" FORCE)
        set(SAF_USE_INTEL_IPP OFF CACHE BOOL "" FORCE)
        set(SAF_USE_FFTW OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_SIMD OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_NETCDF OFF CACHE BOOL "" FORCE)
        set(SAF_USE_FAST_MATH_FLAG OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_FAST_MATH_FLAG OFF CACHE BOOL "" FORCE)
        if(NOT DEFINED SAF_PERFORMANCE_LIB)
            if(APPLE)
                set(SAF_PERFORMANCE_LIB "SAF_USE_APPLE_ACCELERATE_ILP64" CACHE STRING "" FORCE)
            else()
                set(SAF_PERFORMANCE_LIB "SAF_USE_OPEN_BLAS_AND_LAPACKE" CACHE STRING "" FORCE)
            endif()
        endif()
        FetchContent_Declare(
            Spatial_Audio_Framework
            GIT_REPOSITORY https://github.com/leomccormack/Spatial_Audio_Framework.git
            GIT_TAG v1.3.4
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR framework
        )
    elseif(package_name STREQUAL "Opus")
        string(TOUPPER "${MR_ADM_OPUS_PROVIDER}" _mr_opus_requested_provider)
        if(MR_ADM_USE_SYSTEM_OPUS)
            set(_mr_opus_requested_provider SYSTEM)
        endif()
        if(NOT _mr_opus_requested_provider MATCHES "^(AUTO|SYSTEM|VENDORED)$")
            message(FATAL_ERROR "MR_ADM_OPUS_PROVIDER must be AUTO, SYSTEM, or VENDORED")
        endif()

        set(_mr_opus_provider "${_mr_opus_requested_provider}")
        if(_mr_opus_provider STREQUAL "AUTO")
            if(CMAKE_CONFIGURATION_TYPES OR MR_ADM_OPUS_AUTO_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
                set(_mr_opus_provider VENDORED)
            else()
                set(_mr_opus_provider SYSTEM)
            endif()
            message(STATUS "libopus provider AUTO resolved to ${_mr_opus_provider}")
        endif()

        if(_mr_opus_provider STREQUAL "SYSTEM")
            find_package(Opus CONFIG QUIET)
            if(TARGET Opus::opus)
                message(STATUS "Using system libopus via find_package")
                return()
            endif()

            find_library(MR_OPUS_LIB NAMES opus
                HINTS /opt/homebrew/lib /usr/local/lib /usr/lib
                NO_CACHE)
            find_path(MR_OPUS_INCLUDE NAMES opus_multistream.h
                HINTS /opt/homebrew/include /usr/local/include /usr/include
                PATH_SUFFIXES opus
                NO_CACHE)
            if(MR_OPUS_LIB AND MR_OPUS_INCLUDE)
                add_library(Opus::opus UNKNOWN IMPORTED GLOBAL)
                set_target_properties(Opus::opus PROPERTIES
                    IMPORTED_LOCATION "${MR_OPUS_LIB}"
                    INTERFACE_INCLUDE_DIRECTORIES "${MR_OPUS_INCLUDE}"
                )
                message(STATUS "Using system libopus: ${MR_OPUS_LIB}")
                return()
            endif()

            if(NOT _mr_opus_requested_provider STREQUAL "AUTO")
                message(FATAL_ERROR "System libopus was requested but not found")
            endif()
            if(NOT MR_ADM_CORE_FETCH_DEPS)
                message(FATAL_ERROR "libopus was not found and MR_ADM_CORE_FETCH_DEPS is OFF")
            endif()
            set(_mr_opus_provider VENDORED)
            message(STATUS "System libopus not found; falling back to vendored static libopus")
        endif()

        if(NOT MR_ADM_CORE_FETCH_DEPS)
            message(FATAL_ERROR "Vendored libopus requires MR_ADM_CORE_FETCH_DEPS=ON")
        endif()

        if(DEFINED BUILD_SHARED_LIBS)
            set(_mr_adm_core_had_shared_opus TRUE)
            set(_mr_adm_core_saved_shared_opus "${BUILD_SHARED_LIBS}")
        else()
            set(_mr_adm_core_had_shared_opus FALSE)
        endif()
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
        FetchContent_Declare(Opus
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG v1.5.2 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(Opus)
        if(_mr_adm_core_had_shared_opus)
            set(BUILD_SHARED_LIBS "${_mr_adm_core_saved_shared_opus}" CACHE BOOL "" FORCE)
        else()
            unset(BUILD_SHARED_LIBS CACHE)
            unset(BUILD_SHARED_LIBS)
        endif()
        if(TARGET opus AND NOT TARGET Opus::opus)
            add_library(Opus::opus ALIAS opus)
        endif()
        if(NOT TARGET Opus::opus)
            message(FATAL_ERROR "Vendored libopus did not provide Opus::opus")
        endif()
        message(STATUS "Using vendored static libopus")
        return()
    else()
        message(FATAL_ERROR "Unknown dependency: ${package_name}")
    endif()

    FetchContent_MakeAvailable(${package_name})
    if(_mr_adm_core_restore_build_shared_libs)
        if(_mr_adm_core_had_build_shared_libs)
            set(BUILD_SHARED_LIBS "${_mr_adm_core_saved_build_shared_libs}" CACHE BOOL "" FORCE)
        else()
            unset(BUILD_SHARED_LIBS CACHE)
            unset(BUILD_SHARED_LIBS)
        endif()
    endif()
    if(package_name STREQUAL "Spatial_Audio_Framework" AND TARGET saf)
        target_compile_options(saf PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-deprecated-declarations>)
    endif()
endfunction()

mr_adm_core_find_or_fetch(fmt fmt::fmt)
mr_adm_core_find_or_fetch(spdlog spdlog::spdlog)
mr_adm_core_find_or_fetch(CLI11 CLI11::CLI11)
mr_adm_core_find_or_fetch(nlohmann_json nlohmann_json::nlohmann_json)
mr_adm_core_find_or_fetch(tl-expected tl::expected)
mr_adm_core_find_or_fetch(libebur128 ebur128)
mr_adm_core_find_or_fetch(dr_libs dr_wav::dr_wav)
mr_adm_core_find_or_fetch(FLAC FLAC::FLAC)
mr_adm_core_find_or_fetch(libbw64 libbw64)
mr_adm_core_find_or_fetch(libadm adm)
mr_adm_core_find_or_fetch(libear ear)
mr_adm_core_find_or_fetch(Spatial_Audio_Framework saf)
mr_adm_core_find_or_fetch(Opus Opus::opus)
