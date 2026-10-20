if(NOT MR_ADM_ENABLE_ROOM_CUDA)
    return()
endif()
if(NOT WIN32)
    message(FATAL_ERROR "MR_ADM_ENABLE_ROOM_CUDA currently supports Windows only")
endif()
option(MR_ADM_ROOM_CUDA_FUSE_TARGET "Fuse target gather, FFT and install" ON)
option(MR_ADM_ROOM_CUDA_FUSE_CONVOLVE "Research: fuse convolution, FFT and extraction" OFF)
option(MR_ADM_ROOM_CUDA_COMPACT_UPLOAD "Compact CUDA metadata uploads" ON)
option(MR_ADM_ROOM_CUDA_FREQUENCY_MAJOR "Schedule response rows within frequency tiles" ON)
set(MR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR "" CACHE PATH "MathDx include directory containing cufftdx.hpp")
set(MR_ADM_ROOM_CUDA_ARCHITECTURES "75;80;86;89;90" CACHE STRING "SM architectures for the optional target FFT device programs")
find_package(CUDAToolkit 12 REQUIRED)
add_library(mr_adm_room_cuda SHARED src/adm_render_room/cuda/backend.cpp)
target_compile_features(mr_adm_room_cuda PRIVATE cxx_std_20)
target_compile_definitions(mr_adm_room_cuda PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MR_ADM_ROOM_CUDA_COMPACT_UPLOAD)
    target_compile_definitions(mr_adm_room_cuda PRIVATE ROOM_GPU_COMPACT_UPLOAD)
endif()
if(MR_ADM_ROOM_CUDA_FREQUENCY_MAJOR)
    target_compile_definitions(mr_adm_room_cuda PRIVATE ROOM_GPU_FREQUENCY_MAJOR)
endif()
if(MR_ADM_ROOM_CUDA_FUSE_TARGET OR MR_ADM_ROOM_CUDA_FUSE_CONVOLVE)
    if(NOT EXISTS "${MR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR}/cufftdx.hpp")
        message(FATAL_ERROR "FFT fusion requires an explicit MathDx include directory")
    endif()
    list(GET CUDAToolkit_INCLUDE_DIRS 0 _mr_cuda_toolkit_include)
endif()
if(MR_ADM_ROOM_CUDA_FUSE_CONVOLVE)
    # This unselected research variant still compiles its dynamic convolution
    # kernel locally. Product V2 uses only the precompiled target FFT below.
    target_compile_definitions(mr_adm_room_cuda PRIVATE
        ROOM_CUDA_MATHDX_INCLUDE="${MR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR}"
        ROOM_CUDA_TOOLKIT_INCLUDE="${_mr_cuda_toolkit_include}")
endif()
if(MR_ADM_ROOM_CUDA_FUSE_TARGET)
    target_compile_definitions(mr_adm_room_cuda PRIVATE ROOM_GPU_FUSE_TARGET)
endif()
if(MR_ADM_ROOM_CUDA_FUSE_CONVOLVE)
    target_compile_definitions(mr_adm_room_cuda PRIVATE ROOM_GPU_FUSE_CONVOLVE)
endif()
target_include_directories(mr_adm_room_cuda PRIVATE src/adm_render_room/cuda)
target_link_libraries(mr_adm_room_cuda PRIVATE CUDA::cuda_driver CUDA::cudart CUDA::nvrtc CUDA::cufft)
set_target_properties(mr_adm_room_cuda PROPERTIES OUTPUT_NAME mradm_room_cuda)
if(MSVC)
    target_compile_options(mr_adm_room_cuda PRIVATE /W4 /EHsc /utf-8)
endif()

# No dependency is linked into the public C API or loaded for CPU/exact. Keep
# the optional plugin and its redistributable runtime DLLs side by side.
set(_mr_cuda_runtime_files)
foreach(_component cudart cufft nvrtc nvrtc-builtins)
    file(GLOB _matches "${CUDAToolkit_BIN_DIR}/${_component}64*.dll")
    if(NOT _matches)
        message(FATAL_ERROR "Cannot locate CUDA runtime component ${_component}")
    endif()
    list(APPEND _mr_cuda_runtime_files ${_matches})
endforeach()
foreach(_runtime IN LISTS _mr_cuda_runtime_files)
    add_custom_command(TARGET mr_adm_room_cuda POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_runtime}" "$<TARGET_FILE_DIR:mr_adm_room_cuda>"
        VERBATIM)
endforeach()
set(MR_ADM_ROOM_CUDA_RUNTIME_FILES "${_mr_cuda_runtime_files}" CACHE INTERNAL "Optional ROOM CUDA dependencies" FORCE)

set(_mr_cuda_device_files)
if(MR_ADM_ROOM_CUDA_FUSE_TARGET)
    set(_mr_cuda_source "${PROJECT_SOURCE_DIR}/scripts/research/room_gpu/session_kernels.cu")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mr_cuda_source}")
    file(SHA256 "${_mr_cuda_source}" _mr_cuda_source_sha256)
    string(SUBSTRING "${_mr_cuda_source_sha256}" 0 16 _mr_cuda_target_id)
    target_compile_definitions(mr_adm_room_cuda PRIVATE
        ROOM_GPU_PRECOMPILED_TARGET
        ROOM_CUDA_TARGET_ID="${_mr_cuda_target_id}")

    add_executable(mr_adm_room_cuda_compile EXCLUDE_FROM_ALL src/adm_render_room/cuda/compile_target.cpp)
    target_compile_features(mr_adm_room_cuda_compile PRIVATE cxx_std_20)
    target_link_libraries(mr_adm_room_cuda_compile PRIVATE CUDA::nvrtc)
    if(MSVC)
        target_compile_options(mr_adm_room_cuda_compile PRIVATE /W4 /EHsc /utf-8)
    endif()
    foreach(_runtime IN LISTS _mr_cuda_runtime_files)
        if(_runtime MATCHES "[/\\\\]nvrtc")
            add_custom_command(TARGET mr_adm_room_cuda_compile POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_runtime}" "$<TARGET_FILE_DIR:mr_adm_room_cuda_compile>"
                VERBATIM)
        endif()
    endforeach()
    file(GLOB_RECURSE _mr_mathdx_headers CONFIGURE_DEPENDS
        "${MR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR}/*.h"
        "${MR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR}/*.hpp")
    if(NOT MR_ADM_ROOM_CUDA_ARCHITECTURES)
        message(FATAL_ERROR "ROOM CUDA target FFT requires at least one SM architecture")
    endif()
    set(_mr_cuda_architectures ${MR_ADM_ROOM_CUDA_ARCHITECTURES})
    list(REMOVE_DUPLICATES _mr_cuda_architectures)
    foreach(_arch IN LISTS _mr_cuda_architectures)
        if(NOT _arch MATCHES "^[0-9]+$")
            message(FATAL_ERROR "Invalid ROOM CUDA SM architecture: ${_arch}")
        endif()
        set(_device_file "${CMAKE_CURRENT_BINARY_DIR}/room_cuda/target-${_mr_cuda_target_id}-sm${_arch}.ptx")
        add_custom_command(OUTPUT "${_device_file}"
            COMMAND mr_adm_room_cuda_compile
                "${_mr_cuda_source}" "${_device_file}" "${_arch}"
                "${MR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR}" "${_mr_cuda_toolkit_include}"
            DEPENDS mr_adm_room_cuda_compile "${_mr_cuda_source}" ${_mr_mathdx_headers}
            VERBATIM)
        list(APPEND _mr_cuda_device_files "${_device_file}")
        # Multi-config generators may place the DLL in a configuration directory.
        if(CMAKE_CONFIGURATION_TYPES)
            add_custom_command(TARGET mr_adm_room_cuda POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:mr_adm_room_cuda>/room_cuda"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_device_file}" "$<TARGET_FILE_DIR:mr_adm_room_cuda>/room_cuda/"
                VERBATIM)
        endif()
    endforeach()
    add_custom_target(mr_adm_room_cuda_device_code DEPENDS ${_mr_cuda_device_files})
    add_dependencies(mr_adm_room_cuda mr_adm_room_cuda_device_code)
    set(MR_ADM_ROOM_CUDA_TARGET_SOURCE_SHA256 "${_mr_cuda_source_sha256}"
        CACHE INTERNAL "Source identity of the target FFT device programs" FORCE)
    set(MR_ADM_ROOM_CUDA_MATHDX_LICENSE "${MR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR}/../LICENSE.txt"
        CACHE FILEPATH "License accompanying the build-time MathDx SDK")
endif()
set(MR_ADM_ROOM_CUDA_DEVICE_FILES "${_mr_cuda_device_files}"
    CACHE INTERNAL "Optional ROOM CUDA precompiled target FFT device programs" FORCE)
