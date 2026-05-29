function(mr_adm_core_find_iamf_aom_bridge)
    if(TARGET MacinRender::IamfAomBridge)
        return()
    endif()

    set(_mr_iamf_hints)
    if(MR_ADM_IAMF_AOM_ROOT)
        list(APPEND _mr_iamf_hints "${MR_ADM_IAMF_AOM_ROOT}")
    endif()

    find_library(MR_ADM_IAMF_AOM_BRIDGE_LIBRARY
        NAMES mr_iamf_aom_bridge
        HINTS ${_mr_iamf_hints}
        PATH_SUFFIXES lib lib64
        NO_CACHE)

    if(NOT MR_ADM_IAMF_AOM_BRIDGE_LIBRARY)
        message(FATAL_ERROR
            "MR_ADM_ENABLE_IAMF=ON requires the official AOM iamf-tools prebuilt bridge. "
            "Set MR_ADM_IAMF_AOM_ROOT to a directory containing lib/libmr_iamf_aom_bridge.*")
    endif()

    add_library(MacinRender::IamfAomBridge UNKNOWN IMPORTED GLOBAL)
    set_target_properties(MacinRender::IamfAomBridge PROPERTIES
        IMPORTED_LOCATION "${MR_ADM_IAMF_AOM_BRIDGE_LIBRARY}"
    )

    message(STATUS "Using AOM iamf-tools bridge: ${MR_ADM_IAMF_AOM_BRIDGE_LIBRARY}")
endfunction()
