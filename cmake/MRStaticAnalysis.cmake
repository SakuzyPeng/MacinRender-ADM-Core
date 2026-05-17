if(MR_ADM_CORE_ENABLE_CLANG_TIDY)
    find_program(
        CLANG_TIDY_EXECUTABLE
        NAMES clang-tidy
        HINTS
            /opt/homebrew/opt/llvm/bin
            /usr/local/opt/llvm/bin
    )
    if(NOT CLANG_TIDY_EXECUTABLE)
        message(WARNING "MR_ADM_CORE_ENABLE_CLANG_TIDY is ON, but clang-tidy was not found")
    endif()
endif()

if(MR_ADM_CORE_ENABLE_CPPCHECK)
    find_program(
        CPPCHECK_EXECUTABLE
        NAMES cppcheck
        HINTS
            /opt/homebrew/bin
            /usr/local/bin
    )
    if(NOT CPPCHECK_EXECUTABLE)
        message(WARNING "MR_ADM_CORE_ENABLE_CPPCHECK is ON, but cppcheck was not found")
    endif()
endif()

function(mr_adm_core_apply_static_analysis target_name)
    if(MR_ADM_CORE_ENABLE_CLANG_TIDY AND CLANG_TIDY_EXECUTABLE)
        set(clang_tidy_args
            "${CLANG_TIDY_EXECUTABLE}"
            "--config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy"
            "-p=${CMAKE_BINARY_DIR}"
        )

        if(APPLE)
            execute_process(
                COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE MR_ADM_CORE_MACOS_SDK_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            list(APPEND clang_tidy_args "--extra-arg=-target" "--extra-arg=${CMAKE_SYSTEM_PROCESSOR}-apple-macos")
            if(MR_ADM_CORE_MACOS_SDK_PATH)
                list(APPEND clang_tidy_args "--extra-arg=-isysroot" "--extra-arg=${MR_ADM_CORE_MACOS_SDK_PATH}")
            endif()
        endif()

        set_target_properties(
            ${target_name}
            PROPERTIES
                CXX_CLANG_TIDY "${clang_tidy_args}"
        )
    endif()

    if(MR_ADM_CORE_ENABLE_CPPCHECK AND CPPCHECK_EXECUTABLE)
        set_target_properties(
            ${target_name}
            PROPERTIES
                CXX_CPPCHECK
                    "${CPPCHECK_EXECUTABLE};--enable=warning,style,performance,portability;--std=c++20;--inline-suppr;--suppressions-list=${CMAKE_CURRENT_SOURCE_DIR}/CppcheckSuppressions.txt;--error-exitcode=2"
        )
    endif()
endfunction()
