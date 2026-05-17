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
    else()
        message(FATAL_ERROR "Unknown dependency: ${package_name}")
    endif()

    FetchContent_MakeAvailable(${package_name})
endfunction()

mr_adm_core_find_or_fetch(fmt fmt::fmt)
mr_adm_core_find_or_fetch(spdlog spdlog::spdlog)
mr_adm_core_find_or_fetch(CLI11 CLI11::CLI11)
mr_adm_core_find_or_fetch(tl-expected tl::expected)
mr_adm_core_find_or_fetch(libbw64 libbw64)
mr_adm_core_find_or_fetch(libadm adm)
mr_adm_core_find_or_fetch(libear ear)
