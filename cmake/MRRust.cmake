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

# Record Cargo's actual source locations for license verification and build
# provenance. The lockfile includes dependencies for all supported platforms.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_BUILD_RUSTC=${Rust_COMPILER_CACHED}"
        "${Rust_CARGO_CACHED}" metadata --locked --format-version 1
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
