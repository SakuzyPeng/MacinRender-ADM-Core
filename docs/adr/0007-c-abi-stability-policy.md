# ADR 0007：C ABI 稳定性承诺与版本策略

> 状态：已接受（已进入阶段 2，当前 ABI 为 stable v1.7）
> 日期：2026-05-17（v1.1 增量记录补充于 2026-05-30，v1.2 / v1.3 / v1.4 / v1.5 / v1.6 / v1.7 于 2026-06-01）
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

## opaque 指针与 callback 生命周期

`adm_context_t`、`adm_render_result_t` 是 opaque pointer，调用方不应直接 dereference 或假设大小。生命周期约定：

- **创建/销毁配对**：`adm_create_context` ↔ `adm_destroy_context`，`adm_render_*` 返回的 `adm_render_result_t**` ↔ `adm_destroy_render_result`。调用方必须严格配对。
- **线程亲缘性**：实验期暂不承诺多线程安全；稳定期至少承诺"不同 context 跨线程安全，单个 context 单线程使用"。如未来要支持单 context 跨线程，由独立 ADR 决定。
- **callback 生命周期**：`adm_progress_cb` 仅在对应函数调用期间被调用；调用方可保证 callback 和 `user_data` 在该期间有效，无需考虑函数返回后的异步回调（除非未来明确引入异步 API）。
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
