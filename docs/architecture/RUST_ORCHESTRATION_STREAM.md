# 文件流式与监听的打开迁入 Rust（五期 5.3）

> 五期 5.3 的记录（2026-11-03 至 2026-11-05）。路线见[审计五期一节](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)，
> 驱动面清单见 [5.1 记录](RUST_ORCHESTRATION_BASELINE.md)，离线编排见 [5.2 记录](RUST_ORCHESTRATION_OFFLINE.md)。

## 范围

5.3 把文件流式渲染的驱动面移入 `rust/crates/mradm-engine` 的 `stream` 模块：打开、分步 seek 与取消点、覆盖校验与应用、
朝向、room 的尾声与控制，以及实时监听打开流的规则。一致性工具 `mr_adm_phase2_stream` 换成 Rust。

| 批次 | 原 C++ | Rust |
| --- | --- | --- |
| A | 四个流式壳：`speaker_pcm.cpp` 的 `FileStream`（VBAP / HOA / EAR）、`triple_balance_stream.cpp`、`room_renderer.cpp` 的 `RoomStream`、`binaural_renderer.cpp` 的 `BinauralStream`；各后端 `open_stream` 中的格式检查（HOA 输入格式、Triple Balance D 模式与流格式、room 的帧数 / 声道 / 采样率） | `stream::open` 与统一的分步接口：`process`、`seek_begin` / `seek_step`（各后端的取消点与文字照原样）、`validate_overrides` / `set_overrides`、朝向、格式查询、room 尾声与控制；FFI `mradm_engine_stream_*` |
| B | `monitor_session.cpp` 的 `RealtimeStreamFactory::open`：D 模式拒绝、缺省布局 `0+2+0`、渲染器选择与 DS 路由校验、场景句柄检查、矩阵解析、`monitor` 模块下的诊断、屏幕检查、后端准备、系统空间监听的朝向置零；`renderer_factory` 的 `resolve_renderer` / `validate_direct_speakers_routing` / `resolve_direct_speakers_matrix` | `stream::open_monitor`（离线与监听共用 `render` 里由请求组后端请求与矩阵路由的辅助函数）；FFI `mradm_engine_monitor_open` / `_take_stream` / `_destroy` |
| B | `tests/tools/phase2_stream.cpp` | `mradm-ffi` example `phase2_stream`（与 `phase2_render` 共用 `examples/phase2/io.rs` 的写出代码） |

### C++ 剩下的部分

- **`render_common::EngineStream`**（`src/adm_render_common/speaker_pcm.{h,cpp}`）：
  - 实现 `IRenderStream`，每次 seek 步骤之前传入取消标志；
  - `open_engine_stream` 供各后端的 `open_stream` 调用，`adopt_engine_stream` 供监听工厂包装 Rust 交出的流；
  - 流错误的 context 仍由 C++ 按 location 用原生路径拼写，Windows 字节不变。
- **各后端 `open_stream`**：只剩一行调用，`dynamic_cast` 失败的文字与 Triple Balance 计划级的 D 模式检查留在 C++，保持原顺序。
- **监听工厂**（`src/adm_engine/monitor_session.cpp`）：组请求记录，调 FFI，按顺序转发日志，包装 Rust 流；Apple 后端作为岛，由 C++ 组 `RenderPlan`、创建适配器并打开。`RequestTexts` 与 Apple 适配器的创建移到 `src/adm_engine/request_record.h`，离线与监听共用。
- **`renderer_factory`**（46 行）：只剩 `normalize_output_layout`，供系统空间 sink 的布局选择使用。
- **`PreviewSession`**（`render_service.cpp` 约 25 行）：保持 C++ 外壳（用户决定）。它只缓存策略后的场景与准备句柄，每个窗口调用一次 Rust 渲染状态机，没有自己的规则；C ABI 迁移时（六期）再处理。

### 保持不变的规则

- **字节与文字**：PCM、检查点、错误码、文字、context、日志、取消点逐项相同。
- **取消点**：扬声器流在一步 seek 之前检查一次（“stream seek cancelled”）；Triple Balance 在开始前与每一步之前检查，finish 并入最后一步；room 只在步与步之间检查；双耳在开始前、每一步之前与最后一步之后检查。
- **并发**：覆盖校验只借用不可变的校验器（Triple Balance 的源、room 的 scope），仍可与 worker 的处理重叠。
- **检查点**：每个名字只归一方；**棘轮**：只降，每批在同一提交运行 `--update`。

### 用户决定

- **2026-11-03**：`PreviewSession` 保持 C++ 外壳；5.3 分 A、B、C 三批，每批单独提交，对照通过后再做下一批。
- **2026-11-04**：五期加入 5.8“一致性工具与命名清理”，生产源码注释里的切片编号改写为功能描述；原 5.8 两平台验收顺延为 5.9（见审计五期一节）。
- **2026-11-05**：B 的收尾中 ctest 偶发报出 MonitorEngine 原有的竞态，顺手修掉（见发布说明）。

## 发布说明

1. **A**：
   - 删除按种类的流 FFI：`mradm_render_stream_*`、`mradm_render_triple_balance_stream_*`、`mradm_render_binaural_stream_*`、`mradm_render_room_stream_*` 与 `mradm_render_room_validate_overrides`，以及它们的句柄与设置记录；
   - 删除 `triple_balance_stream.cpp` 与 `render_state.h`，`EnginePrepared` 只剩句柄（格式检查读 Rust 的准备结果）。
2. **B 的路径**：
   - **Windows**：监听的矩阵标签与准备阶段的 context 改为 UTF-8，归入 5.2 C2 记录的 Windows 路径例外类别；
   - **严格校验**：监听请求里非 UTF-8 或含 NUL 的路径被拒绝，与离线相同。
3. **B 的其他变化**：
   - 删除 `mradm_engine_resolve_renderer` / `_validate_routing` / `_resolve_matrix` 及 `EngineRenderer`、`EngineMatrixRequest`、`EngineResult.matrix`；
   - `direct_speakers_matrix_test` 中调用这三个函数的路由范围检查删除，由 request-v1 黄金数据覆盖；
   - 工具检查 room 夹具名时按路径组件判断，写法与 C++ 工具略有不同，锁定矩阵中观察不到；
   - MonitorEngine 的 worker 先写失败日志、再以 release 置位 `failed_`，`status()` 以 acquire 读取，所以调用方看到失败状态时日志一定已写入。可观察的行为不变，只消除了测试偶发先于日志读取的竞态。
4. **维护者修正**：`ab66752` 为监听工厂的 `BackendKeepAlive` 成员加 cppcheck `unusedStructMember` 注释，`61b9fac` 把注释压回棘轮上限内（净行数不变）。

## 黄金数据

| 数据 | 源提交 | 内容 | 植入并被抓到的缺陷 |
| --- | --- | --- | --- |
| `mradm-engine/tests/monitor-v1` | `297c7da` | 2,960 例监听打开（渲染器 × 布局 × 路由 × 矩阵、D 模式、屏幕、系统空间、平滑、spread、SOFA、room、LFE、几何），记录日志与错误或打开的流 | `open_monitor`：4 处（缺省布局、屏幕检查顺序、诊断模块名、系统空间朝向） |

- 生成器在 scratchpad 中，按行号抽出当时的 `RealtimeStreamFactory::open`，不提交。SOFA 用例在不带 `sofa` feature 的构建中跳过。
- A 的 `tests/stream.rs` 有 4 个测试（各后端的打开错误与取消点），植入 4 处缺陷都被抓到：Triple Balance 少一处步内检查、room 在开始前多查一次、双耳最后不查、HOA 格式检查漏掉采样率。
- B 的工具植入 5 处缺陷，3 处被对照报出（扬声器标签、朝向轴、平滑）。另 2 处在设计上观察不到：流的输出与请求分块无关；数值 spread 只影响 SAF spreader 的拒绝，矩阵里没有这种情况。

## 验证

本机对照都在 Linux x64、config A 的 Release 构建上进行，基线是每批开始时的 main。

| 批次 | 对照 | 结果 |
| --- | --- | --- |
| A（`9d6ce93`，基线 `3945b16`） | 矩阵的 36 个文件流式行，plain 与 diag；918 个流式变体（事件、seek 目标、编辑与比例、被拒的编辑、朝向、分块、平滑，711 例渲染），与旧 `mr_adm_phase2_stream` 对照 | 相同（2,486 个检查点）；退出码、stderr 与两遍 PCM 逐字节相同 |
| B（`3aa9e22`，基线 `297c7da`） | 新旧 C++ 监听工厂各驱动 2,960 例，打开的 282 条流在非静音输入上渲染、seek、再渲染；同一构建上 C++ 工具与 `--tool mr_adm_phase2_stream=<Rust>` 各采集 plain 与 diag（`--baseline`），用 `compare-collections` 比较；918 个流式变体 | 全部相同，含 282 条流的 PCM；plain 的 outputs 291、comparisons 979、replay_status 176、artifacts 256、日志 656 相同，diag 的 artifacts 31,229、日志 665 相同；0 差异 |

- **常规检查**：两批 ctest 都是 82/82 连跑 3 次，B 另把 `mr_adm_realtime_tests` 重复 50 次；clippy、fmt、FFI 头校验（B 后 341 个函数、145 个结构体）、clang-format / tidy、许可、retention、version 与 manual 目标都通过。
- **维护者合入**：A 随 5.8 规划一并合入（`297c7da`），B 合入为 `79a176f`，之后是上节的两个 cppcheck 修正。
- **三平台 Consistency**：每次合并后都通过，各次运行的 Compare platforms 均成功。

  | 合并点 | 运行 |
  | --- | --- |
  | A（`297c7da`） | 41794387787 |
  | B（`79a176f`） | 41987602401 |
  | B 修正（`61b9fac`） | 41994125083 |

  `ab66752` 的运行 41993504034 被取消，由 `61b9fac` 那次覆盖。最后一次运行在 config A、B 的普通与诊断四份报告中，都是 291 个 PCM id 相同、0 个不同、平台集完整、门禁失败 0。
- **待你本地验证**：macOS 上的 Apple 后端监听与系统空间监听，Windows 上监听的路径文字。

## 规模

C/C++（`language-surface.py`）：

| 合并点 | 生产 | 测试 | `src/adm_engine` | `src/adm_render_common` | `src/adm_renderer_factory` | 说明 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 5.2（`3945b16`） | 29,805 | 49,008 | 1,981 | 3,802 | 230 | 5.3 基线 |
| A（`297c7da`） | 29,408 | 49,008 | 1,981 | 3,802 | 230 | 四个流式壳合为 `EngineStream`；后端 `open_stream` 缩为一行 |
| B（`79a176f`） | 29,297 | 48,741 | 2,049 | 3,806 | 46 | 监听打开规则移出；`RequestTexts` 移入 `request_record.h`；C++ 工具删除 |
| B 修正（`61b9fac`） | 29,297 | 48,741 | 2,049 | 3,806 | 46 | 只改注释 |

- **累计**：5.3 共减少生产 C/C++ 508 行，五期至今 32,551 → 29,297（10.0%）。
- **按后端**：A 中 `src/adm_render_triple_balance` 257 → 101、`src/adm_render_room` 417 → 305、`src/adm_render_binaural` 404 → 278、`src/adm_render_hoa` 59 → 56。
- **`src/adm_engine` 增加 68 行**：`RenderService` 的 `RequestTexts` 移入共享头并补上 Apple 适配器的创建，监听工厂多了岛的打开代码；`render_service.cpp` 同时减少 114 行。
- **Rust**：`mradm-engine` 9,116 → 10,432 行（含 monitor-v1 的测试代码）。
- **岛**（3,213 行）与 CUDA 边界（512 行）不变。
