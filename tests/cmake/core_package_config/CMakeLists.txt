cmake_minimum_required(VERSION 3.24)
project(CorePackageConfigTest LANGUAGES NONE)

include(CMakePackageConfigHelpers)
set(package_prefix "${CMAKE_CURRENT_BINARY_DIR}/package")
set(dependency_prefix "${CMAKE_CURRENT_BINARY_DIR}/dependencies")
set(package_directory "${package_prefix}/lib/cmake/MacinRenderADMCore")
set(CMAKE_INSTALL_LIBDIR lib)
set(MR_ADM_INSTALLED_RUST_ARCHIVE libmradm_ffi.a)
set(Rust_CARGO_TARGET_LINK_NATIVE_LIBS "")
configure_package_config_file(
    "${MR_ADM_SOURCE_DIR}/cmake/MacinRenderADMCoreConfig.cmake.in"
    "${package_directory}/MacinRenderADMCoreConfig.cmake"
    INSTALL_PREFIX "${package_prefix}"
    INSTALL_DESTINATION lib/cmake/MacinRenderADMCore
)
file(WRITE "${package_directory}/MacinRenderADMCoreTargets.cmake" "")
file(WRITE "${package_prefix}/lib/${MR_ADM_INSTALLED_RUST_ARCHIVE}" "")

# Model dependencies installed under a different prefix. Before CMake 3.30,
# their generated package configs overwrite the caller's PACKAGE_PREFIX_DIR.
foreach(dependency IN ITEMS fmt tl-expected Threads)
    file(WRITE "${dependency_prefix}/${dependency}Config.cmake"
        "set(PACKAGE_PREFIX_DIR \"${dependency_prefix}\")\n"
        "set(${dependency}_FOUND TRUE)\n")
endforeach()
set(CMAKE_PREFIX_PATH "${dependency_prefix}")
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
find_package(MacinRenderADMCore CONFIG REQUIRED PATHS "${package_prefix}" NO_DEFAULT_PATH)
get_target_property(archive MacinRender::RustDSP IMPORTED_LOCATION)
set(expected_archive "${package_prefix}/lib/${MR_ADM_INSTALLED_RUST_ARCHIVE}")
if(NOT archive STREQUAL expected_archive OR NOT EXISTS "${archive}")
    message(FATAL_ERROR "Rust archive resolved to '${archive}', expected '${expected_archive}'")
endif()
