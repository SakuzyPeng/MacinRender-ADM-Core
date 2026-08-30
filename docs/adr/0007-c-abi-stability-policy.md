# ADR 0007：C ABI 稳定性承诺与版本策略

> 状态：已接受（已进入阶段 2，当前 ABI 为 stable v1.35）
> 日期：2026-05-17（增量记录持续更新至 2026-08-30 的 v1.35）
> 适用范围：`adm_c_api` 模块（`include/adm/c_api.h` 与 `src/adm_c_api/`），以及任何通过该 ABI 的下游绑定（GUI（图形用户界面）、Rust CLI、Python/Node/Swift 绑定）。`adm_core` 与 `adm_render*` 的 C++ 内部 API 不受本 ADR 约束。

## 背景

`include/adm/c_api.h` 已经在 M1 阶段定义了最小 C ABI：

- 2 个 opaque struct（`adm_context_t`、`adm_render_result_t`）
- 1 个 enum（`adm_error_code_t`，6 个值）
- 1 个 callback typedef（`adm_progress_cb`）
- 6 个导出函数

这套接口在 M2/M3 实现阶段会持续演化——`adm_render_file` 大概率要补 RenderOptions 表达、result 句柄要补声道数/时长/响度等查询函数、可能新增 `adm_probe_file` 等。如果在演化期间放任改 signature、改 enum 值、改 struct 字段顺序，第一波接入的绑定方（特别是 Swift GUI）每次升级都要重写胶水代码，迁移成本不可控。

但另一方面，**M2/M3 完成前就锁死 ABI 是过早承诺**——领域模型尚未稳定，错误码可能漏掉关键场景，RenderOptions 字段尚未对齐 BS.2127 全集。

本 ADR 划清"实验期" 与"稳定期"的边界，定义稳定后的版本策略、兼容规则与 deprecation 流程，避免在 M2 中途因"是不是要走稳定流程"而反复纠结。

## 决策

C ABI 走 **两阶段稳定** 模型：

### 阶段 1：实验期（M1 ~ M3）

- 现行所有 C ABI 符号视为 **不稳定（experimental）**，下游不应假设跨 commit 二进制兼容。
- 在 `include/adm/c_api.h` 顶部加注释明确：

  ```c
  /*
   * STATUS: experimental.
   * 本 C ABI 在 M3 完成（首个端到端渲染闭环跑通）前不承诺二进制兼容。
   * 升级到任何 0.x.y 版本都可能要求重新编译绑定方。
   * 稳定承诺与版本策略：docs/adr/0007-c-abi-stability-policy.md
   */
  ```

- 项目版本号保持 `0.x.y`（CMake `project(... VERSION 0.x.y)`），不发布 SONAME。
- 允许任意修改 signature、enum 值、struct 字段、callback 形参。

### 阶段 2：稳定期（M3 完成后）

- M3 验收（`libear` 后端最小闭环跑通、至少一个 golden fixture 可端到端渲染）后正式宣布 ABI v1。
- 版本号切到 `1.0.0`，CMake 与共享库 SONAME 同步：

  ```cmake
  set_target_properties(mr_adm_c_api PROPERTIES
      VERSION ${PROJECT_VERSION}        # 例如 1.2.3
      SOVERSION ${PROJECT_VERSION_MAJOR})  # 例如 1
  ```

- 引入版本宏（在 `include/adm/c_api.h` 顶部）：

  ```c
  #define ADM_API_VERSION_MAJOR 1
  #define ADM_API_VERSION_MINOR 0
  #define ADM_API_VERSION_PATCH 0
  #define ADM_API_VERSION ((ADM_API_VERSION_MAJOR * 10000) + \
                           (ADM_API_VERSION_MINOR * 100)   + \
                            ADM_API_VERSION_PATCH)
  ```

  绑定方可用 `#if ADM_API_VERSION >= 10200` 做条件编译。
- 新增运行时查询函数：

  ```c
  int adm_api_version_major(void);
  int adm_api_version_minor(void);
  int adm_api_version_patch(void);
  ```

  允许动态加载（dlopen）场景验证版本。

## 稳定后的兼容规则（v1 之后）

### 允许（不破坏 ABI）

- 在 `adm_error_code_t` enum 末尾追加新值。
- 新增导出函数（保留旧函数）。
- 在 opaque struct 内增删字段（因为下游不能依赖布局）。
- 给函数新增 `noexcept` 保证（不影响调用方）。
- 文档与注释修改。

### 禁止（破坏 ABI）

- 修改现有函数 signature（参数、返回值、调用约定）。
- 修改 `adm_error_code_t` 中已有值（重排或删除）。
- 修改非 opaque struct 的字段顺序、类型或大小。
- 修改 callback `adm_progress_cb` 形参或调用约定。
- 修改宏 `ADM_API_VERSION_*` 的含义。
- 删除导出函数（必须走 deprecation 流程，见下）。

如需任一禁止变更，必须发布 `2.0.0`（主版本升级），SONAME 同步切到 `.so.2`，旧版本可在过渡期共存。

### Deprecation 流程

- 标记为 deprecated 的函数在 header 中加注释 + `__attribute__((deprecated))`（GCC/Clang）或 `__declspec(deprecated)`（MSVC）：

  ```c
  ADM_API_DEPRECATED("使用 adm_render_file_v2() 替代")
  adm_error_code_t adm_render_file(...);
  ```

- Deprecated 函数 **至少保留 2 个 minor 版本**（例如在 1.3 标记 deprecated，最早在 1.5 才可删除），给绑定方迁移窗口。
- Major 版本升级（2.0）可一次性删除所有已 deprecated 函数。

## 变更记录（实际发布的 ABI 演化）

记录已发布的 ABI 表面变化，便于绑定方查阅。所有变更均遵循上述兼容规则。

### v1.0.0（首个稳定版本）

阶段 2 切换时冻结的最小表面：

- opaque：`adm_context_t`、`adm_render_result_t`
- enum：`adm_error_code_t`（`ADM_ERROR_OK`..`ADM_ERROR_INTERNAL`，0..6）
- callback：`adm_progress_cb`
- 函数：版本查询 ×3、`adm_create_context` / `adm_destroy_context`、`adm_render_file`、
  `adm_destroy_render_result`、`adm_render_result_error_code` / `adm_render_result_message`

### v1.1.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

全部为新增符号 / opaque 内部扩展，**未触碰任何 v1.0 已有 signature、enum 值或 callback**，
因此是 minor 升级。`include/adm/c_api.h` 顶部新增 `#include <stdint.h>`（C 标准头，符合写法约束）。

- **新 enum**（值从 0 起，镜像对应 C++ 枚举，末尾可继续追加）：
  `adm_renderer_t`、`adm_output_bit_depth_t`、`adm_speaker_spread_mode_t`、
  `adm_binaural_spread_mode_t`、`adm_iamf_container_t`、`adm_log_level_t`。
  每个加 `static_assert(sizeof == sizeof(int))`，并在 `adm_c_api.cpp` 加 enum 值 ↔ C++ 枚举的
  交叉 `static_assert`。
- **新 opaque**：`adm_render_options_t`（options builder 句柄）、`adm_scene_info_t`（probe 摘要句柄）。
- **Options builder**：`adm_create_render_options` / `adm_destroy_render_options` + 一组 setter，
  覆盖 CLI 全部用户选项（renderer、output_layout、bit_depth、loudness/peak、bitrate、interp、
  smoothing、spread mode、iamf container、sofa/semantic 路径）。校验失败的 setter 返回
  `adm_error_code_t`（非法 enum / NaN-inf / 越界 → `ADM_ERROR_INVALID_ARGUMENT`，OOM →
  `ADM_ERROR_INTERNAL`），范围与 CLI（`src/adm_cli/render_command.cpp`）保持一致；不会失败的
  POD setter 仍返回 `void`。
- **Render**：`adm_render_file_ex`（带 options；`opts==NULL` 等价全默认）。旧 `adm_render_file`
  转调 `_ex` 传 NULL，行为完全不变。
- **Result 访问器**：`adm_render_result_output_path`、`adm_render_result_loudness_lufs` /
  `adm_render_result_peak_dbtp`（返回 1/0 表达 optional），以及诊断日志
  `adm_render_result_log_count` / `adm_render_result_log_entry`（渲染期捕获的
  warning/info/debug 行，字符串归 result 句柄所有）。
- **Probe**：`adm_probe_file` / `adm_destroy_scene_info` + 6 个标量访问器（sample_rate、
  channels、frames、duration、programme/object count），经 `RenderService::probe` 实现，
  不让 ABI 直接依赖 `ADMIo`（守 ADR 0003）。
- **Scene inspect（JSON）**：`adm_inspect_file_json` 解析整树并返回 JSON 字符串（schema 1:1
  镜像 `mradm inspect`：file / programmes / contents / objects（逐 track / 逐 block）/
  hoa_tracks / import_warnings），`adm_free_string` 释放。经 `RenderService::inspect_json`
  实现（守 ADR 0003）；序列化用 nlohmann/json，但**全程 TU-local PRIVATE**，对外只返回
  `std::string`，无第三方类型跨边界（守 ADR 0003 / 0004）。该函数引入下述新所有权惯例。
  JSON root 带稳定标识 `"schema": "mradm.scene-inspect"` / `"schema_version": 1`。
  另有 `adm_inspect_file_xml`（镜像 `mradm inspect --xml`）返回 BWF 内嵌的原始 `<axml>`
  ADM XML 块（verbatim），经 `RenderService::axml`（转调 `io::get_axml`）实现，同样用
  `adm_free_string` 释放、`out_xml==NULL` 仅校验。
- **Policy template（JSON）**：`adm_policy_template_json`（镜像 `mradm inspect
  --write-semantic-policy-template`）返回某场景的可编辑中性 semantic-policy 模板 JSON
  （root 带 `"schema": "mradm.semantic-policy.v1"`，是合法 policy 文档），GUI 可在内存编辑后
  再经 `adm_render_options_set_semantic_policy_path` 回灌。经 `RenderService::policy_template_json`
  实现（转调 `build_semantic_policy_template`），同样 `adm_free_string` 释放、`out_json==NULL` 仅校验。
- **Capabilities（JSON）**：`adm_capabilities_json` 枚举可用 renderer 后端及其能力，返回 JSON
  字符串（schema 1:1 镜像 `mradm backends`：每个后端的 feature flags + supported layouts，
  并带 `"renderer"` 字段对应 `adm_render_options_set_renderer` 的取值），`adm_free_string`
  释放。经 `RenderService::capabilities_json` 实现（守 ADR 0003），同样 nlohmann TU-local
  PRIVATE、对外只 `std::string`。JSON root 带 `"schema": "mradm.capabilities"` /
  `"schema_version": 1`。
- **Layouts（JSON）**：`adm_layouts_json` 返回输出声道顺序参考表（schema 1:1 镜像
  `mradm layouts`：每个 format+layout 的 channels / container / order / note，外加派生的
  `supported_by`——哪些 renderer 支持该布局），`adm_free_string` 释放。经
  `RenderService::layouts_json` / `output_layouts` 实现；声道顺序表搬到引擎
  （`src/adm_engine/layout_table.cpp`）做**单一数据源**，CLI `mradm layouts` 与 C ABI 共用，
  消除原 CLI 本地表的漂移风险。nlohmann TU-local PRIVATE、对外只 `std::string`。
  JSON root 带 `"schema": "mradm.layouts"` / `"schema_version": 1`。

### v1.2.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

只新增两个 options setter，**未触碰任何已有 signature、enum 值或 callback**，因此是 minor 升级。

- **输出区间裁剪 setter**：`adm_render_options_set_render_start_sec` /
  `adm_render_options_set_render_end_sec`，在渲染时间线（等于输入时间线）上裁出 `[start, end)` 段。
  `start` 需有限且 `>= 0`；`end` 取 `sec <= 0` 表示"清空 / 渲染到结尾"（对应 C++ 侧 `optional` 的
  `nullopt`），正值才设置，非有限 → `ADM_ERROR_INVALID_ARGUMENT`。`end > start` 的关系在渲染时校验
  （与 CLI `--start` / `--end` 同一引擎路径）。`opts==NULL` 按既有惯例返回 `ADM_ERROR_OK`（no-op）。
- **响度/真峰口径**：后端对裁剪窗口内联计量，故裁剪后文件的 `adm_render_result_loudness_lufs` /
  `adm_render_result_peak_dbtp` 与写入的元数据均描述被保留段（不是完整渲染）；无额外文件遍历，
  无裁剪时与既有行为 bit-exact。
- 字段经 `adm_render_file_ex` 直接透传到 `RenderService`，**未新增 render 入口**。

### v1.3.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

只新增一个 options setter，**未触碰任何已有 signature、enum 值或 callback**，因此是 minor 升级。

- **最终增益 setter**：`adm_render_options_set_final_gain_db`，设置一个**不受限制**的最终增益（dB），
  在所有自动增益 staging（loudness 归一 / peak makeup / peak limit）**之后**无条件施加。它**故意绕过
  peak limit**——在 peak clamp 计算完成后才叠加，因此可把信号推过峰值上限与 0 dBFS（integer 输出可能
  削顶，由调用方负责）。`0` 为 no-op。**不设范围上限**（符合"不受限制"语义），但非有限值（NaN/inf）→
  `ADM_ERROR_INVALID_ARGUMENT`。`opts==NULL` 按既有惯例返回 `ADM_ERROR_OK`（no-op）。
- **口径**：增益被并入引擎的合并增益一次施加，故裁剪/响度处理后的 `adm_render_result_loudness_lufs` /
  `adm_render_result_peak_dbtp` 与写入的元数据均**反映含 final gain 后**的实际文件电平。
- 字段经 `adm_render_file_ex` 直接透传到 `RenderService`，**未新增 render 入口**。

### v1.4.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

新增一个 opaque 类型、四个生命周期函数与一个 options setter，**未触碰任何已有 signature、enum 值或
callback**（已冻结的 `adm_progress_cb` 维持原样、未改返回值），因此是 minor 升级。面向 GUI 的「取消」按钮：
此前 `ADM_ERROR_CANCELLED` 已在 enum 中定义但引擎无任何代码路径可产生它，本次将其打通为可达。

- **取消句柄**：新增 opaque `adm_cancel_token_t`（内部包 `std::stop_source`）与
  `adm_create_cancel_token` / `adm_destroy_cancel_token`（严格配对）/ `adm_cancel`（请求取消，幂等）/
  `adm_reset_cancel_token`（换入新 `stop_source` 以复用 token）。所有函数对 `NULL` 均为安全 no-op。
- **关联到渲染**：`adm_render_options_set_cancel_token` 把 token 以**借用指针**（非持有）挂到 options 上；
  token 必须存活至引用它的渲染返回。`token==NULL` 清除关联（渲染变为不可取消），`opts==NULL` 为 no-op。
  C 侧 options 仅保存指针，在 `adm_render_file_ex` 内才 `get_token()` 解析出 `std::stop_token`，
  确保 `adm_reset_cancel_token` 换源后总能被下一次渲染观察到。
- **线程语义**：与 `adm_context_t`「单线程 / 外部串行化」不同，`adm_cancel` **显式允许跨线程**——可与正在
  另一线程执行、引用同一 token 的 `adm_render_file_ex` 并发调用（依托 `std::stop_source`/`std::stop_token`
  的并发契约）。`adm_create` / `adm_reset` / `adm_destroy` **不**线程安全，须在渲染返回后才调用。
- **引擎行为**：`RenderOptions` / `RenderPlan` 各增 `std::stop_token cancel_token`（std 类型，未违反
  ADR 0003 的第三方边界）。`RenderService` 在导入后插入粗粒度检查点，各 renderer（EAR / VBAP / HOA /
  binaural）在帧循环边界检查 `stop_requested()`，命中即返回 `ErrorCode::cancelled` → `ADM_ERROR_CANCELLED`。
  取消（或任何中途失败）时，direct WAV/CAF 的部分写出文件会被删除（lossy 输出由既有 `TempFileGuard` 兜底），
  故取消不留截断残件。
- **未新增 render 入口**：token 经既有 `adm_render_file_ex`/options 透传，`adm_render_file` 与
  `adm_render_file_ex(opts==NULL)` 行为不变（不可取消）。

### v1.5.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

新增两个 options setter、一个 result accessor，**未触碰任何已有 signature、enum 值或 callback**，因此是
minor 升级。此前语义策略接口不对称——模板可经 `adm_policy_template_json` 取到内存字符串，但应用只能经
`adm_render_options_set_semantic_policy_path`（文件路径）、report 也只能经 `set_semantic_report_path`
写文件。本次把读写两端都补成内存接口，GUI 可全程不落临时文件。

- **内存策略输入**：`adm_render_options_set_semantic_policy_json` 接受 UTF-8 JSON 字符串
  （schema `mradm.semantic-policy.v1`），内部拷贝；`NULL`/`""` 清空。**优先级高于
  `semantic_policy_path`**（两者并存时记一条 warning 并采用内存策略）。畸形 JSON 不在 setter 处诊断，
  而是在渲染时作为错误浮现（与文件路径一致）。`opts==NULL` 返回 `ADM_ERROR_OK`，OOM 返回
  `ADM_ERROR_INTERNAL`。
- **内存报告捕获**：`adm_render_options_set_capture_semantic_report(opts, enabled)` 开启后，渲染会把
  生效语义报告（schema `mradm.semantic-report.v1`）一并捕获到内存，经
  `adm_render_result_semantic_report_json` 读取。与 `semantic_report_path`（写文件副本）相互独立，可同时使用。
- **报告 accessor 生命周期**：返回的字符串由 result 句柄持有，存活至 `adm_destroy_render_result`，
  **不可** 交给 `adm_free_string`（区别于 `adm_inspect_file_json` 等"调用方拥有"的接口）。未捕获或
  `result==NULL` 时返回 `NULL`。无论渲染错误码为何都可读取，便于在后段失败时仍展示报告。
- **引擎实现**：`semantic_policy.cpp` 把解析与报告生成各拆出纯内存函数
  （`parse_semantic_policy(string_view,label)` / `build_semantic_report(...)->string`），文件版
  （`load_semantic_policy_file` / `write_semantic_report_file`）转调之，文件输出字节不变；`RenderOptions`
  增 `semantic_policy_json` / `capture_semantic_report`，`RenderResult` 增 `semantic_report_json`。
  字段经既有 `adm_render_file_ex` 透传，**未新增 render 入口**。

### v1.6.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

新增一个只读 JSON 查询，**未触碰任何已有 signature、enum 值或 callback**，因此是 minor 升级。补齐
GUI 缺失的"输出容器可用性 / 约束"可编程查询——此前这套矩阵（FLAC ≤8ch、Opus 固定 48k、APAC macOS-only、
IAMF 需 `MR_ADM_ENABLE_IAMF=ON`、bitrate 区间）只在 README 文档里，GUI 只能硬编码、跨平台/不同构建易错。

- **输出格式查询**：`adm_output_formats_json(context, out_json)` 返回 JSON（root 带
  `"schema": "mradm.output-formats"` / `"schema_version": 1`），结构同 `adm_layouts_json`：`out_json`
  须非 NULL，结果为**调用方拥有**的堆字符串，经 `adm_free_string` 释放。不做项目文件 I/O；但在 IAMF 开启的
  构建中，计算 `iamf_mp4_packager` 标志会探测 PATH 并可能短暂 fork `mp4box`/`ffmpeg -version`（默认
  `MR_ADM_ENABLE_IAMF=OFF` 构建该标志为 false、不探测）。调用方若在热路径上使用应缓存结果。
- **内容**：`formats` 数组每项含 `format` / `extensions` / `available`（+ 不可用时的
  `available_reason`）/ `lossy` / `max_channels`（0=无限）/ `fixed_sample_rate`（0=任意）/
  `supports_height`，以及可选 `bit_depths`、`bitrate_kbps_per_ch`（Opus）或 `bitrate_kbps_total`
  （APAC）。root 另带 `features` 对象：`apac` / `iamf` / `iamf_mp4_packager` / `sofa` 构建/平台开关。
- **引擎实现**：新增静态格式表 `src/adm_engine/format_table.cpp`（与 `layout_table.cpp` 并列，nlohmann
  TU-local PRIVATE、对外只 `std::string`），可用性来自 `audio::apac_encoding_available()`（新增，
  `__APPLE__` 守卫）、`audio::iamf_encoding_available()` / `audio::iamf_mp4_packager_available()`（已有）、
  `binaural_sofa_supported()`（新增，`SAF_ENABLE_SOFA_READER_MODULE` 守卫）。经
  `RenderService::output_formats_json()` 暴露。**未新增 render 入口、未触碰渲染路径**。

### v1.7.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

新增一个 enum 与一个纯映射函数，**未触碰任何已有 signature、enum 值或 callback**——特别地，已冻结的
`adm_progress_cb`（`stage` 仍为 `const char*`）签名不变。补齐 GUI 做分阶段进度 / 本地化所需的阶段枚举。

- **进度阶段枚举**：`adm_render_stage_t`（`ADM_STAGE_UNKNOWN=0` 起，至 `ADM_STAGE_FINISHED`）镜像
  `mradm::RenderStage` 的语义集合；`ADM_STAGE_UNKNOWN` 用于 NULL/无法识别的字符串。
- **映射函数**：`adm_render_stage_from_string(const char* stage)` 把回调里的 `stage` 字符串映射为枚举，
  纯函数、线程安全、不分配，可直接在进度回调里调用。`stage` 字符串本就是回调契约的稳定部分；GUI 无需自行
  字符串匹配。
- **防漂移**：`stage_name()`（enum→string）与 `adm_render_stage_from_string()`（string→enum）共享同一组
  字符串；进度回调测试端到端渲染并断言引擎实际发出的每个 stage 都不映射为 `ADM_STAGE_UNKNOWN`，守住两者一致。
- **零回调改造**：未引入新回调类型、未新增 render 入口；既有调用方完全不受影响。

### v1.8.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

新增一个 opaque 类型与三个函数,**未触碰任何已有 signature、enum 值或 callback**。面向 GUI 时间线拖动
(scrubbing):此前每次渲染一个窗口都要重新 import ADM + 应用语义策略,反复预览同一文件代价高。

- **预览会话**:opaque `adm_preview_session_t` + `adm_create_preview_session(ctx, input, opts, out)` /
  `adm_preview_render_window(session, start_sec, end_sec, out_path, cb, ud, result)` /
  `adm_destroy_preview_session`(create/destroy 严格配对)。create 时 import + 应用策略**一次**,缓存
  处理后的 `AdmScene`;render_window 复用缓存 scene(跳过 import + 策略)+ Phase 1 的按需窗口渲染。
- **窗口参数**:`start_sec` 须有限且 `>= 0`;`end_sec <= 0` 表示"到结尾"(与 `set_render_end_sec` 一致),
  正值在渲染时校验 `> start`。`opts` 的 trim 字段被忽略——窗口由每次调用提供。`opts` 可为 NULL。
- **生命周期/线程**:session 非线程安全(同 context,单线程或外部串行化);`opts` 引用的 cancel token 在
  create 时捕获,作用于该 session 的每次窗口渲染。result 句柄语义同 `adm_render_file_ex`。
- **引擎实现**:`RenderService::render` 增可选 `const AdmScene* preimported_scene` 参数(非 NULL 时直接
  用、跳过 import / 策略 / 报告);新增 `RenderService::prepare_preview_scene` 与 `PreviewSession`
  (`include/adm/render.h`)。语义策略 json/path 解析抽成 `resolve_and_apply_policy` 单一来源。
  **未新增 render 文件入口、未触碰渲染算法**。Phase 2a:仅缓存 scene;后端 prepare/render 拆分(矩阵/HRTF
  缓存)留待 Phase 2b。

### v1.9.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为 APAC-in-CAF 输出追加容器选择，不改变既有 `.caf` 默认 PCM 行为。

- **新增 enum**：`adm_apac_container_t`，现有值为 `ADM_APAC_CONTAINER_MPEG4` 与 `ADM_APAC_CONTAINER_CAF`。
- **新增 setter**：`adm_render_options_set_apac_container(opts, container)`；`MPEG4` 为默认，`CAF` 要求最终输出路径为
  `.caf`，否则渲染时返回 invalid argument。
- **兼容性**：未修改 `adm_render_file_ex`、result、callback 或已有 option setter；旧调用方输出 `.caf` 仍得到
  float32 PCM CAF，只有显式设置 APAC CAF 容器时才写有损 APAC-in-CAF。

### v1.10.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为 GUI 细粒度进度条追加结构化 progress v2；旧 `adm_progress_cb` 与既有 render/preview 入口保持不变。

- **新增 enum**：`adm_progress_operation_t`，描述阶段内具体操作（validate/probe/import/policy/plan/prepare/render/trim/gain/bit-depth/FLAC/Opus/APAC/IAMF/package/metadata/finish）。
- **新增结构体 callback**：`adm_progress_event_v2_t` + `adm_progress_v2_cb`。事件携带 `struct_size`、`adm_render_stage_t`、operation、整体进度、阶段进度、帧级 `current_frame/total_frames` 和临时 message 指针。
- **新增入口**：`adm_render_file_ex2` 与 `adm_preview_render_window_v2`，参数与既有 `_ex` / preview render 对齐，只把 progress callback 换成 v2。旧入口继续从同一内部事件降级为 `fraction/stage/message`。
- **进度口径**：整体进度按稳定阶段范围映射（前处理约 0.00–0.30、渲染 0.30–0.90、后处理/编码 0.90–0.99、完成 1.00），不承诺等价于耗时占比；GUI 可用帧字段和时间戳自行估算 ETA。

### v1.11.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为 SAF HRTF 双耳后端补清晰 renderer 名称。

- **新增 enum 值**：`ADM_RENDERER_SAF_BINAURAL`，映射 C++ `RendererSelection::saf_binaural`。
- **Capabilities / layouts JSON**：追加 `"renderer": "saf-binaural"` 与 `supported_by: "saf-binaural"`。

### v1.12.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为 GUI 输出选项补“实际支持矩阵”，避免调用方手动拼接 backends/layouts/formats 三张表。

- **新增入口**：`adm_render_support_matrix_json(context, out_json)`，返回调用方拥有的 UTF-8 JSON 字符串，仍用 `adm_free_string` 释放。
- **内容**：root schema 为 `"mradm.render-support-matrix"` / `schema_version: 1`，包含 `features`、`backends`、`layouts`、`targets`、`entries`。每个 entry 给出 `renderer` / `layout` / `target` / `format` / `container` / `encoding` / `supported`，不支持时带 `reason`。
- **目标拆分**：区分 PCM `caf` 与 `apac_caf`、`apac_mpeg4` 与 APAC CAF、raw `iamf` 与 `iamf_mp4`；APAC CAF / IAMF MP4 在 target 中声明需要的 option。
- **口径**：这是基于当前构建、平台、renderer capability、layout 表、format/container 约束的静态支持矩阵，不读取具体 ADM 文件；输入采样率等项目级条件仍由 render/probe 流程最终校验。

### v1.13.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为 GUI/外部工具补通用 export 入口。

- **新增入口**：`adm_export_file`，复用现有导出路径，保持 render/preview ABI 不变。

### v1.14.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为 IAMF scalable channel layers 增加显式 opt-in 配置；默认仍保持单层 IAMF 输出。

- **新增 setter**：`adm_render_options_set_iamf_layers(opts, iamf_layers_csv)`，CSV 例：`5.1,5.1.2,5.1.4,7.1.4`；`NULL` 或空字符串清除配置。
- **约束**：仅对 IAMF raw / IAMF MP4 输出生效；最后一层必须匹配最终输出布局，层级需单调增加，当前开放 `5.1`、`5.1.2`、`5.1.4`、`7.1`、`7.1.4`。
- **兼容性**：旧调用方不设置该字段时行为不变；高度输出中包含平面层只记录 warning，不阻断用户显式请求。

### v1.29.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为语义编辑器的实时静音补齐明确状态，与离线 semantic policy 的 `gain.mute` 保持一致。

- **实时静音**：`adm_monitor_override_t` 尾部追加 `mute`；非 0 时将匹配的 AudioObject 或
  DirectSpeakers `speaker_label` 声道设为静音，`gain_db` 在静音状态下忽略。
- **旧结构保护**：64 位 ABI 中，v1.23–v1.28 结构在 `head_locked` 后带 4 字节尾部对齐填充。
  v1.29 先追加 `reserved_v1_29` 占据该填充，再把 `mute` 放到旧 `sizeof` 之后；旧调用方的
  `struct_size` 因而不会覆盖 `mute`。解析端对缺失字段使用 `mute=0`。
- **后端语义**：EAR、SAF VBAP、HOA 与 SAF binaural 使用零增益；Apple AUSpatialMixer 将零线性增益
  映射到其输入增益下限 −120 dB。字段仍按现有 worker block 边界生效，并跨后端热切换保留。

### v1.30.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为内置 22.2（`9+10+3`）输出增加显式双 LFE 路由策略。

- **新增 enum**：`adm_lfe_routing_mode_t`，冻结值为 `ADM_LFE_ROUTING_DIRECT=0`、
  `ADM_LFE_ROUTING_SPLIT_POWER=1`。
- **新增 setter**：`adm_render_options_set_lfe_routing_mode()`；未知枚举返回
  `ADM_ERROR_INVALID_ARGUMENT`，`opts=NULL` 保持安全 no-op 并返回 `ADM_ERROR_OK`。
- **兼容性**：默认 `direct`，旧调用方行为不变；只追加 symbol 与 enum，不改变 opaque struct、现有
  signature 或枚举值，动态库 `SOVERSION` 继续为 1。
- **行为边界**：`split-power` 只适用于单一语义 LFE 的 22.2 输出；原生 LFE1+LFE2 素材在后端
  prepare 阶段返回 `ADM_ERROR_INVALID_ARGUMENT`。其它布局保持既有路由并记录 warning。

### v1.31.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为 EAR / SAF 软件扬声器渲染增加可选择的 CoreAudio 固定输出几何；默认仍保持既有几何。

- **新增 enum**：`adm_speaker_geometry_t`，冻结值为
  `ADM_SPEAKER_GEOMETRY_STANDARD=0`、`ADM_SPEAKER_GEOMETRY_APPLE=1`。
- **新增 setter**：`adm_render_options_set_speaker_geometry()`；未知枚举返回
  `ADM_ERROR_INVALID_ARGUMENT`，`opts=NULL` 保持安全 no-op 并返回 `ADM_ERROR_OK`。
- **兼容性**：默认 `standard`，旧调用方输出不变；仅追加 symbol 与 enum，不改变 opaque struct、
  现有 signature 或枚举值，动态库 `SOVERSION` 继续为 1。
- **行为边界**：`apple` 只选择有效输出扬声器坐标，不修改输入 ADM 标称语义或声道顺序；
  Apple AUSpatialMixer 后端本身始终使用 CoreAudio 几何。LFE 不参与几何切换。

### v1.32.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为 SAF / Apple 的 DirectSpeakers 增加显式标签直达与位置空间化双路由。

- **新增 enum**：`adm_direct_speakers_routing_mode_t`，冻结值为
  `ADM_DIRECT_SPEAKERS_ROUTING_AUTOMATIC=0`、`ADM_DIRECT_SPEAKERS_ROUTING_LABEL=1`、
  `ADM_DIRECT_SPEAKERS_ROUTING_POSITION=2`。
- **新增 setter**：`adm_render_options_set_direct_speakers_routing_mode()`；未知枚举返回
  `ADM_ERROR_INVALID_ARGUMENT`，`opts=NULL` 保持安全 no-op 并返回 `ADM_ERROR_OK`。
- **自动策略**：SAF 扬声器与 Apple 扬声器解析为 `label`；Apple binaural 解析为 `position`；EAR、
  SAF binaural、HOA 等后端在 `automatic` 下保持原生行为，显式 `label` / `position` 返回
  `ADM_ERROR_UNSUPPORTED`。Apple binaural 显式 `label` 同样返回 unsupported。
- **语义**：`label` 先精确匹配输出标签，再使用共享别名；未命中时做零扩散、无插值空间化，
  已知 BS.2051 / 别名标签优先使用标签方向，否则使用 ADM 标称位置，并 warning。`position`
  忽略非 LFE 标签，以零扩散、无插值位置空间化。需要位置回退但缺少位置时统一使用
  `(0°, 0°)` 并 warning。LFE 识别和既有专用路由始终优先。
- **兼容性说明**：ABI 仍为纯追加，旧二进制无需重编译，`SOVERSION` 继续为 1；但这是一次已明确
  接受的默认渲染语义调整——Apple 扬声器 DirectSpeakers 从旧 AmbienceBed 位置空间化改为标签直达。
  Apple binaural 默认仍保持位置空间化，SAF 默认保持标签路由。

### v1.33.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为实时覆盖补齐 ADM `headLocked` 的“继承 / 显式 false / 显式 true”三态，避免仅调增益的覆盖条目
意外把源 ADM 状态改成 scene-relative。

- **结构尾部扩展**：`adm_monitor_override_t` 在 v1.29–v1.32 结构的 64 位尾部填充位置先追加
  `reserved_v1_33`（调用方必须置 0），再追加 `head_locked_valid`。字段顺序、类型与旧字段均未改变；
  完整 v1.33 结构的 `struct_size` 才覆盖 valid 字段。
- **新调用方语义**：`head_locked_valid == 0` 表示不提供实时 head-lock 值，继承当前活动 ADM block 的
  有效 `headLocked`；非 0 才采用既有 `head_locked`（0=scene/world-relative，非 0=head-relative）。
- **旧调用方兼容**：v1.23、v1.29、v1.32 等旧 `struct_size` 均不含 valid 字段，解析端继续把旧
  `head_locked` 当作始终显式的值，保持 v1.23–v1.32 行为。数组仍以首元素 `struct_size` 为 stride，
  不读取调用方结构范围之外的字节。
- **托管绑定**：C# `AdmMonitorOverride` 同步追加两个尾字段；GUI 只有在用户实际改动 head-lock 时才置
  `HeadLockedValid=1`，仅 gain/mute/topology 编辑保持 0。
- **既有 JSON 接口的加法字段**：scene inspect schema v1 在对象及 Objects/DirectSpeakers/HOA block
  中追加有效 `head_locked`；schema 版本不变，旧消费者可忽略未知字段。

### v1.34.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

为 DirectSpeakers 增加用户定义的稀疏标签路由矩阵；GUI 控件不属于本版本，调用方先通过 C++、C ABI
或 CLI 提供配置。

- **冻结 enum 追加值**：`ADM_DIRECT_SPEAKERS_ROUTING_MATRIX=3`。既有
  `AUTOMATIC=0`、`LABEL=1`、`POSITION=2` 的数值和语义不变。
- **新增 setter**：`adm_render_options_set_direct_speakers_matrix_path()` 与
  `adm_render_options_set_direct_speakers_matrix_json()`。字符串在调用期间复制进 options；`NULL` 或空串
  清除对应配置；`opts=NULL` 保持安全 no-op 并返回 `ADM_ERROR_OK`。
- **配置优先级**：path 与内存 JSON 同时存在时，内存 JSON 优先，并通过既有 render result 日志记录
  warning。`matrix` 模式缺配置、或非 `matrix` 模式携带任一配置，均返回
  `ADM_ERROR_INVALID_ARGUMENT`。
- **严格 schema 与数学**：仅接受 `mradm.direct-speakers-matrix.v1`。每条 source route 必须恰好选择
  非空 `targets` 或 `mute:true`；有限正权重按 `sqrt(weight / sum)` 固定换算成等功率 gain。输入/输出标签
  复用 BS.2051 与 DAW alias 规范化，规范化后的重复项、未知或歧义目标、输出布局不匹配、非 LFE block
  未完全覆盖以及任何 LFE route 都是错误；未被素材使用的 route 只记录 warning。
- **后端边界**：EAR、SAF 与 Apple 扬声器离线/实时路径接受矩阵；`automatic` 选择到 EAR 的扬声器路径
  同样接受。Apple/SAF binaural 与 HOA 返回 `ADM_ERROR_UNSUPPORTED`。LFE 不进入矩阵，继续优先使用
  既有 `direct` / `split-power` 专用路由。
- **兼容性**：只追加 enum 值和 symbol，不改变 opaque struct、既有函数 signature 或旧 enum 数值；
  动态库 `SOVERSION` 继续为 1。旧调用方不设置新模式与配置时行为不变。

### v1.35.0（additive，向后二进制兼容，`SOVERSION` 仍为 1）

新增 producer-neutral 实时 Scene 输入与播放器 pull 输出。接口只描述 Renderer-native canonical Scene，
不记录、识别或推断 producer、codec、语言或项目来源。

- **新增 opaque handle**：`adm_scene_stream_t`；由 `adm_create_scene_stream` /
  `adm_destroy_scene_stream` 严格配对。配置冻结输入/输出采样率、有界输入容量、输出 SPSC ring、水位线和
  SAF VBAP / SAF binaural 的原生布局、SOFA、spread、geometry、LFE routing 选项。
- **新增 canonical Scene POD**：element descriptor、完整 object state、mono planar PCM、初始状态、
  sample-accurate metadata update 与原子 `SceneFrame`。所有非 opaque POD 首字段均为 `struct_size`；
  POD 数组以首元素 `struct_size` 为 stride，同一数组每个元素尺寸必须相同。字符串与数组仅借用至
  当前调用返回；`submit` 返回前完成深拷贝。
- **时间线与 backpressure**：`begin_epoch` 是同步 reset barrier，epoch 严格递增；generation 在首帧前
  配置且拓扑不可变；frame 必须连续，gap/overlap 需新 epoch。`submit(timeout_ms)` 明确返回 accepted、
  would-block、timed-out 或 closed，不丢弃已接受 frame。输入队列同时受 sample 与 owned-byte budget 限制。
- **播放器 pull**：输出固定为 interleaved normalized `f32`。`pull` 始终填满缓冲并把短缺尾部补零；
  buffering/underrun 的零不推进媒体位置。pull 路径不加锁、不分配、不做 I/O，也不写日志或错误字符串。
- **渲染与采样率**：metadata 在输入 sample domain 生效，动态 SAF backend 完成空间渲染后再经 PRIVATE
  libsamplerate 0.2.2（`SRC_SINC_MEDIUM_QUALITY`）转换；同采样率旁路。持续有理数 accumulator 保证
  EOS 总长为 `ceil((end-target) * output_rate / input_rate)`，不按 SceneFrame 独立取整。
- **线程模型**：同一 handle 允许一个 producer/control 线程、一个 audio-pull 线程和一个 status 轮询
  线程；create/destroy 不得与其它调用并发。播放器仍独占设备、播放状态、A/V 同步与主时钟。
- **兼容性**：只新增 enum、POD、opaque handle 与 symbol；没有改动 v1.34 或更早的布局、signature、
  enum 数值和 callback，`SOVERSION` 继续为 1。

## opaque 指针与 callback 生命周期

`adm_context_t`、`adm_render_result_t` 是 opaque pointer，调用方不应直接 dereference 或假设大小。生命周期约定：

- **创建/销毁配对**：`adm_create_context` ↔ `adm_destroy_context`，`adm_render_*` 返回的 `adm_render_result_t**` ↔ `adm_destroy_render_result`。调用方必须严格配对。
- **线程亲缘性**：实验期暂不承诺多线程安全；稳定期至少承诺"不同 context 跨线程安全，单个 context 单线程使用"。如未来要支持单 context 跨线程，由独立 ADR 决定。
- **callback 生命周期**：`adm_progress_cb` / `adm_progress_v2_cb` 仅在对应函数调用期间被调用；调用方可保证 callback 和 `user_data` 在该期间有效，无需考虑函数返回后的异步回调（除非未来明确引入异步 API）。v2 事件里的 `message` 指针同样只在 callback 调用期间有效。
- **字符串所有权（句柄持有）**：返回 `const char*` 的函数（如 `adm_render_result_message`、`adm_render_result_log_entry` 写出的 module/message）保证字符串在对应 opaque 句柄被 destroy 前有效；调用方不得 free 该指针。
- **字符串所有权（调用方持有，v1.1 起）**：经 `char**` 出参返回**堆字符串**的函数（如 `adm_inspect_file_json`）把所有权转移给调用方；调用方必须、且只能用 `adm_free_string` 释放（不得用 `free()`/`delete`，因为分配器由 ABI 一侧决定）。`adm_free_string(NULL)` 是安全 no-op。未来其它返回堆字符串的函数复用同一释放函数。

这些约定写入 `include/adm/c_api.h` 的注释中。

## 实验期注意事项（M2/M3 中要做的事）

虽然实验期允许自由改动，但仍应：

- 每次破坏性变更在 commit message 注明，便于绑定方追踪。
- 不无缘无故改名；如有更好命名，借 M3 切到 v1 时一次性整理。
- 实验期内已有的 6 个函数应视为"基本形状已定"，新增函数倾向于追加而非替换。
- M3 切 v1 前做一次 ABI review，列出所有不满意的 signature 并一次性修正，避免 v1 出生就带历史包袱。

## 写法约束

- `include/adm/c_api.h` 不得引入额外 include（保持 `#include <stddef.h>` 等 C 标准头之内）。
- 公共 C ABI 函数实现统一在 `src/adm_c_api/adm_c_api.cpp`，全部标 `noexcept`。
- `static_assert(sizeof(adm_error_code_t) == sizeof(int), ...)` 类断言用于跨编译器 enum 大小一致性。
- SONAME 升级（破坏 ABI）必须配套 CHANGELOG（暂未引入，M3 时补）和 deprecation 周期说明。

## 影响

- `include/adm/c_api.h` 在本 ADR 接受后立即添加"experimental"状态注释；阶段 2 的版本宏在 M3 切 v1 时再添加。
- `CMakeLists.txt` 暂不设置 SOVERSION（实验期不发布稳定 SO）；M3 切 v1 时按本 ADR 模板补。
- 后续如有新增 ABI 函数，commit message 注明"experimental"标签，便于过滤。
- M3 切 v1 时需发起 "API v1 freeze" review，列出所有 deprecated/未删除项，并更新本 ADR 状态为 "M3 完成后切阶段 2"。

## 后果

优点：

- 实验期保留足够灵活性，避免过早承诺。
- 稳定期规则明确，绑定方可依靠 semver 做兼容矩阵。
- 与 ADR 0002（Rust-friendly）配合，未来 Rust 绑定可直接复用 v1 ABI。

代价：

- 实验期内绑定方需要紧跟 commit；早期 GUI/Swift 绑定属于"协作开发"而非"消费稳定 API"。
- 稳定期需要严格执行兼容规则，contributor 修改公共头时要 review。
- Deprecation 周期意味着即使决定删除的函数也要保留代码几个版本，增加少量维护负担。

## 风险

- **过早切 v1**：M3 跑通但还未充分使用就切 v1，可能很快发现 ABI 不合理。缓解：M3 切 v1 前做一次 ABI review；如发现重大缺陷，允许推迟到 M4。
- **实验期被外部依赖**：第三方在 0.x 时期就把 ABI 当稳定用。缓解：header 顶部明确写 experimental；初次外部接入前主动提醒。
- **deprecation 周期过长**：保留 deprecated 函数久了变成 dead code。缓解：major 版本升级时一次性清理。

## 参考资料

- 现行 C ABI：`include/adm/c_api.h`、`src/adm_c_api/adm_c_api.cpp`
- 错误码 enum 与 C++ 错误模型的映射：ADR 0005
- Rust-friendly 边界要求：ADR 0002
- 领域模型边界（C ABI 不暴露第三方类型）：ADR 0003
- semver 规范：https://semver.org/lang/zh-CN/
- Linux SONAME 实践：https://tldp.org/HOWTO/Program-Library-HOWTO/shared-libraries.html
