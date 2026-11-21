cmake_minimum_required(VERSION 3.24)

project(
    MacinRenderADMCore
    VERSION 1.9.1
    DESCRIPTION "Cross-platform ADM rendering core for MacinRender"
    LANGUAGES C CXX
)

option(MR_ADM_CORE_BUILD_CLI "Build the mradm command-line interface" ON)
option(MR_ADM_CORE_BUILD_TESTS "Build tests" ON)
option(MR_ADM_BUILD_CAPI_BUNDLE "Accepted and ignored since 6.6: the shared libmradm_capi is always built" OFF)
option(MR_ADM_BUILD_HEADMOTION "Build standalone AirPods motion shim (macOS)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MRProject)

# ── Islands ───────────────────────────────────────────────────────────────────
# The C/C++ islands and third-party C libraries (src/CMakeLists.txt, the project mradm-islands-sys
# builds on its own), then the Rust library and tools built by Cargo over them (cmake/MRRust.cmake).
add_subdirectory(src)
include(MRRust)
mr_adm_core_write_dependency_record()

# ── AirPods motion shim (GUI, macOS) ─────────────────────────────────────────
if(APPLE AND MR_ADM_BUILD_HEADMOTION)
    enable_language(OBJC)
    add_library(mr_headtrack SHARED gui/native/mr_headtrack.m gui/native/mr_headmotion.m)
    target_compile_options(mr_headtrack PRIVATE -fobjc-arc)
    target_link_libraries(mr_headtrack PRIVATE "-framework CoreMotion" "-framework Foundation")
    set_target_properties(mr_headtrack PROPERTIES INSTALL_NAME_DIR "@rpath")
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

if(MR_ADM_CORE_BUILD_TESTS)
    enable_testing()

    # The island tests' private declarations (tests/support/ffi/island_test_ffi.h). The island tests
    # link the C ABI: one C ABI call binds the islands' Rust services (tests/support/rust_services_test.h).
    include_directories(tests/support/ffi)

    add_executable(mr_adm_wav_io_tests tests/unit/wav_io_test.cpp)
    target_link_libraries(mr_adm_wav_io_tests PRIVATE MacinRender::ADMAudio MacinRender::ADMCAPI)
    mr_adm_core_apply_warnings(mr_adm_wav_io_tests)
    mr_adm_core_apply_static_analysis(mr_adm_wav_io_tests)
    add_test(NAME mr_adm_wav_io_tests COMMAND mr_adm_wav_io_tests)

    # The audio island's WAVE / CAF writer wrappers (R3 C4 removed its SDK checks; 7.2 C its metadata checks).
    add_executable(mr_adm_core_unit_tests tests/unit/core_smoke_test.cpp)
    target_link_libraries(mr_adm_core_unit_tests PRIVATE MacinRender::ADMAudio MacinRender::ADMCAPI)
    mr_adm_core_apply_warnings(mr_adm_core_unit_tests)
    mr_adm_core_apply_static_analysis(mr_adm_core_unit_tests)
    add_test(NAME mr_adm_core_unit_tests COMMAND mr_adm_core_unit_tests)

    # Exercise the public header and shared library as a downstream C++ consumer, including the
    # import library on Windows. Island tests that need private symbols use the static library.
    add_executable(mr_adm_c_api_tests tests/unit/c_api_consumer_test.cpp)
    target_include_directories(mr_adm_c_api_tests PRIVATE include)
    target_link_libraries(mr_adm_c_api_tests PRIVATE mradm_capi_bundle)
    if(MSVC)
        target_compile_options(mr_adm_c_api_tests PRIVATE /utf-8)
    elseif(APPLE)
        set_target_properties(mr_adm_c_api_tests PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@loader_path")
    elseif(UNIX)
        set_target_properties(mr_adm_c_api_tests PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
    endif()
    target_compile_definitions(mr_adm_c_api_tests PRIVATE MR_ADM_ENABLE_IAMF=$<BOOL:${MR_ADM_ENABLE_IAMF}>)
    # Keep the synthetic room beside the consumer for relocated builds.
    add_custom_target(mr_adm_capi_room_fixture
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/room/synthetic"
            "$<TARGET_FILE_DIR:mr_adm_c_api_tests>/capi-room"
        VERBATIM)
    add_dependencies(mr_adm_c_api_tests mr_adm_capi_room_fixture)
    mr_adm_core_apply_warnings(mr_adm_c_api_tests)
    mr_adm_core_apply_static_analysis(mr_adm_c_api_tests)
    add_test(NAME mr_adm_c_api_tests COMMAND mr_adm_c_api_tests capi-room/room.json
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:mr_adm_c_api_tests>")

    add_executable(mr_adm_c_api_header_smoke tests/unit/c_api_header_smoke.c)
    target_include_directories(mr_adm_c_api_header_smoke PRIVATE include)
    target_compile_features(mr_adm_c_api_header_smoke PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(mr_adm_c_api_header_smoke PRIVATE /utf-8)
    endif()
    mr_adm_core_apply_warnings(mr_adm_c_api_header_smoke)
    mr_adm_core_apply_static_analysis(mr_adm_c_api_header_smoke)
    add_test(NAME mr_adm_c_api_header_smoke COMMAND mr_adm_c_api_header_smoke)

    add_executable(mr_adm_osc_head_tracking_c_example EXCLUDE_FROM_ALL examples/head_tracking_consumer.c)
    target_link_libraries(mr_adm_osc_head_tracking_c_example PRIVATE MacinRender::ADMCAPI)
    mr_adm_core_apply_warnings(mr_adm_osc_head_tracking_c_example)



    # Release-only/manual acceptance tool. It is deliberately excluded from the default build and
    # CTest because its normal invocation runs for ten wall-clock minutes.
    add_executable(mr_adm_scene_stream_stress EXCLUDE_FROM_ALL tests/manual/scene_stream_stress.cpp)
    target_link_libraries(mr_adm_scene_stream_stress PRIVATE MacinRender::ADMCAPI)
    mr_adm_core_apply_warnings(mr_adm_scene_stream_stress)
    mr_adm_core_apply_static_analysis(mr_adm_scene_stream_stress)

    # The consistency tools (R3 A, formerly C++ under tests/tools): the fixture generator, the PCM
    # bit-image tool and the FFT benchmark are `mradm-testkit` examples, the same-process repeat
    # render an `mradm-ffi` example (it renders on the production engine host). Each builds in
    # Release and is copied under its former name, so the scripts and workflows call it unchanged.
    # mr_adm_pcm_bits builds by default so the CLI/protocol regression tests can run through CTest.
    foreach(_mr_tool IN ITEMS make_fixture:mradm-testkit pcm_bits:mradm-testkit
            fft_benchmark:mradm-testkit repeat_render:mradm-ffi)
        string(REPLACE ":" ";" _mr_tool "${_mr_tool}")
        list(GET _mr_tool 0 _mr_tool_example)
        list(GET _mr_tool 1 _mr_tool_package)
        set(_mr_tool_all)
        if(_mr_tool_example STREQUAL "pcm_bits")
            set(_mr_tool_all ALL)
        endif()
        set(_mr_tool_features)
        if(_mr_tool_package STREQUAL "mradm-ffi")
            set(_mr_tool_features --no-default-features ${_mr_consistency_feature_args})
        endif()
        add_custom_target(mr_adm_${_mr_tool_example} ${_mr_tool_all}
            COMMAND ${MR_ADM_CARGO_COMMAND} build --locked --release
                --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" -p ${_mr_tool_package}
                --example ${_mr_tool_example} ${_mr_tool_features}
                --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MR_ADM_RUST_TARGET_DIR}/${Rust_CARGO_TARGET_CACHED}/release/examples/${_mr_tool_example}${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_BINARY_DIR}/mr_adm_${_mr_tool_example}${CMAKE_EXECUTABLE_SUFFIX}"
            DEPENDS mradm_native
            USES_TERMINAL
            VERBATIM)
    endforeach()
    set(_mr_pcm_bits "${CMAKE_CURRENT_BINARY_DIR}/mr_adm_pcm_bits${CMAKE_EXECUTABLE_SUFFIX}")

    # Build the manual probes on request under their original names. Match the Rust profile to
    # the native configuration so the Release-only benchmarks also reject a CMake Debug build.
    set(_mr_probe_optimized "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")
    set(_mr_probe_profile "$<IF:$<CONFIG:MinSizeRel>,minsizerel,$<IF:${_mr_probe_optimized},release,dev>>")
    set(_mr_probe_cargo_dir "$<IF:${_mr_probe_optimized},${_mr_probe_profile},debug>")
    set(_mr_probe_output_dir "${CMAKE_CURRENT_BINARY_DIR}")
    if(CMAKE_CONFIGURATION_TYPES)
        string(APPEND _mr_probe_output_dir "/$<CONFIG>")
    endif()
    # The room device probes link the islands like every mradm-ffi target (mradm-islands-sys); only
    # they reference the capture shim (tests/manual/capture_tap.c, in the islands' link manifest).
    foreach(_mr_probe IN ITEMS osc_head_tracking_probe triple_balance_scene_benchmark
            triple_balance_monitor_benchmark room_scene_benchmark room_file_device_probe)
        set(_mr_probe_depends mradm_native)
        if(_mr_probe MATCHES "^room_" AND MR_ADM_ENABLE_ROOM_CUDA)
            list(APPEND _mr_probe_depends mr_adm_room_cuda)
        endif()
        add_custom_target(mr_adm_${_mr_probe}
            COMMAND ${MR_ADM_CARGO_COMMAND} build --locked
                --profile "${_mr_probe_profile}"
                --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" -p mradm-ffi
                --example ${_mr_probe} --no-default-features ${_mr_consistency_feature_args}
                --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_mr_probe_output_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MR_ADM_RUST_TARGET_DIR}/${Rust_CARGO_TARGET_CACHED}/${_mr_probe_cargo_dir}/examples/${_mr_probe}${CMAKE_EXECUTABLE_SUFFIX}"
                "${_mr_probe_output_dir}/mr_adm_${_mr_probe}${CMAKE_EXECUTABLE_SUFFIX}"
            DEPENDS ${_mr_probe_depends}
            USES_TERMINAL
            VERBATIM)
    endforeach()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME mr_adm_room_realtime_tools_tests COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/room_realtime_tools_test.py")
        if(TARGET mradm_exe AND CMAKE_BUILD_TYPE STREQUAL "Release")
            add_test(NAME mr_adm_room_96k_tests COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/room_96k_test.py"
                "$<TARGET_FILE:mradm_exe>" "$<TARGET_FILE:mradm_capi_bundle>")
            set_tests_properties(mr_adm_room_96k_tests PROPERTIES TIMEOUT 120)
        endif()
        set(MR_ADM_ROOM_PREP_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH
            "Existing Python environment with numpy, scipy and h5py for room preparation tests")
        execute_process(COMMAND "${MR_ADM_ROOM_PREP_PYTHON}" -c "import numpy, scipy, h5py"
            RESULT_VARIABLE _mr_room_python_status OUTPUT_QUIET ERROR_QUIET)
        if(_mr_room_python_status EQUAL 0)
            add_test(NAME mr_adm_room_hrir_prepare_tests COMMAND "${MR_ADM_ROOM_PREP_PYTHON}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/room_hrir_prepare_test.py")
            add_test(NAME mr_adm_room_acoustic_tools_tests COMMAND "${MR_ADM_ROOM_PREP_PYTHON}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/room_acoustic_tools_test.py")
            add_test(NAME mr_adm_hrtf_dvf_tests COMMAND "${MR_ADM_ROOM_PREP_PYTHON}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/hrtf_dvf_test.py")
        else()
            message(STATUS "Room HRIR preparation tests need numpy/scipy/h5py in MR_ADM_ROOM_PREP_PYTHON")
        endif()
        foreach(_room_probe IN ITEMS room_hrir_check room_omni room_reflection_check)
            add_custom_target(mr_adm_${_room_probe}
                COMMAND ${MR_ADM_CARGO_COMMAND} build --locked --release
                    --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" -p mradm-dsp --no-default-features
                    --example ${_room_probe} --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${MR_ADM_RUST_TARGET_DIR}/${Rust_CARGO_TARGET_CACHED}/release/examples/${_room_probe}${CMAKE_EXECUTABLE_SUFFIX}"
                    "${CMAKE_CURRENT_BINARY_DIR}/mr_adm_${_room_probe}${CMAKE_EXECUTABLE_SUFFIX}"
                USES_TERMINAL)
        endforeach()
        add_test(NAME mr_adm_fft_benchmark_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/fft_benchmark_test.py")
        add_test(NAME mr_adm_room_device_probe_tests
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/room_device_probe_test.py")
        # The CLI record of the island slices (phase 7; restored from phase 6, deleted at 7.7).
        add_test(NAME mr_adm_boundary_tool_tests
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/boundary_tools_test.py")
        if(TARGET mradm_exe)
            add_custom_target(mr_adm_consistency_tools
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/consistency/collection.py"
                    --stamp-build "${CMAKE_CURRENT_BINARY_DIR}"
                DEPENDS mr_adm_rust mr_adm_make_fixture mr_adm_pcm_bits mr_adm_consistency_render
                    mr_adm_consistency_scene mr_adm_consistency_stream mr_adm_consistency_kernels mr_adm_repeat_render
                USES_TERMINAL
            )
        endif()
        set(_mr_scene_checkpoint_args)
        if(MR_ADM_CONSISTENCY_DIAGNOSTICS)
            list(APPEND _mr_scene_checkpoint_args --diagnostic-checkpoints)
        endif()
        add_test(NAME mr_adm_collection_tool_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/collection_tools_test.py"
                --scene "${CMAKE_CURRENT_BINARY_DIR}/mr_adm_consistency_scene${CMAKE_EXECUTABLE_SUFFIX}"
                ${_mr_scene_checkpoint_args})
    endif()

    if(MR_ADM_CONSISTENCY_DIAGNOSTICS)
        foreach(_trace_target IN ITEMS mr_adm_audio mr_adm_render_common)
            target_compile_definitions(${_trace_target} PRIVATE MR_ADM_CONSISTENCY_DIAGNOSTICS=1)
        endforeach()
    endif()

    # Standard-library-only Python tests exercise the actual executable and shell exit codes.
    # Native builds without Python can still use the tools; the consistency workflow requires
    # this test explicitly on each runner so its checks cannot silently disappear there.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        if(WIN32)
            find_program(MR_ADM_CONSISTENCY_BASH NAMES bash
                PATHS "$ENV{ProgramFiles}/Git/bin" "$ENV{ProgramFiles}/Git/usr/bin" NO_DEFAULT_PATH)
        else()
            find_program(MR_ADM_CONSISTENCY_BASH NAMES bash)
        endif()
        set(_mr_consistency_bash "")
        if(MR_ADM_CONSISTENCY_BASH)
            set(_mr_consistency_bash "${MR_ADM_CONSISTENCY_BASH}")
        endif()
        add_test(NAME mr_adm_consistency_tool_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/consistency_tools_test.py"
                --pcm-bits "${_mr_pcm_bits}" --bash "${_mr_consistency_bash}")
        add_test(NAME mr_adm_consistency_build_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/consistency_build_test.py"
                --cmake "${CMAKE_COMMAND}" --c-compiler "${CMAKE_C_COMPILER}"
                --cxx-compiler "${CMAKE_CXX_COMPILER}")
        add_test(NAME mr_adm_consistency_checkpoint_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/consistency_checkpoints_test.py")
        add_test(NAME mr_adm_quality_scripts_tests
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/quality_scripts_test.py")
    endif()

    if(Python3_Interpreter_FOUND AND TARGET mradm_capi_bundle)
        add_test(NAME mr_adm_hptf_stereo_c_api_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/hptf_stereo_test.py"
                    "$<TARGET_FILE:mradm_capi_bundle>")
        set_tests_properties(mr_adm_hptf_stereo_c_api_tests PROPERTIES TIMEOUT 60)
        if(APPLE)
            add_test(NAME mr_adm_apple_render_trim_tests
                COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/apple_render_trim_test.py"
                        "$<TARGET_FILE:mradm_capi_bundle>")
            set_tests_properties(mr_adm_apple_render_trim_tests PROPERTIES TIMEOUT 60)
        endif()
    endif()

    add_executable(mr_adm_speaker_layouts_tests tests/unit/speaker_layouts_test.cpp)
    target_link_libraries(mr_adm_speaker_layouts_tests PRIVATE MacinRender::ADMRenderCommon MacinRender::ADMCAPI)
    mr_adm_core_apply_warnings(mr_adm_speaker_layouts_tests)
    mr_adm_core_apply_static_analysis(mr_adm_speaker_layouts_tests)
    add_test(NAME mr_adm_speaker_layouts_tests COMMAND mr_adm_speaker_layouts_tests)

    # The Apple island's DirectSpeakers matrix rendering; the Rust backends' cases are Rust tests.
    if(APPLE)
        add_executable(mr_adm_direct_speakers_matrix_render_tests tests/unit/direct_speakers_matrix_render_test.cpp)
        target_link_libraries(mr_adm_direct_speakers_matrix_render_tests
            PRIVATE MacinRender::ADMRenderApple MacinRender::ADMAudio MacinRender::ADMDsp MacinRender::ADMCAPI)
        mr_adm_core_apply_warnings(mr_adm_direct_speakers_matrix_render_tests)
        mr_adm_core_apply_static_analysis(mr_adm_direct_speakers_matrix_render_tests)
        add_test(NAME mr_adm_direct_speakers_matrix_render_tests COMMAND mr_adm_direct_speakers_matrix_render_tests)
    endif()

    # The island LFE pair recombination; on macOS also the Apple backend through the C ABI and its
    # island table (the EAR / VBAP routing is tested in mradm-engine tests/lfe_render.rs, R3 F2).
    add_executable(mr_adm_lfe_routing_tests tests/unit/lfe_routing_test.cpp)
    target_link_libraries(mr_adm_lfe_routing_tests PRIVATE MacinRender::ADMRenderCommon MacinRender::ADMCAPI)
    if(APPLE)
        target_link_libraries(mr_adm_lfe_routing_tests
            PRIVATE MacinRender::ADMCAPI MacinRender::ADMRenderApple MacinRender::ADMAudio)
    endif()
    mr_adm_core_apply_warnings(mr_adm_lfe_routing_tests)
    mr_adm_core_apply_static_analysis(mr_adm_lfe_routing_tests)
    add_test(NAME mr_adm_lfe_routing_tests COMMAND mr_adm_lfe_routing_tests)

    set(_mr_rust_test_features --no-default-features)
    if(MR_ADM_ENABLE_SOFA)
        list(APPEND _mr_rust_test_features --features sofa)
    endif()
    set(_mr_rust_unit_test_command
        ${MR_ADM_CARGO_COMMAND} test --locked --workspace
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml"
        --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}" ${_mr_rust_test_features})
    set(_mr_room_trace_test_command
        ${MR_ADM_CARGO_COMMAND} test --locked
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" -p mradm-dsp
        --no-default-features --features room-trace --test room_profile --test room_no_alloc
        --test rayon_sleep_no_alloc
        --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}")

    # Compile both Cargo test configurations during the build, outside CTest's execution
    # timeouts. Reuse the exact test commands and Cargo cache; finish the native Rust
    # library first so the two builds do not contend for Cargo's target-directory lock.
    add_custom_target(mr_adm_rust_test_binaries ALL
        COMMAND ${_mr_rust_unit_test_command} --no-run
        COMMAND ${_mr_room_trace_test_command} --no-run
        DEPENDS mr_adm_rust
        USES_TERMINAL VERBATIM)

    add_test(NAME mr_adm_rust_unit_tests COMMAND ${_mr_rust_unit_test_command})
    set_tests_properties(mr_adm_rust_unit_tests PROPERTIES TIMEOUT 300 RESOURCE_LOCK mr_adm_rust_cargo)
    add_test(NAME mr_adm_room_trace_tests COMMAND ${_mr_room_trace_test_command})
    set_tests_properties(mr_adm_room_trace_tests PROPERTIES TIMEOUT 300 RESOURCE_LOCK mr_adm_rust_cargo)

    # The release workflows run the encoder tests with the product's profile and native libraries.
    set(_mr_encoder_test_command
        ${MR_ADM_CARGO_COMMAND} test --locked
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml"
        --target-dir "${MR_ADM_RUST_TARGET_DIR}" --target "${Rust_CARGO_TARGET_CACHED}"
        --profile "${_mr_profile}" --no-default-features ${_mr_consistency_feature_args}
        --lib -p mradm-ffi -p mradm-capi)
    add_custom_target(mr_adm_encoder_tests
        COMMAND ${_mr_encoder_test_command} --no-run
        DEPENDS mr_adm_rust
        USES_TERMINAL VERBATIM)
    add_test(NAME mr_adm_encoder_tests
        COMMAND ${_mr_encoder_test_command} -- flac::tests:: opus_mka::tests:: mp4_metadata::tests:: mp4_packager::tests:: flac_outputs_are_encoded_and_tagged)
    set_tests_properties(mr_adm_encoder_tests PROPERTIES TIMEOUT 300 RESOURCE_LOCK mr_adm_rust_cargo)

    add_executable(mr_adm_meter_tests tests/unit/meter_test.cpp)
    target_link_libraries(mr_adm_meter_tests PRIVATE MacinRender::ADMCore MacinRender::ADMDsp MacinRender::ADMCAPI)
    mr_adm_core_apply_warnings(mr_adm_meter_tests)
    mr_adm_core_apply_static_analysis(mr_adm_meter_tests)
    add_test(NAME mr_adm_meter_tests COMMAND mr_adm_meter_tests)

    # Island dispatch rejects null requests and leaves unrelated islands to their owners.
    add_executable(mr_adm_audio_island_tests tests/unit/audio_island_test.cpp)
    target_link_libraries(mr_adm_audio_island_tests PRIVATE MacinRender::ADMAudio MacinRender::ADMCAPI)
    mr_adm_core_apply_warnings(mr_adm_audio_island_tests)
    mr_adm_core_apply_static_analysis(mr_adm_audio_island_tests)
    add_test(NAME mr_adm_audio_island_tests COMMAND mr_adm_audio_island_tests)

    add_executable(mr_adm_iamf_smoke_tests tests/unit/iamf_smoke_test.cpp)
    target_link_libraries(mr_adm_iamf_smoke_tests PRIVATE MacinRender::ADMAudio MacinRender::ADMCAPI)
    target_compile_definitions(mr_adm_iamf_smoke_tests PRIVATE MR_ADM_ENABLE_IAMF=$<BOOL:${MR_ADM_ENABLE_IAMF}>)
    mr_adm_core_apply_warnings(mr_adm_iamf_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_iamf_smoke_tests)
    add_test(NAME mr_adm_iamf_smoke_tests COMMAND mr_adm_iamf_smoke_tests)

    add_executable(mr_adm_apac_smoke_tests tests/unit/apac_smoke_test.cpp)
    target_link_libraries(mr_adm_apac_smoke_tests PRIVATE MacinRender::ADMAudio MacinRender::ADMCAPI)
    if(APPLE)
        target_link_libraries(mr_adm_apac_smoke_tests PRIVATE ${MR_ADM_AUDIOTOOLBOX_FW} "-framework CoreFoundation")
    endif()
    # Pin the APAC stall-watchdog test to the real mradm subprocess helper (and
    # order the build after it). Skipped when the CLI is not built.
    if(TARGET mradm_exe)
        target_compile_definitions(mr_adm_apac_smoke_tests
            PRIVATE MRADM_EXE_PATH="$<TARGET_FILE:mradm_exe>")
        add_dependencies(mr_adm_apac_smoke_tests mr_adm_rust)
    endif()
    mr_adm_core_apply_warnings(mr_adm_apac_smoke_tests)
    mr_adm_core_apply_static_analysis(mr_adm_apac_smoke_tests)
    add_test(NAME mr_adm_apac_smoke_tests COMMAND mr_adm_apac_smoke_tests)

    if(APPLE)
        # Manual real-device check for the system-spatial monitor (AVSampleBufferAudioRenderer):
        # drives a live monitor with monitor_system_spatial=on. NOT a ctest — needs AirPods + a
        # human turning their head. Run by hand: mr_monitor_spatial_check <input> [layout] [secs].
        add_executable(mr_monitor_spatial_check tests/manual/system_spatial_monitor_check.cpp)
        target_link_libraries(mr_monitor_spatial_check PRIVATE MacinRender::ADMCAPI)
        mr_adm_core_apply_warnings(mr_monitor_spatial_check)

        # The Apple backend through the C ABI and its island table, and the AUSpatialMixer HRTF probe
        # (R3 F3: without the C++ SDK).
        add_executable(mr_adm_apple_smoke_tests tests/unit/apple_renderer_smoke_test.cpp)
        target_link_libraries(mr_adm_apple_smoke_tests
            PRIVATE MacinRender::ADMCAPI MacinRender::ADMRenderApple MacinRender::ADMAudio
                nlohmann_json::nlohmann_json ${MR_ADM_AUDIOTOOLBOX_FW})
        mr_adm_core_apply_warnings(mr_adm_apple_smoke_tests)
        mr_adm_core_apply_static_analysis(mr_adm_apple_smoke_tests)
        add_test(NAME mr_adm_apple_smoke_tests COMMAND mr_adm_apple_smoke_tests)

        add_executable(mr_adm_apple_island_tests tests/unit/apple_island_test.cpp)
        target_link_libraries(mr_adm_apple_island_tests PRIVATE MacinRender::ADMRenderApple MacinRender::ADMDsp MacinRender::ADMCAPI)
        mr_adm_core_apply_warnings(mr_adm_apple_island_tests)
        mr_adm_core_apply_static_analysis(mr_adm_apple_island_tests)
        add_test(NAME mr_adm_apple_island_tests COMMAND mr_adm_apple_island_tests)
    endif()

    if(MR_ADM_CORE_BUILD_CLI)
        # The mradm executable as a user runs it: mradm-cli's tests/binary.rs (R3 E4), the Cargo test
        # command and binaries of mr_adm_rust_unit_tests with MRADM_EXE_PATH naming the executable.
        # The target of the same name builds that test binary (release.yml builds it by name).
        add_custom_target(mr_adm_cli_smoke_tests
            COMMAND ${_mr_rust_unit_test_command} --test binary --no-run
            DEPENDS mr_adm_rust
            USES_TERMINAL VERBATIM)
        add_test(NAME mr_adm_cli_smoke_tests COMMAND ${_mr_rust_unit_test_command} --test binary)
        set_tests_properties(mr_adm_cli_smoke_tests PROPERTIES
            ENVIRONMENT "MRADM_EXE_PATH=$<TARGET_FILE:mradm_exe>"
            TIMEOUT 300 RESOURCE_LOCK mr_adm_rust_cargo)
    endif()
endif()

# ── Install ───────────────────────────────────────────────────────────────────
# The public surface is the C ABI only: c_api.h, the shared libmradm_capi with its versioned name and
# links (and the import library on Windows) and the mradm command line, all built by Cargo
# (cmake/MRRust.cmake). There is no C++ SDK or CMake package.
install(FILES include/adm/c_api.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/adm")
if(WIN32)
    install(FILES "$<TARGET_FILE:mradm_capi_bundle>" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "$<TARGET_LINKER_FILE:mradm_capi_bundle>" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
else()
    foreach(_mr_name IN ITEMS "${_mr_shared_name}" ${_mr_shared_links})
        install(FILES "$<TARGET_FILE_DIR:mradm_capi_bundle>/${_mr_name}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endforeach()
endif()
if(MR_ADM_CORE_BUILD_CLI)
    install(PROGRAMS "$<TARGET_FILE:mradm_exe>" DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# Optional untracked local targets (developer scratch / experiments). The `local/`
# directory is git-ignored; this hook lets it reuse the main build (deps already
# resolved) without polluting any tracked target. No-op when the directory is absent.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/local/CMakeLists.txt")
    add_subdirectory(local EXCLUDE_FROM_ALL)
endif()
