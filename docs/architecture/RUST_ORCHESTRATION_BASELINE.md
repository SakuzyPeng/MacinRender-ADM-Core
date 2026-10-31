# 方向与基线（五期 5.1）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)五期 5.1，路线见
> [Rust 主导逆转路线](RUST_INVERSION_ROADMAP.md)，四期结项见 [RUST_INVERSION_CLOSEOUT.md](RUST_INVERSION_CLOSEOUT.md)。
> 本切片只改一致性脚本、测试与文档，PCM、内核输出与生产代码都不变。

## 方向

五期把四期留在 C++ 的驱动面迁入 Rust：`RenderService` / `PreviewSession`、后处理编排、`SceneStreamEngine` /
`SceneOutputSession`、`MonitorEngine`、ring buffer、OSC UDP 收发与快照、设备调度。退出条件见审计：一致性工具改为 Rust，
并在同一构建上证明新旧工具输出逐位相同；C++ 的 `AdmScene` 与适配壳删除。

用户决定（2026-10-31）：
- 照审计中初拟的顺序推进（5.1 → 5.8），5.1 起切片表确定；
- 不新增 ADR，五期的规则写在审计、路线图与本记录里，ADR 0021 的方向不变；
- 编排与实时调度放进新 crate `mradm-engine`（5.2 新建），依赖 `mradm-render` / `mradm-scene` / `mradm-wav`；
  `mradm-render` 继续只放后端与公共渲染层；
- 5.1 带上新旧工具的对照工具（下文“对照工具”）。

## 驱动面清单

### 一致性工具

| 工具 | 产出 | 驱动的 C++ 接口 | 五期去向 |
| --- | --- | --- | --- |
| `mradm render`（CLI） | 离线 PCM | `RenderService::render` | CLI 仍是 C++ 壳（六期迁），5.2 起经它调用 Rust 编排；不可替换 |
| `mr_adm_phase2_render` | 离线同进程重复 | `RenderService` | 5.2 改为 Rust |
| `mr_adm_phase2_stream` | 文件流式重放 | `io::RenderScene::import`、内部 `resolve_renderer`、`IRenderStream`、`live_override` | 5.3 改为 Rust |
| `mr_adm_phase2_scene` | Scene 重放 | `SceneStreamEngine`（时钟缝）、`SceneOutputSession`（捕获设备） | 5.4 改为 Rust |
| `mr_adm_phase2_kernels` | 内核测量 | 已是 Rust（`mradm-ffi` example） | 不变 |
| `mr_adm_make_fixture` / `mr_adm_pcm_bits` / `mr_adm_repeat_render` | 夹具、PCM 抽取、重复渲染 | 测试工具 | 随所驱动的接口迁移 |

### CLI 子命令

| 子命令 | 调用 |
| --- | --- |
| `render` | `RenderService::render`（另读屏幕配置） |
| `inspect` | `RenderService::write_policy_template` |
| `export` | `RenderService::export_file` |
| `formats` / `input-layouts` | `RenderService::output_formats` / `input_layouts_json` |
| `layouts` | `RenderService::output_layouts` 与各后端的能力表 |
| `backends` | 各后端的能力表（`*_capabilities()`） |
| `__apac-encode` | APAC 编码子进程（Apple 岛） |

### C ABI（164 个导出）

| 家族 | 个数 | engine 对象 | 五期切片 |
| --- | ---: | --- | --- |
| `adm_render_*`、render options / result 的 create 与 destroy、cancel token 的 create / destroy / reset、`adm_cancel` | 59 | `RenderService` | 5.2 |
| `adm_inspect_file_*`、`adm_probe_file`、`adm_scene_info_*`、`adm_export_file`、`adm_policy_template_json`、`adm_capabilities_json`、`adm_layouts_json`、`adm_output_formats_json`、`adm_input_layouts_json` | 16 | `RenderService` 与能力表 / support matrix / 场景 JSON | 5.2 |
| `adm_room_*` | 5 | `RoomProfile` / `inspect_room` | 5.2 |
| `adm_preview_*`、preview session 的 create / destroy | 4 | `PreviewSession` | 5.3 |
| `adm_scene_*`（不含 `scene_info`）、`adm_create_scene_stream*` / `adm_create_scene_output` 与 destroy | 38 | `SceneStreamEngine` / `SceneOutputSession` | 5.4 |
| `adm_monitor_*`、`adm_create_monitor*`、`adm_destroy_monitor`、`adm_hptf_parse_parametric_eq` | 27 | `monitor_session` / `MonitorEngine` | 5.5 |
| `adm_osc_*`、OSC 头追踪的 create / destroy | 8 | `OscHeadTrackingReceiver` | 5.5 |
| `adm_api_version_*`、context 的 create / destroy、`adm_context_last_error_message`、`adm_free_string` | 7 | 版本、上下文与内存 | 六期 |

按 `include/adm/c_api.h` 声明的函数名分类，共 164 个，与 `check-capi-exports.py` 校验的导出数相同。
C ABI 本身在六期由 Rust cdylib 实现，五期内各家族经 C++ 薄壳调用 Rust 编排，签名与语义不变。

## 岛接口清单

Rust 编排在五期要调回 C/C++ 的点。5.1 只列出，接口在各切片实现时确定，方向一律是“岛提供窄 C 接口、Rust 调用”：

| 岛 | 现在的 C++ 入口 | 调用方 | 计划 |
| --- | --- | --- | --- |
| FLAC / Opus MKA / IAMF / APAC 编码与 MP4 udta | `convert_to_flac` / `convert_to_opus_mka` / `convert_to_iamf` / `convert_to_apac`、`write_file_metadata`（`src/adm_audio`，经 Rust reader 读已写完的 float WAVE） | `RenderService` | 5.2：窄 C 接口，保持“读 float WAVE、写目标文件”的形状；编码器岛归七期 |
| Apple AUSpatialMixer 后端 | `apple_renderer`（`IRenderer`，`src/adm_apple`） | 渲染器工厂 | 5.2 / 5.6：岛实现窄 C 渲染接口，路由与预处理经 Rust |
| 系统空间监听 sink | `make_avsamplebuffer_device`（macOS）、`make_spatialaudioclient_device`（Windows） | `monitor_session::make_monitor_device` | 5.5：设备接口窄 C 化 |
| miniaudio 设备 | `make_miniaudio_device`（`src/adm_realtime/miniaudio_device.cpp`） | Monitor、Scene 输出 | 5.5：留作 C 岛 |
| Windows room worker 等待（MMCSS） | `room_worker_wait` / `room_worker_active` | Monitor / room worker | 5.5 |
| CUDA | `room_cuda_selection`（已在 `mradm-ffi`） | room 准备 | 不变 |

## 规则

- **工具切换。** 新的 Rust 工具先与旧工具在同一构建上，用 `compare-collections.py` 对完整采集达到 0 差异，才替换构建目录里的同名工具；
  切换后旧工具删除。替换期间的采集带 `tool_overrides`，不能作为三平台证据。
- **调用方向。** CLI 与 C ABI 在五期内经 C++ 薄壳调用 Rust 编排（`mradm-engine`），六期再迁；不为测试在 C ABI 中新增入口。
- **检查点。** 每个检查点名仍只归一方，迁移时随代码转到 Rust，名称与内容不变。
- **棘轮与验收。** 迁出即删除，生产 C/C++ 只减不增（同一提交 `language-surface.py --update`）；每个切片以 Linux x64 本机新旧对照为完成条件，
  三平台验收集中进行，结果回填。

## 对照工具

- `run-rust-phase2.py --tool NAME=PATH`（可重复）：只能替换 `mr_adm_phase2_render`、`mr_adm_phase2_stream`、`mr_adm_phase2_scene`；
  名字未知、重复、文件不存在或不可执行时拒绝。构建目录里的原二进制照旧通过 build stamp 校验；manifest 的 `binaries` 记替换文件的摘要，
  另记 `tool_overrides: {name: {path, sha256}}`（不替换时为 `{}`）。`compare-rust-phase2.py` 拒绝带替换的采集。
- `compare-collections.py <reference> <candidate> [--output report.json]`：比较同一构建、同一平台的两份完整采集。
  - 前提（不符时退出码 2）：schema、source、config、diagnostics、平台（`platform.system` / `platform.machine`）与用例清单相同；
  - 比较（不同时退出码 1）：outputs（路径与 PCM sha256）、fixtures（含 Scene 工具写出的输入与事件表）、comparisons、replay_status、
    artifacts（kernels、checkpoints、kernel-checkpoints），以及 `logs/` 下每个文件；
  - 日志只做一项规范化：把各自采集根目录的绝对路径换成 `<collection>`；
  - 不同的检查点报告第一个不同的字。
- 单元测试在 `tests/unit/phase2_tools_test.py` 的 `ToolSubstitutionTests`（随 ctest 运行）。

## 规模基线

`8ef7017` 的生产 C/C++ 为 32,551 行（岛 3,213 行、CUDA 边界 512 行另计）。五期范围内的模块：

| 模块 | 行数 | 五期去向 |
| --- | ---: | --- |
| `src/adm_realtime` | 5,437 | `SceneStreamEngine`（2,436）5.4；`MonitorEngine`（1,372）、OSC（647）、ring buffer、downmix、峰值保护、room worker 等待 5.5；miniaudio 设备（215）留作 C 岛 |
| `src/adm_engine` | 3,934 | `render_service`（1,403）、能力表 / support matrix / 格式与布局表 / 场景 JSON（约 1,400）5.2；`scene_output_session`（439）5.4；`monitor_session`（630）5.5 |
| `src/adm_renderer_factory` | 387 | 5.2 |
| `src/adm_render_common` | 3,490 | 5.6（`routing_ffi.h` 759、`live_rust_renderer` 244，其余为 Apple 岛用的薄包装） |
| 后端适配壳（binaural / room / triple_balance / vbap / ear / hoa） | 1,963 | 5.6 |
| `src/adm_core`、`src/adm_metadata`、`src/adm_io` | 356、765、429 | 5.6（`AdmScene` 视图与文件编排） |
| `src/adm_dsp` | 1,994 | 私有 FFI 头随 C++ 调用点删除 |
| `src/adm_audio` | 4,184 | 编码器（约 2,700）留作岛（七期）；薄包装随 5.2 缩小 |
| `include/adm` | 4,426 | `c_api.h`（2,008）归六期；其余内部头随调用方删除 |
| `src/adm_c_api`、`src/adm_cli` | 3,544、1,642 | 六期 |

五期结束时，生产 C/C++ 预计只剩 `c_api.h` 与 C ABI 实现、CLI、编码器与 miniaudio 的窄接口；岛不计入。以各切片实测为准。

## 验证

Linux x64 本机，`build/consistency-a`（config A 普通构建，`8ef7017` 加本切片的脚本）：

| 采集 | 替换 | `compare-collections`（对照不替换的采集） |
| --- | --- | --- |
| 自对照 | 三个回放工具都换成复制到别处的同一批二进制 | 0 差异，退出码 0：outputs 291、fixtures 208、comparisons 979、replay_status 176、artifacts 256、日志 656 项全部相同 |
| 植入 PCM 差异 | `mr_adm_phase2_stream` 换成包装脚本，`stream-binaural-point-*` 两遍输出的最后一字节翻转 | 退出码 1：outputs 2 项与 comparisons 7 项不同，其余相同 |
| 植入日志差异 | 同上，`stream-binaural-point-fixed` 多打印一行 | 退出码 1：日志 2 项（两个进程）不同，其余相同 |

- 自对照说明日志里除采集根目录外没有易变的文字；每份采集约 2 分钟、292 MB。
- 带替换的采集在 manifest 里记录三个工具的摘要；`compare-rust-phase2.py` 的校验拒绝它，不带替换的采集照常通过。
- 检查点差异需要诊断采集。本机的诊断构建树用已安装的依赖（`MR_ADM_CORE_USE_INSTALLED_DEPS=ON`），采集脚本拒收，所以检查点一项只由单元测试覆盖（第一个不同的字）。
- `phase2_tools_test.py` 全部通过，新增 `ToolSubstitutionTests` 8 个测试；生产代码没有改动，棘轮不变。
