# 删除 C++ 外壳与 SDK（六期 6.5）

> 状态：进行中（A、B、C 批完成，2026-11-14）。路线见 [PCM 一致性审计六期一节](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)。

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

验证（本机 Linux x64 Release，一致性 config A，参照为 main `8e665a5` 的同配置构建）：
- CLI 记录 113 条 0 差异（参照已是 Rust CLI，help 文本也相同）；C ABI 采集 3,283 行、36 个文件 0 差异，consumer 测试在两个库上都通过；
- 完整一致性采集：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- 共享库与参照大小相同、未定义符号相同，恰好导出 164 个 `adm_*`；关闭测试（`MR_ADM_CORE_BUILD_TESTS=OFF`）的构建单独产出 `mradm` 与共享库，
  `cmake --install` 只装 `c_api.h`、共享库与 `mradm`（取依赖的 FLAC / opus / fmt / tl-expected 仍带出自己的安装规则，C 批处理）；
- ctest 85/85、FFI 头校验、质量脚本测试、参考登记、改动 C/C++ 的 clang-format / clang-tidy / cppcheck（移入的文件更新了 suppression 路径）；
- 规模：生产 C/C++ 15,314 → 11,380 行（`src/adm_engine` 只剩平台事实岛，`src/adm_io` / `src/adm_renderer_factory` 清空），`tests/support` 8,920 → 12,808 行。
  macOS / Windows 的原生构建待 CI 与维护者。

## B 批：失去调用者的私有 FFI

- **删除 21 个没有调用者的 `mradm-ffi` 导出**（C++、测试、工具与 Rust 都不调用）及其声明：`mradm_engine_normalize_layout`（布局别名，
  `renderer_factory` 删除后无人调用）、`mradm_engine_render_map_progress`、`mradm_dsp_live_binaural_*` 5 个（整个 `live_binaural.rs` 与
  `tests/support/binaural/live_binaural_ffi.h`；Rust 双耳 Scene 流式直接用 `mradm-dsp`）、`mradm_engine_monitor_open` / `take_stream` /
  `destroy`（监听打开 5.5 起在 Rust 会话内）、`mradm_room_prepare`、`mradm_dsp_spreader_delay`、Triple Balance D 模式 5 个与
  `mradm_dsp_tb_snapshot_capture`，以及 A 批移入后没有任何 include 的 `decimal.h` / `stream_dsp.h` / `timeline.h` 包装和它们的
  `mradm_dsp_parse_decimal` / `mradm_dsp_live_gain` / `mradm_dsp_seconds_to_frame`（一致性内核直接调用 `mradm-dsp`）。`mradm-ffi`
  导出 412 → 391 个。
- **私有 FFI 头按调用者拆分**：`src` 中 8 个手写头（`config_ffi.h`、`dsp_ffi.h`、`room_ffi.h`、`scene_math_ffi.h`、`adm_ffi.h`、
  `scene_ffi.h`、`engine_ffi.h`、`routing_ffi.h`）只留生产 C/C++（岛与进度跳板）用到的记录与入口，按 include 闭包从生产源码求出；
  其余声明原样移到 `tests/support/ffi/<名>_test_ffi.h`，测试头先 include 对应的生产头，测试源码改 include 测试头。例如 `dsp_ffi.h`
  只剩增益组与计量（120 → 10 条声明），`engine_ffi.h` 只剩能力记录、岛事件 / 回复与 Apple / 设备岛表（115 → 20）。
  冻结参考 `tests/reference/osc_protocol/legacy.h` 不改，由引用它的测试先 include `scene_math_test_ffi.h`。
- FFI 头校验列入 8 个测试头（391 个函数、163 个结构体）；`CppcheckSuppressions.txt` 对 `tests/support/ffi/*` 照原头抑制
  `unusedStructMember`。

验证（本机 Linux x64 Release，一致性 config A，参照同 A 批为 main `8e665a5`）：
- CLI 记录 113 条 0 差异；C ABI 采集 3,283 行、36 个文件 0 差异，consumer 测试在两个库上都通过；
- 完整一致性采集：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- 共享库比参照小 44 KB（死代码删除），导出 164 个 `adm_*`、未定义符号与参照相同；ctest 85/85、FFI 头校验、Rust fmt / clippy、
  改动 C/C++ 的 clang-format / clang-tidy / cppcheck；
- 规模：生产 C/C++ 11,380 → 9,768 行（`src` 的私有 FFI 头 2,676 → 1,063 行），`tests/support` 增加测试头，`mradm-ffi` 25,971 → 25,260 行。

## C 批：`include/adm` 只剩 `c_api.h`，岛的依赖

用户决定（2026-11-14）：`Result` 改为项目自有的最小类型；生产仍用的 C++ 头放到岛的私有目录。

- **头的去处**：`include/adm` 除 `c_api.h` 外的 12 个头（`audio_io`、`capability`、`direct_speakers_matrix`、`errors`、
  `live_override`、`logging`、`options`、`progress`、`render`、`render_apple`、`room`、`screen`）移到 `src/adm_core/adm`，
  include 写法仍为 `"adm/xxx.h"`，只对岛与测试目标可见、不安装；`render.h` 中只有测试使用的 `RenderService` /
  `PreviewSession` 与发现记录（`SceneProbe`、`OutputLayoutRow`、`OutputFormat*`）移到 `tests/support/sdk/include/adm/render_service.h`。
- **`Result`**：`mradm::Result<T>` 由 `tl::expected<T, Error>` 改为 `errors.h` 中的自有类型（值或 `Error`、`Result<void>`、
  `mradm::unexpected`、`value_or`、`emplace`、与值比较；无值时 `value()` 抛 `BadResultAccess`，同 `tl::bad_expected_access`）；
  `tl::unexpected` / `tl::make_unexpected` 全部改写为 `mradm::unexpected`，包括 10 个冻结参考（只改写法，`retention.json`
  同步更新哈希，算法不变；ebur128 参考的两处 `.map` 改为直接判断）。tl-expected 移出构建与许可证清单。
- **fmt**：生产只剩 Apple 岛、`flac_io.cpp`、`metadata_io.cpp` 共 18 处，改为字符串拼接；`{:.1f}` / `{:.2f}` 改用与 locale 无关的
  `std::to_chars` fixed，`{:08X}` 改为逐位十六进制。本机对 220 万个随机值与半数边界值核对与 fmt 11.2.0 逐字节相同（写入输出文件的
  `loudness=` / `peak=` 注释不变）。
- **测试专用依赖**：fmt 与 nlohmann_json 只在 `MR_ADM_CORE_BUILD_TESTS` 时获取（冻结参考、测试 SDK 与工具），清单中
  `distribution` 为 `test-only`、`cmake_option` 为 `MR_ADM_CORE_BUILD_TESTS`；生产不再链接 nlohmann_json（四期 4.4 起已不使用）。
- **安装面**：libFLAC 与 libopus 以 `EXCLUDE_FROM_ALL` 获取（CMake 3.28+），`cmake --install` 只装 `c_api.h`、共享库与 `mradm`。
- 质量脚本的 cppcheck 增加 `src/adm_core` 与 `tests/support/sdk/include`，使移动后的头能被解析。

验证（本机 Linux x64 Release，一致性 config A，参照同 A 批为 main `8e665a5`）：
- CLI 记录 113 条 0 差异；C ABI 采集 3,283 行、36 个文件 0 差异，consumer 测试在两个库上都通过；
- 完整一致性采集：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- 关闭测试的构建不获取 fmt / nlohmann_json / tl-expected，`cmake --install` 恰好安装 `include/adm/c_api.h`、`libmradm_capi.so*` 与 `bin/mradm`；
- ctest 85/85、FFI 头校验、参考登记、许可证校验与 SBOM、改动 C/C++ 的 clang-format（CI 的新版本规则，本机以 clang-format 23 核对）/ clang-tidy / cppcheck（cppcheck 与 main 同文件对照无新增）；
  Apple 岛的改动只能由 macOS CI 编译；
- 规模：生产 C/C++ 9,768 → 9,674 行（`Result` 自有实现 +98 行，`render.h` 测试专用部分移出 −208 行）。
