include(FetchContent)
include(MRDependencyRecord)

function(mr_adm_core_find_or_fetch package_name target_name)
    set_property(GLOBAL APPEND PROPERTY MR_ADM_DEPENDENCY_NAMES "${package_name}")
    set_property(GLOBAL PROPERTY "MR_ADM_DEPENDENCY_TARGET_${package_name}" "${target_name}")
    if(TARGET ${target_name})
        return()
    endif()

    if(MR_ADM_CORE_USE_INSTALLED_DEPS AND NOT package_name STREQUAL "FLAC" AND NOT package_name STREQUAL "Opus")
        if(package_name STREQUAL "Corrosion")
            find_package(Corrosion 0.6.1 EXACT CONFIG QUIET)
        else()
            find_package(${package_name} CONFIG QUIET)
        endif()
    endif()
    if(package_name STREQUAL "Corrosion" AND COMMAND corrosion_import_crate AND NOT TARGET Corrosion::Corrosion)
        add_library(mr_adm_corrosion INTERFACE)
        add_library(Corrosion::Corrosion ALIAS mr_adm_corrosion)
    endif()
    if(TARGET ${target_name})
        return()
    endif()

    if(NOT package_name STREQUAL "FLAC" AND NOT package_name STREQUAL "Opus" AND NOT MR_ADM_CORE_FETCH_DEPS)
        message(FATAL_ERROR "${package_name} was not found and MR_ADM_CORE_FETCH_DEPS is OFF")
    endif()

    if(package_name STREQUAL "Corrosion")
        FetchContent_Declare(Corrosion
            GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
            GIT_TAG v0.6.1
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(Corrosion)
        add_library(mr_adm_corrosion INTERFACE)
        add_library(Corrosion::Corrosion ALIAS mr_adm_corrosion)
        return()
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
        if(NOT TARGET dr_flac::dr_flac)
            add_library(mr_dr_flac INTERFACE)
            add_library(dr_flac::dr_flac ALIAS mr_dr_flac)
            target_include_directories(mr_dr_flac SYSTEM INTERFACE "${dr_libs_SOURCE_DIR}")
        endif()
        return()
    elseif(package_name STREQUAL "miniaudio")
        # Single-header realtime audio device I/O (mackron/miniaudio). One translation unit
        # defines MINIAUDIO_IMPLEMENTATION (src/adm_realtime/miniaudio_device.cpp); the
        # INTERFACE target just exposes the header + the platform audio link deps. On Linux
        # miniaudio dlopen()s ALSA/PulseAudio at runtime, so only dl/pthread/m are needed.
        FetchContent_Declare(
            miniaudio
            GIT_REPOSITORY https://github.com/mackron/miniaudio.git
            GIT_TAG 0.11.21
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(miniaudio)
        if(NOT miniaudio_POPULATED)
            cmake_policy(PUSH)
            if(POLICY CMP0169)
                cmake_policy(SET CMP0169 OLD)
            endif()
            FetchContent_Populate(miniaudio)
            cmake_policy(POP)
        endif()
        if(NOT TARGET miniaudio)
            add_library(mr_miniaudio INTERFACE)
            add_library(miniaudio ALIAS mr_miniaudio)
            target_include_directories(mr_miniaudio SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")
            if(APPLE)
                target_link_libraries(mr_miniaudio INTERFACE
                    "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox"
                    "-framework AudioUnit")
            elseif(UNIX)
                find_package(Threads REQUIRED)
                target_link_libraries(mr_miniaudio INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
            endif()
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
            # Linked statically into mradm and the shared library only: no install rules of its own
            # (CMake 3.28+; older versions still install the FLAC headers and archive).
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(FLAC)
        if(MSVC AND TARGET FLAC)
            # FLAC 1.5.0's win_utf8_io.c omits stdlib.h. MSVC's implicit-int
            # malloc declaration truncates pointers on x64; Debug decoder
            # diagnostics reach this path on corrupt input. Keep upstream bytes
            # intact and reject any remaining undeclared C functions.
            set_property(SOURCE "${flac_SOURCE_DIR}/src/share/win_utf8_io/win_utf8_io.c"
                TARGET_DIRECTORY FLAC APPEND PROPERTY COMPILE_OPTIONS /FIstdlib.h /we4013)
        endif()
        if(MR_ADM_STRICT_FP AND TARGET FLAC)
            # FLAC 1.5.0 adds these target-level options after CMAKE_C_FLAGS. Remove them
            # only in the controlled configuration; changing a cache preference would leak
            # into later baseline builds. Explicit ISA intrinsics remain subject to the scan.
            get_target_property(_mr_flac_options FLAC COMPILE_OPTIONS)
            if(_mr_flac_options)
                list(REMOVE_ITEM _mr_flac_options
                    -fassociative-math -fno-signed-zeros -fno-trapping-math -freciprocal-math "/fp:fast")
                set_property(TARGET FLAC PROPERTY COMPILE_OPTIONS "${_mr_flac_options}")
            endif()
        endif()
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
            GIT_TAG v1.5.2 GIT_SHALLOW TRUE
            # As FLAC: linked statically, nothing of its own installed (CMake 3.28+).
            EXCLUDE_FROM_ALL)
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
endfunction()

# fmt and nlohmann_json are test-only since 6.5 C (test SDK and tools).
if(MR_ADM_CORE_BUILD_TESTS)
    mr_adm_core_find_or_fetch(fmt fmt::fmt)
    mr_adm_core_find_or_fetch(nlohmann_json nlohmann_json::nlohmann_json)
endif()
mr_adm_core_find_or_fetch(dr_libs dr_flac::dr_flac)
mr_adm_core_find_or_fetch(FLAC FLAC::FLAC)
mr_adm_core_find_or_fetch(Opus Opus::opus)
mr_adm_core_find_or_fetch(miniaudio miniaudio)
mr_adm_core_write_dependency_record()
