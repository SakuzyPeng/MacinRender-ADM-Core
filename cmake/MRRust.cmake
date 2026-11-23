# The Rust side of the top-level test tree (6.6). Cargo builds the C ABI (mradm-capi: the shared
# libmradm_capi, the staticlib the C++ tests link and the mradm executable) and every Rust tool. The
# islands come from src/ (added as a subdirectory before this file); every Cargo command here hands
# their link manifest to the mradm-islands-sys build script (MRADM_NATIVE_LINK_FILE), so Cargo links
# this tree's islands and does not build them again. Corrosion left the build with 6.6.

# ── Toolchain ─────────────────────────────────────────────────────────────────
# The pinned toolchain (rust/rust-toolchain.toml) through rustup, else the cargo and rustc on PATH.
# The cache names stay those scripts/consistency/build_info.py records.
set(Rust_TOOLCHAIN "1.98.0" CACHE STRING "Pinned Rust toolchain" FORCE)
if(NOT Rust_CARGO_CACHED OR NOT Rust_COMPILER_CACHED)
    find_program(MR_ADM_RUSTUP rustup)
    foreach(_mr_tool cargo rustc)
        set(_mr_path "")
        if(MR_ADM_RUSTUP)
            execute_process(COMMAND "${MR_ADM_RUSTUP}" which --toolchain "${Rust_TOOLCHAIN}" ${_mr_tool}
                OUTPUT_VARIABLE _mr_path OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _mr_status ERROR_QUIET)
            if(NOT _mr_status EQUAL 0)
                set(_mr_path "")
            endif()
        endif()
        if(NOT _mr_path)
            find_program(_mr_found_${_mr_tool} ${_mr_tool} REQUIRED)
            set(_mr_path "${_mr_found_${_mr_tool}}")
        endif()
        if(_mr_tool STREQUAL "cargo")
            set(Rust_CARGO_CACHED "${_mr_path}" CACHE FILEPATH "cargo of the pinned toolchain")
        else()
            set(Rust_COMPILER_CACHED "${_mr_path}" CACHE FILEPATH "rustc of the pinned toolchain")
        endif()
    endforeach()
endif()
execute_process(COMMAND "${Rust_COMPILER_CACHED}" -vV OUTPUT_VARIABLE _mr_rustc_version RESULT_VARIABLE _mr_status)
if(NOT _mr_status EQUAL 0 OR NOT _mr_rustc_version MATCHES "release: ${Rust_TOOLCHAIN}")
    message(FATAL_ERROR "Rust ${Rust_TOOLCHAIN} is required (rust/rust-toolchain.toml): ${Rust_COMPILER_CACHED}")
endif()
string(REGEX MATCH "host: ([^\r\n]+)" _mr_host "${_mr_rustc_version}")
set(_mr_rust_target "${CMAKE_MATCH_1}")
if(Rust_CARGO_TARGET)
    set(_mr_rust_target "${Rust_CARGO_TARGET}")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    set(_mr_rust_target aarch64-apple-darwin)
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    set(_mr_rust_target x86_64-apple-darwin)
endif()
set(Rust_CARGO_TARGET_CACHED "${_mr_rust_target}" CACHE STRING "Cargo target triple of this tree" FORCE)

# The system libraries a Rust staticlib needs (as rustc reports them for an empty crate), plus those
# the crates name with #[link] (MMCSS, sockets), for the C++ programs that link the staticlib.
set(_mr_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/rust-native-libs")
file(MAKE_DIRECTORY "${_mr_probe_dir}")
file(WRITE "${_mr_probe_dir}/lib.rs" "")
execute_process(COMMAND "${Rust_COMPILER_CACHED}" --crate-type staticlib --crate-name mr_native_probe --edition 2021
        --target "${Rust_CARGO_TARGET_CACHED}" --print native-static-libs -o "${_mr_probe_dir}/probe.lib" lib.rs
    WORKING_DIRECTORY "${_mr_probe_dir}" ERROR_VARIABLE _mr_native_probe OUTPUT_QUIET RESULT_VARIABLE _mr_status)
string(REGEX MATCH "native-static-libs: ([^\n]*)" _mr_native "${_mr_native_probe}")
if(NOT _mr_status EQUAL 0 OR NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Cannot read the system libraries of a Rust staticlib: ${_mr_native_probe}")
endif()
separate_arguments(MR_ADM_RUST_SYSTEM_LIBS NATIVE_COMMAND "${CMAKE_MATCH_1}")
if(MSVC)
    # The MSVC probe reports the same system libraries for both CRTs. The consuming compiler
    # selects the matching CRT through its /DEFAULTLIB directive.
    list(FILTER MR_ADM_RUST_SYSTEM_LIBS EXCLUDE REGEX "^/defaultlib:")
    list(APPEND MR_ADM_RUST_SYSTEM_LIBS avrt.lib ws2_32.lib)
elseif(APPLE)
    find_library(MR_ADM_COREAUDIO_FW CoreAudio REQUIRED)
    list(APPEND MR_ADM_RUST_SYSTEM_LIBS "${MR_ADM_COREAUDIO_FW}")
elseif(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD|NetBSD|DragonFly)$")
    find_package(ALSA REQUIRED)
    list(APPEND MR_ADM_RUST_SYSTEM_LIBS ALSA::ALSA)
endif()

# ── Cargo commands ────────────────────────────────────────────────────────────
# One target directory for every configuration and command; Cargo separates profiles itself.
file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" _mr_rust_manifest)
string(SHA1 _mr_rust_path_hash "${_mr_rust_manifest}")
string(SUBSTRING "${_mr_rust_path_hash}" 0 5 _mr_rust_path_hash)
set(MR_ADM_RUST_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/rust/cargo/rust_${_mr_rust_path_hash}")
# Keep the Cargo link on the same SDK and deployment target as the C++ islands.
set(MR_ADM_CARGO_COMMAND "${CMAKE_COMMAND}" -E env "CARGO_INCREMENTAL=0"
    "MRADM_NATIVE_LINK_FILE=${MR_ADM_NATIVE_LINK_FILE}"
    "CARGO_BUILD_RUSTC=${Rust_COMPILER_CACHED}")
if(APPLE)
    if(CMAKE_OSX_SYSROOT)
        list(APPEND MR_ADM_CARGO_COMMAND "SDKROOT=${CMAKE_OSX_SYSROOT}")
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND MR_ADM_CARGO_COMMAND "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
endif()
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
    # Merge the active Cargo flags at build time. Their unit separator cannot appear in VS XML.
    list(APPEND MR_ADM_CARGO_COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/MRRunCargoMSVC.cmake"
        -- "${CMAKE_MSVC_RUNTIME_LIBRARY}")
else()
    list(APPEND MR_ADM_CARGO_COMMAND "RUSTFLAGS=-Crelocation-model=pic")
endif()
list(APPEND MR_ADM_CARGO_COMMAND "${Rust_CARGO_CACHED}")

set(_mr_consistency_features)
if(MR_ADM_ENABLE_SOFA)
    list(APPEND _mr_consistency_features sofa)
endif()
if(MR_ADM_CONSISTENCY_DIAGNOSTICS)
    list(APPEND _mr_consistency_features diagnostics)
endif()
if(MR_ADM_ENABLE_ROOM_CUDA)
    list(APPEND _mr_consistency_features room-cuda)
endif()
set(_mr_consistency_feature_args)
if(_mr_consistency_features)
    list(JOIN _mr_consistency_features "," _mr_consistency_feature_string)
    set(_mr_consistency_feature_args --features "${_mr_consistency_feature_string}")
endif()
# ── The C ABI and the command line ────────────────────────────────────────────
# One Cargo build of mradm-capi gives the shared libmradm_capi (the product), its staticlib (for the
# C++ tests, which call the islands directly and need the same copy of them as the C ABI that binds
# their Rust services) and, when enabled, the mradm executable. They are copied into the build
# directory under the names CMake used to give them, with the shared library's versioned name and links.
set(_mr_profile "$<IF:$<CONFIG:Debug>,dev,$<IF:$<CONFIG:MinSizeRel>,minsizerel,release>>")
set(_mr_profile_dir "$<IF:$<CONFIG:Debug>,debug,$<IF:$<CONFIG:MinSizeRel>,minsizerel,release>>")
set(_mr_cargo_out "${MR_ADM_RUST_TARGET_DIR}/${Rust_CARGO_TARGET_CACHED}/${_mr_profile_dir}")
set(_mr_rust_out "${CMAKE_CURRENT_BINARY_DIR}")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND _mr_rust_out "/$<CONFIG>")
endif()
set(_mr_version "${PROJECT_VERSION}")
set(_mr_major "${PROJECT_VERSION_MAJOR}")
if(WIN32)
    set(_mr_static_name mradm_capi.lib)
    set(_mr_shared_cargo mradm_capi.dll)
    set(_mr_shared_name mradm_capi.dll)
    set(_mr_shared_links)
elseif(APPLE)
    set(_mr_static_name libmradm_capi.a)
    set(_mr_shared_cargo libmradm_capi.dylib)
    set(_mr_shared_name libmradm_capi.${_mr_version}.dylib)
    set(_mr_shared_links libmradm_capi.${_mr_major}.dylib libmradm_capi.dylib)
else()
    set(_mr_static_name libmradm_capi.a)
    set(_mr_shared_cargo libmradm_capi.so)
    set(_mr_shared_name libmradm_capi.so.${_mr_version})
    set(_mr_shared_links libmradm_capi.so.${_mr_major} libmradm_capi.so)
endif()
set(_mr_copy_commands
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_mr_rust_out}/rust-static"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_mr_cargo_out}/${_mr_static_name}"
        "${_mr_rust_out}/rust-static/${_mr_static_name}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_mr_cargo_out}/${_mr_shared_cargo}"
        "${_mr_rust_out}/${_mr_shared_name}")
set(_mr_rust_byproducts "${_mr_rust_out}/rust-static/${_mr_static_name}" "${_mr_rust_out}/${_mr_shared_name}")
foreach(_mr_link IN LISTS _mr_shared_links)
    list(APPEND _mr_copy_commands COMMAND "${CMAKE_COMMAND}" -E create_symlink "${_mr_shared_name}"
        "${_mr_rust_out}/${_mr_link}")
    list(APPEND _mr_rust_byproducts "${_mr_rust_out}/${_mr_link}")
endforeach()
if(WIN32)
    list(APPEND _mr_copy_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_mr_cargo_out}/mradm_capi.dll.lib" "${_mr_rust_out}/mradm_capi.lib")
    list(APPEND _mr_rust_byproducts "${_mr_rust_out}/mradm_capi.lib")
endif()
set(_mr_cargo_targets --lib)
if(MR_ADM_CORE_BUILD_CLI)
    list(APPEND _mr_cargo_targets --bin mradm)
    set(_mr_executable "mradm${CMAKE_EXECUTABLE_SUFFIX}")
    list(APPEND _mr_copy_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_mr_cargo_out}/${_mr_executable}" "${_mr_rust_out}/${_mr_executable}")
    list(APPEND _mr_rust_byproducts "${_mr_rust_out}/${_mr_executable}")
    if(APPLE)
        # The APAC encoder requires a full ad-hoc signature on its helper.
        list(APPEND _mr_copy_commands COMMAND /usr/bin/codesign --force --sign - "${_mr_rust_out}/${_mr_executable}")
    endif()
endif()
add_custom_target(mr_adm_rust ALL
    COMMAND ${MR_ADM_CARGO_COMMAND} build --locked --profile "${_mr_profile}"
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" -p mradm-capi ${_mr_cargo_targets}
        --no-default-features ${_mr_consistency_feature_args}
        --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}"
    ${_mr_copy_commands}
    BYPRODUCTS ${_mr_rust_byproducts}
    DEPENDS mradm_native
    USES_TERMINAL
    VERBATIM)
if(MR_ADM_ENABLE_ROOM_CUDA)
    add_dependencies(mr_adm_rust mr_adm_room_cuda)
endif()

function(mr_adm_rust_import target type file)
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(_mr_config IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${_mr_config}" _mr_upper)
            set_property(TARGET ${target} PROPERTY "IMPORTED_${type}_${_mr_upper}"
                "${CMAKE_CURRENT_BINARY_DIR}/${_mr_config}/${file}")
        endforeach()
    else()
        set_property(TARGET ${target} PROPERTY "IMPORTED_${type}" "${CMAKE_CURRENT_BINARY_DIR}/${file}")
    endif()
endfunction()

# The staticlib, followed by the islands it calls and the system libraries of Rust.
add_library(mradm_capi STATIC IMPORTED GLOBAL)
mr_adm_rust_import(mradm_capi LOCATION "rust-static/${_mr_static_name}")
target_link_libraries(mradm_capi INTERFACE mr_adm_native ${MR_ADM_RUST_SYSTEM_LIBS})
add_dependencies(mradm_capi mr_adm_rust)

# The shared C ABI (formerly the CMake target mradm_capi_bundle of MR_ADM_BUILD_CAPI_BUNDLE).
add_library(mradm_capi_bundle SHARED IMPORTED GLOBAL)
mr_adm_rust_import(mradm_capi_bundle LOCATION "${_mr_shared_name}")
if(WIN32)
    mr_adm_rust_import(mradm_capi_bundle IMPLIB "mradm_capi.lib")
elseif(APPLE)
    set_property(TARGET mradm_capi_bundle PROPERTY IMPORTED_SONAME "@rpath/libmradm_capi.${_mr_major}.dylib")
else()
    set_property(TARGET mradm_capi_bundle PROPERTY IMPORTED_SONAME "libmradm_capi.so.${_mr_major}")
endif()
add_dependencies(mradm_capi_bundle mr_adm_rust)

if(MR_ADM_CORE_BUILD_CLI)
    add_executable(mradm_exe IMPORTED GLOBAL)
    mr_adm_rust_import(mradm_exe LOCATION "${_mr_executable}")
    add_dependencies(mradm_exe mr_adm_rust)
endif()

# The C ABI as the C++ island tests link it: c_api.h and the staticlib with the islands.
add_library(mr_adm_c_api INTERFACE)
add_library(MacinRender::ADMCAPI ALIAS mr_adm_c_api)
target_include_directories(mr_adm_c_api INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mr_adm_c_api INTERFACE mradm_capi)

# Rust consistency tools (`mradm-ffi` examples): the kernel probe, the offline
# same-process repeat render, the file-streaming replay and the Scene replay (formerly the C++
# tests/tools/phase2_{render,stream,scene}.cpp). The Scene replay builds by default, as the C++
# tool did,
# because ctest's mr_adm_collection_tool_tests runs it.
foreach(_mr_consistency_example IN ITEMS kernels render stream scene)
    set(_mr_consistency_all)
    if(_mr_consistency_example STREQUAL "scene" AND MR_ADM_CORE_BUILD_TESTS)
        set(_mr_consistency_all ALL)
    endif()
    add_custom_target(mr_adm_consistency_${_mr_consistency_example} ${_mr_consistency_all}
        COMMAND ${MR_ADM_CARGO_COMMAND} build --locked --release
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" -p mradm-ffi
            --example consistency_${_mr_consistency_example} --no-default-features ${_mr_consistency_feature_args}
            --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MR_ADM_RUST_TARGET_DIR}/${Rust_CARGO_TARGET_CACHED}/release/examples/consistency_${_mr_consistency_example}${CMAKE_EXECUTABLE_SUFFIX}"
            "${CMAKE_CURRENT_BINARY_DIR}/mr_adm_consistency_${_mr_consistency_example}${CMAKE_EXECUTABLE_SUFFIX}"
        DEPENDS mradm_native
        USES_TERMINAL
        VERBATIM
    )
endforeach()

# Record Cargo's actual source locations for license verification and build
# provenance. The lockfile includes dependencies for all supported platforms.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_BUILD_RUSTC=${Rust_COMPILER_CACHED}"
        "${Rust_CARGO_CACHED}" metadata --locked --format-version 1
        --no-default-features ${_mr_consistency_feature_args}
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rust"
    RESULT_VARIABLE _mr_cargo_metadata_status
    OUTPUT_VARIABLE _mr_cargo_metadata
    ERROR_VARIABLE _mr_cargo_metadata_error
)
if(NOT _mr_cargo_metadata_status EQUAL 0)
    message(FATAL_ERROR "Cargo metadata failed: ${_mr_cargo_metadata_error}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rust-dependencies.json" "${_mr_cargo_metadata}\n")

add_custom_target(mr_adm_rust_quality
    COMMAND "${Rust_CARGO_CACHED}" fmt --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" --all -- --check
    COMMAND ${MR_ADM_CARGO_COMMAND} clippy --locked --workspace --all-targets
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml"
        --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}" -- -D warnings
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rust"
    DEPENDS mradm_native
    USES_TERMINAL
    VERBATIM
)

# Hand-written private FFI headers vs. the Rust exports, via a throwaway cbindgen header.
# Requires the pinned cbindgen (see scripts/quality/check-ffi-headers.py).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(mr_adm_ffi_header_check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/quality/check-ffi-headers.py"
            --output "${CMAKE_CURRENT_BINARY_DIR}/ffi-check/mradm_ffi.h"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL
    )
endif()
