# ADR 0007：C ABI 稳定性承诺与版本策略

> 状态：已接受（已进入阶段 2，当前 ABI 为 stable v1.1）  
> 日期：2026-05-17（v1.1 增量记录补充于 2026-05-30）  
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

## opaque 指针与 callback 生命周期

`adm_context_t`、`adm_render_result_t` 是 opaque pointer，调用方不应直接 dereference 或假设大小。生命周期约定：

- **创建/销毁配对**：`adm_create_context` ↔ `adm_destroy_context`，`adm_render_*` 返回的 `adm_render_result_t**` ↔ `adm_destroy_render_result`。调用方必须严格配对。
- **线程亲缘性**：实验期暂不承诺多线程安全；稳定期至少承诺"不同 context 跨线程安全，单个 context 单线程使用"。如未来要支持单 context 跨线程，由独立 ADR 决定。
- **callback 生命周期**：`adm_progress_cb` 仅在对应函数调用期间被调用；调用方可保证 callback 和 `user_data` 在该期间有效，无需考虑函数返回后的异步回调（除非未来明确引入异步 API）。
- **字符串所有权**：返回 `const char*` 的函数（如 `adm_render_result_message`）保证字符串在对应 opaque 句柄被 destroy 前有效；调用方不得 free 该指针。

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
