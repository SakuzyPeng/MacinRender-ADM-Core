# 删除适配壳与 C++ 场景（五期 5.6）

> 五期 5.6 的记录（2026-11-11）。路线见[审计五期一节](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)，
> 驱动面清单见 [5.1 记录](RUST_ORCHESTRATION_BASELINE.md)，Apple 岛此前的接口见 [5.3 记录](RUST_ORCHESTRATION_STREAM.md)。

## 范围

5.2–5.5 把离线、文件流式、Scene 流式与 Monitor 的编排移入 Rust 之后，生产 C++ 里还留着三类只为旧驱动面存在的代码：
- 各 Rust 后端的 C++ `IRenderer` 适配壳与对应的 SDK 头；
- C++ `AdmScene`：由 Rust 场景视图逐字段重建，只剩 Apple 岛、CLI `inspect` 与几处摘要在读；
- `src/adm_render_common` 中面向 `AdmScene` 块的路由与对象预处理包装，以及只剩测试调用的私有 FFI 声明。

5.6 把它们删出生产，或移到 `tests/support` 供测试与冻结参考使用。PCM、检查点、日志、错误文字、CLI 输出与 C ABI 不变。

| 批次 | 原 C++ | 现在 |
| --- | --- | --- |
| A（`172b8df`） | `vbap` / `ear` / `hoa` / `triple_balance` / `binaural` / `room` 六个后端的 `IRenderer` 壳；只剩测试在用的 `speaker_pcm`（准备、离线驱动、文件流式）与 `HrtfState`；公开头 `include/adm/render_{vbap,ear,hoa,triple_balance,binaural,room}.h` 与 5 个 CMake 库目标（用户决定：SDK 头在 5.6 移出） | 壳与驱动移到 `tests/support/engine_renderer`（`mr_adm_engine_renderer_test_support`），测试照旧经 `create_*_renderer` 驱动；生产仍要的矩阵、屏幕、实时编辑与 `EnginePrepared` 记录留在 `src/adm_render_common/engine_records`；CLI `backends` / `layouts` 经新增的 `RenderService::backend_capabilities` 取能力表 |
| B（`691192e`，维护者修正 `e777cc9`、`d52f6e9`、`eb17077`） | Apple 岛的 `build_bus_plans` 与 `prepare` 里的检查、日志、文字与算术：DirectSpeakers 路由（标签 / 位置 / 矩阵 / LFE 旁路）、对象块的生效块、通道锁、发散与 extent 云、AU 坐标约定、22.2 LFE 判定 | `mradm-render` `apple::prepare`；岛经 `mradm_render_apple_prepare`（`src/adm_apple/apple_plan_ffi.h`）读回总线与事件，不再读 `AdmScene`，也不再调 C++ 路由包装 |
| C（`a058396`，维护者修正 `4d85c2c`） | `RenderPlan::scene`、`PreparedScene::scene`、`PreviewSession::scene()`、`io::import_scene`、`include/adm/scene.h`、`ADMMetadata`（`metadata.{h,cpp}`，由视图重建 `AdmScene`）、`src/adm_core/scene_math.cpp`、CLI `inspect` 的文本打印 | 生产只持有 Rust 场景句柄；`inspect` 文本在 `mradm-engine` `scene_json::inspect_text`（`mradm_scene_inspect_text`，`RenderService::inspect_text`）；`probe` 的计数、导入告警与 Monitor 会话的格式读场景视图；C++ 场景及其构建、导入、场景数学与块级包装移到 `tests/support/adm_scene`（`mr_adm_scene_test_support`） |
| C2（`fb7bd41`、`da93b7d`、`bc05730`） | `routing_ffi.h` 中只剩测试调用的声明：标签 / LFE / 目标索引 / 矩阵解析、`mradm_render_lfe_routing`、Scene 流式渲染器在引擎之外的入口（`mradm_engine_scene_renderer_create` 与 `mradm_render_live_*`）、双耳 HRTF 视图 | 测试侧 `tests/support/render_test_ffi.h`（纳入 `check-ffi-headers.py`）；Rust 导出不变 |

### C++ 剩下的部分

- **`IRenderer` / `IRenderStream` 接口**（`include/adm/render.h`）：Apple AUSpatialMixer 后端仍以 `IRenderer` 实现，文件流式经 `EngineStream` 适配为 `IRenderStream`，Monitor 经 `island_stream` 岛表取 Apple 流。后端适配壳只剩 Apple 这一个，属于平台岛。
- **`ILiveSceneRenderer`**：接口类 5.4 起已在 `tests/support/live_scene`；生产 `live_scene_renderer.h` 只剩 Scene 流引擎与 C ABI 用的值类型（`ObjectState`、`Frame`、`RendererConfig`、诊断）。
- **`src/adm_render_common`**（批次 C2 后）：`render_common.{h,cpp}` 只剩 Apple 岛与 Windows sink 用的实时编辑增益与 head-lock 查询、`LiveGainRamp`、22.2 LFE 常量与 `LfeRoutingPlan::gain`、`SerialWorker`；另有 `engine_records`、`capability_records`、`hptf_eq`、`speaker_layouts`（Windows sink 与 Scene 输出会话）、`scene_handle.h` 和私有 FFI 头 `engine_ffi.h` / `routing_ffi.h` / `layout_ffi.h`。
- **`src/adm_io`**：`io::RenderScene`（导入、策略模板、inspect JSON 与文本、摘要、导出回写的 C++ 入口）与输入布局表的视图。
- **`src/adm_metadata`**：只剩私有 FFI 头 `adm_ffi.h` / `scene_ffi.h`。

### 测试侧

- `tests/support/adm_scene`：
  - `adm/scene.h`：原 SDK 头，内容不变，冻结参考照旧以 `adm/scene.h` 引用；
  - `metadata.{h,cpp}`：由 Rust 视图与 overlay 重建 `AdmScene`（原 `ADMMetadata`），`metadata::generate` 只剩 libadm 参考在用；
  - `adm_scene.{h,cpp}`：测试侧 `io::import_scene`、`adm_scene(handle)`、`io::adm_scene(RenderScene)`，以及 `ScenePlan`（`RenderPlan` 加 C++ `AdmScene`，给需要场景期望值的测试与冻结参考）；
  - `scene_math.cpp`（原 `src/adm_core`）与 `render_common_scene.{h,cpp}`（块级路由与对象预处理包装）。
- 测试侧引擎驱动（`speaker_pcm.cpp`）从场景句柄读输入格式。HOA 的格式不一致用例原来改 `plan.scene.info.num_channels`，现在换成 6 声道 5.1 声道床的句柄。
- 冻结参考只改 include 与签名，算法未改，`retention.json` 哈希同步更新并在各迁移记录注明：
  - `triple_balance/render_state.{h,cpp}` 改收 `ScenePlan`；
  - `hoa/legacy.h`、`live_vbap/legacy.h` 多一行 `render_common_scene.h`；
  - `libadm/scene_importer.cpp` 多一行 `adm/scene.h`（维护者修正）。

## 发布说明

- **SDK 头移出**：`include/adm/scene.h` 与 `include/adm/render_{vbap,ear,hoa,triple_balance,binaural,room}.h` 不再安装；`mr_adm_metadata` 与 5 个后端库目标删除。外部代码不应依赖这些头；稳定接口仍是 `c_api.h`。
- **`RenderPlan` / `PreparedScene` / `PreviewSession`**：去掉 C++ 场景字段与 `scene()` 访问器，`RenderPlan::scene_handle` 是后端唯一的场景来源。
- **`io::import_scene`** 删除；`RenderService` 新增 `inspect_text` 与 `backend_capabilities`。
- CLI 与 C ABI 行为不变。

## 测试与植入缺陷

- **A**：CLI 新旧 44 份输出（含退出码）逐字节相同。
- **B**：新旧 Apple 计划探针把旧 C++ 计划与新 Rust 计划编进同一 Linux 程序，13 个夹具 × 8 个语义策略 × 全部 Apple 布局与 binaural × 4 种 DS 路由（有无矩阵）× spread 与 LFE 模式，共 7,000 种组合（2,400 个预期错误）。89,444 条总线、152,366 个事件的浮点逐位相同，日志与错误文字相同；植入的 7 处缺陷都被探针或新增的 9 个 `mradm-render` `apple` 测试抓到。
- **C**：
  - `mradm-engine` `tests/catalog.rs` 新增 `inspect_text_matches_the_cpp_cli`：重放 `tests/inspect-v1`，即 C++ CLI（`eb17077`）对 31 个输入的 `mradm inspect` 文本（`catalog-v1` 的 30 个输入，加覆盖节目 / 内容 / 对象元数据、参考屏幕、全部响度字段、舍入平局与未映射轨道的 `rich`），25 个可导入输入逐字节相同；植入的两处缺陷（距离精度、内容 dialogue 分支）都使测试失败。
  - 同一构建上新旧 `mradm` 对 100 个用例（`inspect` 与 `inspect --xml`；ADM、声道床、空文件、非 WAVE 与缺失文件）的 stdout、stderr 与退出码逐字节相同。
- **C2**：`check-ffi-headers.py` 通过（415 个函数、164 个结构体与 Rust 一致）。

## 验证

本机 Linux x64 debug：
- 全量 ctest（不含 `mr_adm_rust_unit_tests`）80/80 通过；A 时 `mr_adm_rust_unit_tests` 单独跑通过，C 只改了 `mradm-engine` `scene_json` 与 `mradm-ffi` `scene`，跑了 `catalog` 测试；
- `mr_adm_rust_quality`（rustfmt + clippy `-D warnings`）、`check-ffi-headers.py`、`check-reference-retention.py`、`format.sh --check`、`language-surface.py --check` 通过；
- Apple 岛与三个 Apple 门控测试（`apple_renderer_smoke_test`、`direct_speakers_matrix_render_test`、`lfe_routing_test`）在桩 AudioToolbox 头下 `clang -fsyntax-only -D__APPLE__ -Wall -Wextra` 0 错误 0 警告。这只说明类型对得上。

维护者已在本机验证并合并 A、B、C（含上列修正），并合并了 C2。

## 规模

生产 C/C++ 由 24,409 行（5.6 之前，`43908e8`）降到 21,776 行（−2,633）：A 后 23,413，B 不变（Apple 岛 3,213 → 3,045），C 后 21,963，C2 后 21,776。主要模块：

| 模块 | 5.6 之前 | 5.6 之后 |
| --- | --- | --- |
| `include/adm` | 4,389 | 3,985 |
| `src/adm_render_common` | 3,660 | 2,629 |
| `src/adm_render_{binaural,ear,hoa,triple_balance,vbap}` | 449 | 0（目录删除） |
| `src/adm_metadata` | 744 | 386（只剩 FFI 头） |
| `src/adm_core` | 350 | 200 |
| `src/adm_cli` | 1,642 | 1,410 |
| `src/adm_apple`（岛） | 2,504 | 2,336 |
