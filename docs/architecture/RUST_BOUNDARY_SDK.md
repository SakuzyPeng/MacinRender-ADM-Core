# 删除 C++ 外壳与 SDK（六期 6.5）

> 状态：已完成（2026-11-14），CI 三平台验收通过；维护者的 macOS arm64 / Windows x64 原生复核待办。路线见 [PCM 一致性审计六期一节](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)。

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

## D 批：Apple 岛不再经 `IRenderer` / `IRenderStream`

用户决定（2026-11-12）把 Apple 岛内部停用 C++ 后端接口推到 6.5。D 批起生产不再有 C++ 后端接口：

- **Apple 后端的入口**：`AppleRenderer`（`IRenderer`）改为三个函数 `apple::prepare` / `apple::render_window` / `apple::open_stream`，
  声明在新的 `src/adm_apple/apple_backend.h`。准备状态 `apple::Prepared` 只在 `spatial_mixer_renderer.cpp` 中定义，不再需要
  `dynamic_cast` 检查；`AppleStream` 去掉 `IRenderStream` 基类与 `override`，渲染、seek、回环、覆盖、朝向与两段式输出的实现不变。
- **流表**：`apple::open_stream` 直接返回 Rust Monitor 调用的 `MradmEngineIslandStream`。建表代码（错误缓冲、异常兜底、覆盖记录的
  换算、可取消 seek）从 `apple_island.cpp` 移到 `apple_backend.h`，以流类型为模板参数，在流的定义处实例化；表的行为与原来相同：
  不需要预滚（原 `seek_requires_preroll` 默认值）、覆盖检查总是通过（原 `validate_overrides` 默认值）、可取消 seek 在已请求取消时报
  `cancelled`、否则 seek 一次（原 `seek_with_cancel` 默认实现），异常照旧报状态 100。`apple_backend.h` 是 Rust 调用的流表所在，
  按岛接口登记到 `ISLAND_INTERFACES`（218 行；`apple_island.cpp` 353 → 166 行）。
- **接口移到测试**：`IRenderer`、`IRenderStream`、`IPreparedRender`、`EmptyPreparedRender` 从 `src/adm_core/adm/render.h` 移到测试 SDK
  的 `tests/support/sdk/include/adm/renderer.h`；只剩测试的 `EnginePrepared` 移到 `tests/support/sdk/render_common/engine_prepared.h`，
  它用的 `MradmRenderPrepared` / `mradm_render_prepared_destroy` 声明移到 `tests/support/ffi/engine_test_ffi.h`；只有测试使用的
  `Diagnostic`、`RenderResult`、`PreparedScene` 移到同一个 `renderer.h`。`render.h` 只剩 Apple 岛用的 `RenderPlan`、`RenderMetrics` 与
  窗口记录；`render_apple.h` 只剩能力表与 LFE 自检。
- **测试**：Apple 的冒烟、LFE 路由与 DirectSpeakers 矩阵测试仍以 `IRenderer` 驱动 Apple 后端，经新的
  `tests/support/apple_renderer`（`mr_adm_apple_renderer_test_support`，只在 macOS）：`prepare` / `render_window` 直接转调，
  流把岛表包装为 `IRenderStream`，因此测试经过的流路径就是生产给 Rust 的表。冻结参考 `triple_balance/render_state.h` 只加一行
  include，`retention.json` 同步哈希。

验证（本机 Linux x64，一致性 config A，参照同 A 批为 main `8e665a5`；对照的源码为 `ff8f814`，之后只把测试专用的结果记录移出
`render.h`，以 ctest 复核）：
- CLI 记录 113 条 0 差异；C ABI 采集 3,283 行、36 个文件 0 差异，consumer 测试在两个库上都通过；
- 完整一致性采集：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- ctest 83/83（本机 debug 树未开 C ABI bundle，少两个只在 bundle / Release 下登记的测试）、FFI 头校验、参考登记、改动 C/C++ 的
  clang-format（以 clang-format 23 核对）与 cppcheck（与 main 同文件对照无新增）；
- Apple 岛与 Apple 测试只能由 macOS CI 编译运行。本机以替身 `AudioToolbox.h` 对 `spatial_mixer_renderer.cpp`、`apple_island.cpp`、
  测试适配器与三个 Apple 测试做了只检查语法的编译；
- 规模：生产 C/C++ 9,674 → 9,432 行，岛 3,045 → 3,028 行。
- 分支 CI（macOS / Windows / Linux / ARM64）通过，macOS 首次编译运行 Apple 岛与三个 Apple 测试；Quality 首次因两个移到测试 SDK 的
  头被 cppcheck 单独检查报 `unusedStructMember` 失败，加与 FFI 头相同的行内抑制（`a5063a9`）后通过。

## E 批：三平台验收

对象为合并 A–D 后的 main `55cd757`（分支同一提交）：

| 工作流 | 结果 |
| --- | --- |
| CI（push） | 通过：macOS / Windows / Linux debug 与 Linux ARM64 Release |
| Consistency（push） | 通过：三平台 A/B 采集，`gates.json` 的全部内核与 294 个 PCM 用例跨平台位相等 |
| Quality（push，full） | 通过：Rust fmt / clippy、FFI 头校验、`check-all.sh` |
| Reference tests（手动） | 通过：各 `MR_ADM_BUILD_*_REFERENCE_TESTS` 对照 |
| Release Build（手动，不发布） | 修正后通过（`8e777d8`，run 43578966428）：macOS / Windows CLI 包、Linux AppImage、两个 GUI 包与各自 smoke |

- **Release 的修正**：五期结项（`2207538`）之后第一次运行 Release Build，macOS CLI 打包的依赖白名单拒绝
  `/usr/lib/libobjc.A.dylib`。`mradm` 经 Rust C ABI 链入设备岛，其中含 Objective-C++ 的 AVSampleBuffer 系统空间 sink，
  因此依赖 Objective-C 运行时；它是 macOS 系统库、不随包分发，`package-macos-cli-release.sh` 只放行这一项（GUI 包早已放行
  `/usr/lib/*`），Homebrew / `/usr/local` 检查不变（`8e777d8`）。Windows CLI、Linux AppImage 与两个 GUI 包（含 P/Invoke smoke）
  在修正前就已通过。
- **本机 Linux x64**：关闭测试的 Release（C ABI bundle）构建 `cmake --install` 恰好安装 `include/adm/c_api.h`、`libmradm_capi.so*` 与
  `bin/mradm`，不获取 fmt / nlohmann_json / tl-expected；`check-capi-exports.py` 确认共享库恰好导出 164 个 `adm_*`；各批的 CLI 记录、
  C ABI 采集与完整一致性采集对照见上文，全部 0 差异。
- **待维护者**：macOS arm64 与 Windows x64 的原生复核（规范构建、完整 CTest、Apple 后端与系统空间监听实机、GUI 包实机），照
  AGENTS.md 的主机验收；IAMF 启用配置。

## 规模

| 时点 | 生产 C/C++ | 岛 | 岛接口 |
| --- | ---: | ---: | ---: |
| 6.5 之前（6.4 结束） | 15,314 | 3,045 | 1,026 |
| A：测试专用外壳与 SDK 移出 | 11,380 | 3,045 | 1,026 |
| B：无调用者的 FFI 与测试专用声明 | 9,768 | 3,045 | 1,026 |
| C：`include/adm` 只剩 `c_api.h`、自有 `Result` | 9,674 | 3,045 | 1,026 |
| D：Apple 岛去 `IRenderer` / `IRenderStream` | 9,432 | 3,028 | 1,057 |

岛接口的增加是 D 批新登记的 `apple_backend.h`（218 行，流表从 `apple_island.cpp` 移入，后者 353 → 166 行）。测试侧随之增加
（56,241 → 62,400 行），只供测试的 SDK、头与适配器都在 `tests/support`。

移交 6.6（构建以 Cargo 为主、cdylib）：生产 C/C++ 只剩岛、编码器与容器元数据、两个进度回调跳板（`adm_c_api.cpp`，仍待另行决定）与
`src/adm_core/adm` 中岛共用的头（`render.h` 只剩 Apple 岛用的计划、计量与窗口记录）。`src` 的私有 FFI 头只保留生产 C/C++ 使用的记录与入口，
其余声明位于 `tests/support`、`tests/unit` 等测试目录。`mradm-ffi` 共保留 391 个函数导出，其中包含测试与工具使用的入口。
