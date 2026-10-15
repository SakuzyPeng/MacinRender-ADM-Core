# Corrosion 0.6.1 adds the Command Line Tools SDK to imported Rust libraries.
# That -L path takes precedence over the SDK selected by the C/C++ compiler:
# an Xcode linker can then read a newer, incompatible CLT libSystem.tbd.
# Let the linker use its selected SDK (including CMAKE_OSX_SYSROOT overrides),
# while retaining Rust's native libraries and any other library search paths.
function(mr_adm_rust_use_platform_sdk target_name)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        return()
    endif()

    get_target_property(link_directories "${target_name}" INTERFACE_LINK_DIRECTORIES)
    if(link_directories)
        list(REMOVE_ITEM link_directories "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib")
        set_property(TARGET "${target_name}" PROPERTY INTERFACE_LINK_DIRECTORIES "${link_directories}")
    endif()
endfunction()
