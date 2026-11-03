# 离线编排迁入 Rust（五期 5.2）

> 五期 5.2 的记录（2026-10-31 至 2026-11-02）。路线见[审计五期一节](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)，
> 驱动面与岛接口清单见 [5.1 记录](RUST_ORCHESTRATION_BASELINE.md)。

## 范围

5.2 把离线渲染的编排整体移入新 crate `rust/crates/mradm-engine`（`#![forbid(unsafe_code)]`），并把一致性工具
`mr_adm_phase2_render` 换成 Rust。CLI 与 C ABI 仍经 C++ 薄壳调用，六期再迁。

| 批次 | 原 C++ | Rust |
| --- | --- | --- |
| A | `capability_json`、`layout_table`、`format_table`、`input_layout_json`、`support_matrix`、`scene_json`，各后端 `*_capabilities()` 的数据 | `mradm-engine` `catalog`、`tables`、`scene_json`：6 个 Rust 后端的能力表与发现文档（capabilities、layouts、output formats、input layouts、support matrix、inspect 场景 JSON），与 nlohmann `dump(2)` 逐字节相同 |
| B | `renderer_factory` 的布局别名、渲染器选择、DS 路由校验与矩阵解析；`RenderService` 的 trim 窗口与输出目标判定 | `request`；fmt 的 `{:.6g}` 由 `fmt::general` 实现 |
| C1 | 各后端 `prepare` 中剩下的 C++ 检查，以及 6 个 `render_window` 循环（`render_speaker_pcm`、双耳、room） | `backend`（统一的准备入口 `mradm_engine_prepare`）与 `offline`（分步离线驱动） |
| C2 | `RenderService::render`：选项校验、导入与策略、语义报告、输出判定与临时文件、准备与渲染、后处理、编码调度、安装、WAV 布局收尾与元数据 | `render`（分步状态机）；`{:.1f}` / `{:.2f}` / `{:+.2f}` / `{:.4f}` 由 `fmt::fixed` 实现；UTC 日期由 Rust 换算 |
| D | `tests/tools/phase2_render.cpp` | `mradm-ffi` example `phase2_render`（与 `phase2_kernels` 同一种 CMake 目标） |

### C++ 剩下的部分

- **`RenderService::render` 的循环壳**（`src/adm_engine/render_service.cpp`，640 行）：
  - 组请求记录（路径一律 UTF-8）；
  - 反复调用 `mradm_engine_render_next`，每一步之前传入取消标志；
  - 转发进度与日志；
  - 执行引擎交出的岛请求：FLAC / Opus MKA / APAC / IAMF 编码器与 MP4 打包、打包器探测、FLAC / Matroska / MP4 元数据，以及 Apple 后端的 `prepare` 与 `render_window`（由场景句柄建 `AdmScene`）；
  - 岛内的进度经 `mradm_engine_render_map_progress` 映射。
- **`catalog.cpp`**：组装平台事实（IAMF、APAC、Apple 能力表、系统空间布局、Apple LFE 自检），并把 Rust 文档解析回 CLI 用的布局 / 格式行，这是 nlohmann 在 engine 里仅剩的用途。
- **`renderer_factory`**（230 行）：按 Rust 返回的 backend 构造 `IRenderer`。
- **其他**：
  - `PreviewSession` 与 `prepare_preview_scene` 归 5.3，策略改调 `mradm_engine_apply_policy`；
  - 各后端的 `IRenderer::prepare` 壳与 `open_stream`；
  - `RoomProfile` / `inspect_room` 薄包装。

### 保持不变的规则

- **字节与文字**：PCM、检查点、错误码、文字、context、日志、进度阶段与 fraction、取消点逐项相同；发现文档、语义报告与模板逐字节相同。
- **检查点**：每个名字只归一方，离线路径上 C++ 已不再写出任何检查点。
- **棘轮**：只降，每批在同一提交运行 `--update`。

### 用户决定

- **2026-11-01**：
  - 调用岛用分步状态机：Rust 的 `next()` 依次交出进度、日志或岛请求，C++ 执行后用 `resume()` 交回结果；
  - A–E 五批，每批单独提交，对照通过后再做下一批；
  - 5.2 删除 Rust 后端的 C++ `render_window` 与 `render_speaker_pcm`。
- **2026-11-02**：C 拆成 C1、C2，C2 一批完成。
- **主机验收**：改为 macOS arm64 + Windows x64，本机新旧对照在 Linux x64 容器中进行。

## 发布说明

1. **A**：
   - 删除 `register_vbap_layout` 与自定义 VBAP 布局注册表（没有生产、C ABI 或 GUI 调用者，`include/adm` 不是对外接口，ADR 0021 §4）；
   - C++ 的场景 JSON 写出器移到 `tests/support/scene_json`，只给 libadm 冻结参考使用。
2. **B**：MP4 打包器的探测提前到输出目标检查之前，只在 IAMF 构建里给出 `--iamf-container mp4` 时运行。原来更早的检查失败时不会探测，现在会多一次子进程，没有可观测的输出差异。
3. **C1**：
   - `IRenderer::render_window` 改为非纯虚，缺省返回 unsupported，只有 Apple 后端实现它；
   - 只测 `render_window` 自身一致性检查的 C++ 测试删除（RenderService 总是用同一份计划准备和渲染）；
   - 离线驱动不接受 `.flac` 渲染目标（RenderService 不会这样调用）。
4. **C2 的路径**：
   - **Windows**：消息与 context 里的非 ASCII 路径改为 UTF-8，包括 C1 的 `render_offline`。这归入 4.8 已记录的 Windows 路径例外类别。
   - **严格校验**：请求里的文件系统路径必须是不含 NUL 的 UTF-8，否则报 invalid_argument “invalid render request (UTF-8 paths without NUL required)”；预览与导出的策略路径报 “semantic policy path must use UTF-8 without NUL”。在 Linux 上，非 UTF-8 路径原本就不受支持，只是报错的时机和文字变了（维护者修正 `cc718b5`）。
5. **C2 的其他变化**：
   - 策略文件按二进制读取，Windows 上 CRLF 不再转换。只可能影响语法错误正文里的位置，正文的变化在 4.4 已经允许。
   - 预览与导出里的策略错误作为结果返回。
   - 准备缓存不再依赖预导入场景（`cc718b5`）。
   - “preparing backend” 进度在后端开始工作之前发出（`cc718b5`）。
6. **删除的私有导出**：
   - B 的 `mradm_engine_trim_window` / `_output_target`；
   - `mradm_scene_apply_policy` / `_policy_warnings` / `_report` 与 `SceneReportOptions`；
   - C1 中各后端的 `mradm_render_*_prepare`、`mradm_render_offline_*`、`mradm_render_binaural_offline_*` 及其记录。
7. **移到 `tests/support` 的头**：`wav_output.*`、`post_gain*.h` 只剩测试使用。
8. **D**：`mr_adm_phase2_render` 的选项解析有两处差别，在锁定矩阵里观察不到：
   - `--object-smoothing-frames` 只接受纯整数（`std::stoul` 曾容忍前导空白与尾随字符）；
   - 含 `.` 路径段的夹具名先规范化再使用（原来拒绝）。
9. **维护者修正**：
   - `dff4563`：覆盖文档对齐 VBAP 内置布局；
   - `a97958e`：`tests/support/scene_json` 加入 clang-tidy、cppcheck 与 check-changed 的排除表；系统空间布局只在 capabilities 文档里构建；
   - `47615fe`：清理 cppcheck 诊断；
   - `b8fc9dd`：矩阵黄金测试与宿主路径分隔符无关；`engine_ffi.h` 的 cppcheck 抑制；`resolve_renderer` 改为接收 `const std::string&`；
   - `cc718b5`：严格路径校验、准备缓存与预导入场景无关、准备进度先于后端工作；`render_service_test` 增加回归用例。

## 黄金数据

各批删除 C++ 之前，用 scratchpad 里的临时生成器（不提交）冻结 C++ 的输出。

| 数据 | 源提交 | 内容 | 植入并被抓到的缺陷 |
| --- | --- | --- | --- |
| `mradm-engine/tests/catalog-v1` | `28eac3f` | 经 C ABI 取得的 5 份发现文档，以及 30 个 inspect 输入（24 份 JSON、6 个拒收） | A：7 处 |
| `mradm-engine/tests/request-v1` | `47615fe` | 布局写法 49、渲染器解析 432、路由 864、矩阵解析 4,550（25 个场景 × 13 份矩阵）、`{:.6g}` 8,045 个值 | B：8 处 |
| `mradm-engine/tests/render-v1/fixed.txt` | fmt 11.2 | `{:.1f}` / `{:.2f}` / `{:+.2f}` / `{:.4f}` 共 8,363 行（随机位模式、中点进位、±0、nan / ±inf、次正规数） | 与 C2 的 4 处共用 |

C1、C2 用模拟岛驱动 `tests/offline.rs`、`tests/backend.rs`、`tests/render.rs`，各植入 4 处缺陷，都被抓到。D 植入 4 处缺陷，都被对照报出。

## 验证

本机对照都在 Linux x64、config A 的 Release 构建上进行，基线是每批开始时的 main。

| 批次 | 对照 | 结果 |
| --- | --- | --- |
| A（`65d6233`，基线 `28eac3f`） | 4 个目录子命令的全部过滤组合，共 155 个 CLI 变体 | 逐字节相同 |
| B（`62e737c`，基线 `47615fe`） | request 对照 1,364 例（渲染器 × 布局 × 路由 × 矩阵、trim 边界、扩展名 × 容器 × 位深 × IAMF 分层；349 例渲染）；矩阵子集 79 个离线 + 36 个文件流式行 | 0 差异；0 个问题 |
| C1（`4234262`，基线 `b6733c7`） | 经 C ABI 的进度与日志探针 1,218 例（含逐点取消）；request、VBAP、HOA、EAR、Triple Balance、room、双耳与输出的 CLI 对照；矩阵子集 plain 与 diag | 事件、日志、结果与输出相同；0 差异；0 个问题 |
| C2（`08596b2`，基线 `13643c2`） | 进度探针 2,997 例（872 例渲染，67,794 个进度事件）；CLI 对照 request 1,364、output 460、VBAP 1,148、HOA 886、EAR 2,096、Triple Balance 992、room 1,014、双耳 948、export 176、模板 17；矩阵子集 plain 与 diag | 全部相同（唯一的差别是一个 MKA 的大小差 1 字节，基线自身重复运行也会出现，来自现有的随机 TrackUID 长度）；0 个问题 |
| D（`7c72a70`，基线 `5723c86`） | 同一构建上，C++ 工具与 `--tool mr_adm_phase2_render=<Rust>` 各采集一份，用 `compare-collections` 比较；diag 构建 79 个离线用例直接对照（含 `MR_ADM_TRACE_DIR`）；切换后再与保存下来的 C++ 工具对照 | outputs 291、fixtures 208、comparisons 979、replay_status 176、artifacts 256、日志 656 全部相同；退出码、stdout、stderr、两遍 pcmbits 与 13,252 个检查点相同；79 例相同 |

- **常规检查**：每批 ctest 全部通过（C1、C2 连跑 3 次），clippy、fmt、FFI 头校验、clang-format / tidy、许可、retention、version 与 manual 目标都通过。
- **D 的 diag 对照**：本机的 diag 树使用已安装的依赖，采集脚本拒绝这种构建，所以改为直接对照。
- **维护者合入审查**：
  - D：macOS arm64 Release 上 79 个离线用例的退出码、stdout、stderr 与两遍 PCM 相同，工具测试 68 个通过、1 个跳过；
  - 其余各批随合并合入，含上节的修正提交。
- **三平台 Consistency**：每次合并后都通过，各次运行的 Compare platforms 均成功。

  | 合并点 | 运行 |
  | --- | --- |
  | A（`a8c40b2`） | 41270746703 |
  | A 修正（`47615fe`） | 41273031688 |
  | B（`b6733c7`） | 41320988064 |
  | C1（`13643c2`） | 41380202595 |
  | C2（`5723c86`） | 41416834273 |
  | D（`af322c5`） | 41455133059 |

  最后一次运行在 config A、B 的普通与诊断四份报告中，都是 291 个 PCM id 相同、0 个不同、平台集完整、门禁失败 0。

## 规模

生产 C/C++（`language-surface.py`）：

| 合并点 | 生产 | `src/adm_engine` | `src/adm_renderer_factory` | 说明 |
| --- | ---: | ---: | ---: | --- |
| 5.1（`28eac3f`） | 32,551 | 3,934 | 387 | 五期基线 |
| A（`a8c40b2`） | 31,360 | 2,744 | 387 | 目录与 JSON 移出 |
| A 修正（`47615fe`） | 31,353 | 2,736 | 387 | |
| B（`b6733c7`） | 31,174 | 2,558 | 230 | 请求判定移出 |
| C1（`13643c2`） | 30,517 | 2,563 | 230 | 6 个 `render_window` 与 `render_speaker_pcm` 删除 |
| C2（`5723c86`） | 29,805 | 1,981 | 230 | `RenderService::render` 改为循环壳 |
| D（`af322c5`） | 29,805 | 1,981 | 230 | 只改测试工具（测试 C/C++ 49,221 → 49,008） |

- **累计**：5.2 共减少生产 C/C++ 2,746 行（8.4%）。
- **`src/adm_render_common` 3,490 → 3,802**：私有 FFI 头 `engine_ffi.h`（434 行）声明了引擎的记录与函数，后端适配壳在 5.6 删除时随之缩小。
- **`src/adm_io` 429 → 381、`src/adm_metadata` 765 → 744**：策略应用与报告改到 Rust。
- **岛**（3,213 行）与 CUDA 边界（512 行）不变。
