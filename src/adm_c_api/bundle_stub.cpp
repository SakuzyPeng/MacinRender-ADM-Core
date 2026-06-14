// 占位翻译单元 —— 仅为 mradm_capi_bundle(SHARED)提供一个源文件让链接成立。
// 真正的导出符号(adm_*)来自 force_load / --whole-archive 进来的 mr_adm_c_api 静态库,
// 见 CMakeLists.txt 的 MR_ADM_BUILD_CAPI_BUNDLE 目标。此文件刻意不包含任何代码。
