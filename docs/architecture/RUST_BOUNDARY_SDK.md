# 删除 C++ 外壳与 SDK（六期 6.5）

> 状态：进行中（A 批完成，2026-11-13）。路线见 [PCM 一致性审计六期一节](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)。

6.3 / 6.4 之后，公开 C ABI 与 `mradm` CLI 都由 Rust 实现，生产只需要 Rust 按名调用的岛（编码器与容器元数据、APAC worker、
Apple 后端、输出设备与系统空间 sink、平台事实）和两个进度回调跳板。6.5 把其余 C++（会话外壳、C++ SDK 头、其实现与打包）删出生产：
测试仍要用的部分移到 `tests/support/sdk`，照 5.6 的做法保留原目标名，测试链接行不变。

## 分批

| 批 | 内容 |
| --- | --- |
| A | 只剩测试使用的外壳与 SDK 移出生产：`RenderService` / `PreviewSession`、`MonitorSession`、`OscHeadTrackingReceiver`、场景导入、房间包、屏幕与 DirectSpeakers 矩阵加载、能力记录、HpTF 辅助、版本串，以及 `include/adm` 中只供它们使用的头；删除无人调用的 `renderer_factory`；生产的岛集中到新目标 `mr_adm_islands`；删除 C++ 静态库与头的安装、`MacinRenderADMCoreConfig` 包配置及其测试 |
| B | 失去调用者的私有 FFI：没有 C++、测试或工具调用的 `mradm-ffi` 入口与手写头一起删除，只剩测试调用的入口随头移到 `tests/support` |
| C | 岛的自有头与依赖：岛不再用 `include/adm` 的 C++ 类型（`Result` / `Error`、选项、进度与日志、能力记录），`include/adm` 只剩 `c_api.h`；tl-expected、fmt、nlohmann_json 移出生产（测试专用到七期） |
| D | Apple 岛内部不再经 `IRenderer` / `IRenderStream`（用户决定 2026-11-12 推到 6.5） |
| E | 三平台验收与记录 |

进度回调的两个 C++ 跳板（`adm_c_api.cpp`）照旧保留，C++ 回调抛出的异常仍转为 `ADM_ERROR_INTERNAL`，等另行决定。

## A 批：测试专用的外壳移出生产

- **移到 `tests/support/sdk`**（`git mv`，内容只改 include 路径）：
  - `engine/`：`render_service.cpp`、`monitor_session.cpp`、`catalog.{h,cpp}`、`request_record.h`；
  - `io/`：`scene_importer.cpp`、`scene_import_internal.h`；`room/`：`room.cpp`、`room_internal.h`；
  - `realtime/`：`osc_head_tracking.cpp`、`osc_ffi.h`；`core/`：`screen.cpp`、`direct_speakers_matrix.cpp`、`version.cpp.in`；
  - `render_common/`：`capability_records.*`、`hptf_eq.*`、`engine_result.h`、`ffi_error.h`、`live_scene_renderer.h`、`consistency_trace.h`；
  - `dsp/`：`dsp.h`、`hrtf.h`、`pcm_mix.h`、`semantic_policy_ffi.h` 与没有任何调用者的 `decimal*`、`stream_dsp*`、`timeline*`（B 批删除）；
  - `include/adm/`：`io.h`、`monitor.h`、`head_tracking.h`、`hptf.h`、`version.h`。
- **删除**：`src/adm_renderer_factory`（`normalize_output_layout` 已无调用者）、`cmake/MacinRenderADMCoreConfig.cmake.in` 与
  `tests/cmake/core_package_config`。
- **目标**：生产只剩 `mr_adm_core`（只含头的 INTERFACE）、`mr_adm_render_common`（Apple / Windows 岛用的增益斜坡、矩阵记录、
  串行 worker 与扬声器布局表）、`mr_adm_audio`、`mr_adm_apple`、`mr_adm_windows`、新的 `mr_adm_islands`（平台事实、输出设备岛与
  miniaudio 设备）、`mr_adm_c_api`（进度回调跳板，链接岛与 Rust 库，不再安装）、共享库与 `mradm`。`mr_adm_engine`、`mr_adm_io`、
  `mr_adm_render_room`、`mr_adm_realtime` 只在构建测试时定义，另有 `mr_adm_sdk_core` / `mr_adm_sdk_render_common`；测试目标以
  `include_directories` 看到 SDK 头。
- **安装**：只装 `c_api.h`、共享库 `mradm_capi`（开启 `MR_ADM_BUILD_CAPI_BUNDLE` 时）与 `mradm`；不再安装 C++ 静态库、C++ 头与 CMake 包
  （用户决定 2026-11-12：直接使用内部 C++ 头的下游不再支持）。

验证：本机 Linux 对照进行中（见下一提交）。

<!-- VERIFY-A -->
