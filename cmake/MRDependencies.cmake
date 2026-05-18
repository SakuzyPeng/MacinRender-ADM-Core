include(FetchContent)

function(mr_adm_core_find_or_fetch package_name target_name)
    find_package(${package_name} CONFIG QUIET)
    if(TARGET ${target_name})
        return()
    endif()

    if(NOT MR_ADM_CORE_FETCH_DEPS)
        message(FATAL_ERROR "${package_name} was not found and MR_ADM_CORE_FETCH_DEPS is OFF")
    endif()

    if(package_name STREQUAL "CLI11")
        FetchContent_Declare(
            CLI11
            GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
            GIT_TAG v2.5.0
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
    elseif(package_name STREQUAL "libbw64")
        set(BW64_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            libbw64
            GIT_REPOSITORY https://github.com/ebu/libbw64.git
            GIT_TAG 0.10.0
            GIT_SHALLOW TRUE
        )
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
        # SAF exposes target 'saf'. Keep optional/GPL modules off; M7 only needs saf_vbap.
        set(_mr_adm_core_restore_build_shared_libs TRUE)
        if(DEFINED BUILD_SHARED_LIBS)
            set(_mr_adm_core_had_build_shared_libs TRUE)
            set(_mr_adm_core_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
        else()
            set(_mr_adm_core_had_build_shared_libs FALSE)
        endif()
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_SOFA_READER_MODULE OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_TRACKER_MODULE OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_HADES_MODULE OFF CACHE BOOL "" FORCE)
        set(SAF_USE_INTEL_IPP OFF CACHE BOOL "" FORCE)
        set(SAF_USE_FFTW OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_SIMD OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_NETCDF OFF CACHE BOOL "" FORCE)
        set(SAF_USE_FAST_MATH_FLAG OFF CACHE BOOL "" FORCE)
        set(SAF_ENABLE_FAST_MATH_FLAG OFF CACHE BOOL "" FORCE)
        if(APPLE AND NOT DEFINED SAF_PERFORMANCE_LIB)
            set(SAF_PERFORMANCE_LIB "SAF_USE_APPLE_ACCELERATE_ILP64" CACHE STRING "" FORCE)
        endif()
        FetchContent_Declare(
            Spatial_Audio_Framework
            GIT_REPOSITORY https://github.com/leomccormack/Spatial_Audio_Framework.git
            GIT_TAG v1.3.4
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR framework
        )
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
mr_adm_core_find_or_fetch(tl-expected tl::expected)
mr_adm_core_find_or_fetch(libebur128 ebur128)
mr_adm_core_find_or_fetch(libbw64 libbw64)
mr_adm_core_find_or_fetch(libadm adm)
mr_adm_core_find_or_fetch(libear ear)
mr_adm_core_find_or_fetch(Spatial_Audio_Framework saf)
