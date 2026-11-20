# The options and compile settings the islands' project (src/CMakeLists.txt, built by the
# mradm-islands-sys build script) shares with the top-level test tree, which adds it as a
# subdirectory (6.6). Included once, before any target is created.
include_guard(GLOBAL)

option(MR_ADM_CONSISTENCY_DIAGNOSTICS "Enable private numerical checkpoints and worker controls (measurement only)" OFF)
option(MR_ADM_CORE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(MR_ADM_CORE_FETCH_DEPS "Fetch the C/C++ dependencies when not found" ON)
option(MR_ADM_CORE_USE_INSTALLED_DEPS "Use installed CMake package configs before FetchContent dependencies" ON)
option(MR_ADM_CORE_ENABLE_CLANG_TIDY "Run clang-tidy as part of compilation when available" OFF)
option(MR_ADM_CORE_ENABLE_CPPCHECK "Run cppcheck as part of compilation when available" OFF)
option(MR_ADM_ENABLE_SOFA "Enable user SOFA HRIR loading for the binaural renderer" ON)
option(MR_ADM_ENABLE_ROOM_CUDA "Build the optional Windows ROOM CUDA runtime (opt-in only)" OFF)
option(MR_ADM_ENABLE_IAMF "Enable IAMF encoding via the official AOM iamf-tools prebuilt bridge" OFF)

# The Rust cdylib (libmradm_capi) links the islands and their static dependencies. On ELF platforms
# every object in those archives must be built as PIC, so establish the default before FetchContent
# and any island target is created.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    # Standalone builds use Rust's default retail CRT, including in Debug. An embedding project
    # can select MultiThreaded (/MT) for its executable and all of the native archives.
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
endif()

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
if(APPLE)
    # Xcode 15+ ld64 warns "ignoring duplicate libraries" when a static lib appears both
    # explicitly (a target links it for its headers) and transitively in the same link closure.
    # It is benign — ld de-duplicates and links correctly — so suppress it where supported.
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries" MR_ADM_LD_NO_WARN_DUPLICATE_LIBRARIES)
    if(MR_ADM_LD_NO_WARN_DUPLICATE_LIBRARIES)
        add_link_options("LINKER:-no_warn_duplicate_libraries")
    endif()
endif()
if(NOT DEFINED MR_ADM_FLAC_AUTO_BUILD_TYPE)
    set(MR_ADM_FLAC_AUTO_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE INTERNAL "Initial build type for FLAC AUTO provider")
endif()
if(NOT DEFINED MR_ADM_OPUS_AUTO_BUILD_TYPE)
    set(MR_ADM_OPUS_AUTO_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE INTERNAL "Initial build type for Opus AUTO provider")
endif()

include(GNUInstallDirs)
include(MRWarnings)
# 受控数值构建的编译标志必须在依赖被 add_subdirectory() 进来之前设好，否则 FetchContent
# 拉下来的第三方目录拿不到（路线图 §5.1 第 3 项）。
include(MRStrictFp)
include(MRDependencies)
include(MRRoomCuda)
include(MRStaticAnalysis)
find_package(Threads REQUIRED)
if(MR_ADM_ENABLE_IAMF)
    include(MRIamfAomBridge)
    mr_adm_core_find_iamf_aom_bridge()
endif()
