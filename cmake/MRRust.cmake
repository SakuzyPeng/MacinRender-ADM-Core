# All configurations share one Cargo target directory. Cargo distinguishes its
# profiles and targets internally; do not duplicate the native dependency tree.
if(NOT DEFINED Rust_COMPILER)
    set(Rust_TOOLCHAIN "1.98.0" CACHE STRING "Pinned Rust toolchain" FORCE)
endif()
set(CORROSION_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CORROSION_VERBOSE_OUTPUT OFF CACHE BOOL "" FORCE)
mr_adm_core_find_or_fetch(Corrosion Corrosion::Corrosion)

function(mr_adm_import_rust)
    # Corrosion 0.6 derives --target-dir from CMAKE_BINARY_DIR and overrides
    # CARGO_TARGET_DIR. Scope its Cargo root here; CMAKE_CURRENT_BINARY_DIR and
    # the enclosing project's native build/cache directories stay unchanged.
    set(CMAKE_BINARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/rust")
    set(features)
    if(MR_ADM_ENABLE_SOFA)
        list(APPEND features sofa)
    endif()
    if(MR_ADM_CONSISTENCY_DIAGNOSTICS)
        list(APPEND features diagnostics)
    endif()
    corrosion_import_crate(
        MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml"
        CRATES mradm-ffi
        PROFILE "$<IF:$<CONFIG:MinSizeRel>,minsizerel,$<IF:$<CONFIG:Debug>,dev,release>>"
        NO_DEFAULT_FEATURES
        FEATURES ${features}
        LOCKED
        FLAGS --locked
    )
endfunction()
mr_adm_import_rust()
corrosion_set_env_vars(mradm_ffi
    "CARGO_TARGET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/build/rust"
    "CARGO_INCREMENTAL=0"
)
corrosion_add_target_rustflags(mradm_ffi "-Crelocation-model=pic")

add_library(mr_adm_dsp INTERFACE)
add_library(MacinRender::ADMDsp ALIAS mr_adm_dsp)
target_link_libraries(mr_adm_dsp INTERFACE mradm_ffi)
target_include_directories(mr_adm_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/adm_dsp")

# Reuse Corrosion's actual target directory for Cargo unit/integration tests.
file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" _mr_rust_manifest)
string(SHA1 _mr_rust_path_hash "${_mr_rust_manifest}")
string(SUBSTRING "${_mr_rust_path_hash}" 0 5 _mr_rust_path_hash)
set(MR_ADM_RUST_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/rust/cargo/rust_${_mr_rust_path_hash}")

set(_mr_phase2_features)
if(MR_ADM_ENABLE_SOFA)
    list(APPEND _mr_phase2_features sofa)
endif()
if(MR_ADM_CONSISTENCY_DIAGNOSTICS)
    list(APPEND _mr_phase2_features diagnostics)
endif()
set(_mr_phase2_feature_args)
if(_mr_phase2_features)
    list(JOIN _mr_phase2_features "," _mr_phase2_feature_string)
    set(_mr_phase2_feature_args --features "${_mr_phase2_feature_string}")
endif()
add_custom_target(mr_adm_phase2_kernels
    COMMAND "${CMAKE_COMMAND}" -E env "RUSTFLAGS=-Crelocation-model=pic" "CARGO_INCREMENTAL=0"
        "RUSTC=${Rust_COMPILER_CACHED}" "${Rust_CARGO_CACHED}" build --locked --release
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" -p mradm-ffi
        --example phase2_kernels --no-default-features ${_mr_phase2_feature_args}
        --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${MR_ADM_RUST_TARGET_DIR}/${Rust_CARGO_TARGET_CACHED}/release/examples/phase2_kernels${CMAKE_EXECUTABLE_SUFFIX}"
        "${CMAKE_CURRENT_BINARY_DIR}/mr_adm_phase2_kernels${CMAKE_EXECUTABLE_SUFFIX}"
    USES_TERMINAL
)

# Record Cargo's actual source locations for license verification and build
# provenance. The lockfile includes dependencies for all supported platforms.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_BUILD_RUSTC=${Rust_COMPILER_CACHED}"
        "${Rust_CARGO_CACHED}" metadata --locked --format-version 1
        --no-default-features ${_mr_phase2_feature_args}
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
    COMMAND "${CMAKE_COMMAND}" -E env "RUSTFLAGS=-Crelocation-model=pic" "CARGO_INCREMENTAL=0"
        "CARGO_BUILD_RUSTC=${Rust_COMPILER_CACHED}" "${Rust_CARGO_CACHED}" clippy --locked --workspace --all-targets
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml"
        --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}" -- -D warnings
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rust"
    USES_TERMINAL
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
