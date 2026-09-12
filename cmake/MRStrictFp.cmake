# 受控数值构建开关（路线图 §5.1 第 3、4 项）。
#
# 阶段 0 要产出两组基线：
#   配置 A —— 当前默认构建，即今天三平台差异的实际状态；
#   配置 B —— 受控构建，测「只靠编译选项 + 关掉 SIMD 分派」能消掉多少差异。
# 两个开关默认 OFF，发行构建不受影响。它们是测量用的实验组，不是既定结论；是否转正由
# 阶段 0 的数据决定。
#
# 覆盖范围与已知盲区：
#   - 走 CMAKE_<lang>_FLAGS 而不是 add_compile_options()，因为前者按语言分开，不会漏进
#     依赖可能启用的汇编方言；作为普通变量它会传递给之后 add_subdirectory() 进来的目录，
#     所以 FetchContent 拉下来的依赖同样受控。
#   - 配置专用标志和 target_compile_options() 排在通用标志之后，能够覆盖它们。
#     STRICT_FP 开启时拒绝通用及活动配置中的冲突选项（包括 -Ofast），不依赖最后一个
#     标志取胜的偶然顺序。依赖追加的目标选项再由 build-info.sh 检查实际编译命令；
#     缺少严格标志或出现冲突均使构建记录失败，不能把它标作有效的受控基线。
#   - 系统预编译库（macOS Accelerate、Windows/Linux OpenBLAS）不受任何项目编译选项控制。
#     受控构建不能声称覆盖它们，实际后端由 scripts/consistency/build-info.sh 记录。
#
# 标志是否真的到达了编译器，以 compile_commands.json 为准（build-info.sh 会核对），不以
# 这里的意图为准。

option(MR_ADM_STRICT_FP
    "Controlled numeric build: disable FP contraction and fast-math (measurement only)" OFF)
option(MR_ADM_EAR_SCALAR_REFERENCE
    "Controlled numeric build: build libear without SIMD dispatch (measurement only)" OFF)

function(mr_adm_core_check_fp_flags variable_name)
    separate_arguments(flags NATIVE_COMMAND "${${variable_name}}")
    foreach(flag IN LISTS flags)
        string(TOLOWER "${flag}" lower_flag)
        if(flag STREQUAL "-Ofast"
           OR flag MATCHES "^(-ffast-math|-funsafe-math-optimizations|-fassociative-math|-ffinite-math-only|-freciprocal-math|-fno-signed-zeros)$"
           OR (flag MATCHES "^-ffp-contract=" AND NOT flag STREQUAL "-ffp-contract=off")
           OR lower_flag MATCHES "^/fp:(fast|contract)$")
            message(FATAL_ERROR
                "MR_ADM_STRICT_FP=ON conflicts with ${variable_name}: ${flag}. "
                "Remove the conflicting option or disable MR_ADM_STRICT_FP for the baseline build.")
        endif()
    endforeach()
endfunction()

function(mr_adm_core_apply_strict_fp)
    if(NOT MR_ADM_STRICT_FP)
        return()
    endif()

    set(configs "${CMAKE_CONFIGURATION_TYPES}")
    if(NOT configs AND CMAKE_BUILD_TYPE)
        set(configs "${CMAKE_BUILD_TYPE}")
    endif()
    foreach(lang IN ITEMS C CXX OBJC OBJCXX)
        mr_adm_core_check_fp_flags("CMAKE_${lang}_FLAGS")
        foreach(config IN LISTS configs)
            string(TOUPPER "${config}" upper_config)
            mr_adm_core_check_fp_flags("CMAKE_${lang}_FLAGS_${upper_config}")
        endforeach()
    endforeach()

    include(CheckCompilerFlag)
    set(fp_flags "")

    if(MSVC)
        # 显式记录 MSVC 的数值模式；继承的冲突选项已在前面拒绝。
        list(APPEND fp_flags "/fp:precise")
        # /fp:precise 下是否仍允许 contraction 随 MSVC 版本变化。若编译器接受显式的关闭
        # 开关就一并加上；接受与否由探测结果决定并记入构建记录，不靠文档推断。
        check_compiler_flag(CXX "/fp:contract-" MR_ADM_HAVE_MSVC_FP_CONTRACT_OFF)
        if(MR_ADM_HAVE_MSVC_FP_CONTRACT_OFF)
            list(APPEND fp_flags "/fp:contract-")
        else()
            message(STATUS "MR_ADM_STRICT_FP: 编译器不接受 /fp:contract-，仅使用 /fp:precise；"
                           "需在反汇编中确认是否仍有 FMA（scripts/consistency/scan-fp.sh）")
        endif()
    else()
        check_compiler_flag(CXX "-ffp-contract=off" MR_ADM_HAVE_FFP_CONTRACT_OFF)
        if(NOT MR_ADM_HAVE_FFP_CONTRACT_OFF)
            message(FATAL_ERROR
                "MR_ADM_STRICT_FP=ON 但编译器不接受 -ffp-contract=off。受控基线必须真的关掉"
                "contraction，静默降级会让配置 B 的结论无效。")
        endif()
        # -fno-fast-math 放在前面：显式的 contract 设置必须排在它之后才不会被重置。
        list(APPEND fp_flags "-fno-fast-math" "-ffp-contract=off")
    endif()

    string(REPLACE ";" " " fp_flags_str "${fp_flags}")
    foreach(lang IN ITEMS C CXX OBJC OBJCXX)
        set(CMAKE_${lang}_FLAGS "${CMAKE_${lang}_FLAGS} ${fp_flags_str}" PARENT_SCOPE)
    endforeach()
    message(STATUS "MR_ADM_STRICT_FP: ${fp_flags_str}")
endfunction()

mr_adm_core_apply_strict_fp()
