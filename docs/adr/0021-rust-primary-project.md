# ADR 0021：项目逆转为 Rust 主导

> 状态：方向已接受（2026-10-16，维护者确定最终目标）；五期起的切分为初拟，在各期准备时确定。
> 日期：2026-10-16
> 适用范围：全部生产代码的语言归属与调用方向、`include/adm/*` 的对外承诺、C ABI 的实现位置、构建与依赖组织。
> 取代：[ADR 0002](0002-cpp-first-rust-later.md)；[ADR 0008](0008-rust-entry-and-saf-replacement.md) 决策三、决策四。
> 修订：ADR 0003、0004、0005、0006、0007、0009 中写明由 C++ 持有的条款（见下文“对既有决策的影响”）；
> [`CPP_ADM_PLATFORM_REWRITE.md`](../architecture/CPP_ADM_PLATFORM_REWRITE.md) 改为历史规划。

## 背景

ADR 0002 选择 C++ 地基的理由是 ADM / BW64 / BS.2127 / SAF 的生产生态集中在 C++。这个前提已经不成立：

| 原 C++ 依赖 | 现状 |
| --- | --- |
| libadm | `mradm-adm`（ADR 0012） |
| libbw64、dr_wav、C++ chunk 改写 | `mradm-wav` |
| libear | `mradm-ear`（ADR 0013） |
| SAF 子集、libebur128、libsamplerate | `mradm-dsp`（ADR 0008、0010、0011） |

三期结项（`0a3b362`）时，PCM 写出为止的数值运算都已在 Rust，C++ 生产代码只剩正确舍入的 `sqrt` 和单独的加乘。
C++ 现在主要承担这些工作：
- 领域模型与语义策略；
- 后端计划构建、块时间线与渲染循环；
- 渲染编排、实时调度与设备；
- PCM 写出编排、公开 C ABI 与 CLI；
- 平台框架接入。

规模（`wc -l`，`0a3b362`）：

| 部分 | 行数 |
| --- | ---: |
| `src/` C/C++/ObjC++ | 约 43.4k |
| `include/` | 约 4.6k |
| C++ 测试（不含冻结参考） | 约 47.6k |
| `rust/crates` | 约 57.3k |

两种语言并存的成本持续存在：
- 手写私有 FFI 头需要 cbindgen 校验；
- 两套错误模型、两套构建（CMake + Corrosion + Cargo）；
- 数值规则要在两种语言里分别约束（例如 S07 的乘加融合）；
- C++ 测试与 C++ 实现成对维护。

## 决策

### 1. 终态

仓库成为 Rust 项目。Cargo workspace 持有：
- 领域模型、语义策略、导入与导出编排；
- 全部项目自有渲染后端（离线渲染、文件流式渲染、Scene 流式渲染）；
- 计量、后处理与 PCM 写出；
- 实时引擎与设备调度；
- 公开 C ABI 的实现，以及 `mradm` CLI。

C/C++/ObjC++ 只保留在“岛”中，经窄 C 接口由 Rust 调用：
- Apple 框架：AUSpatialMixer 后端、系统空间监听 sink、APAC 与 CAF 元数据；
- Windows `ISpatialAudioClient` sink；
- 第三方 C 库：libFLAC、libopus、miniaudio、dr_flac、IAMF bridge。

是否进一步以 Rust 绑定（如 objc2、windows crate）取代这些岛，另行评估。

### 2. 调用方向分期反转

ADR 0008 决策三“当前集成方向为 C++ 调 Rust”和决策四“`AdmScene` 和渲染编排继续由 C++ 持有”由本节取代。

| 期 | 内容 | 状态 |
| --- | --- | --- |
| 四期 | PCM 写出为止的数据链路迁入 Rust：导入投影、语义策略、后端准备与渲染循环、公共渲染层、PCM 写出与后处理 IO。C++ 只保留适配壳（实现现有 C++ 接口并转调 Rust 句柄）与调度 | 已定，切片见 [PCM 一致性审计](../architecture/PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust) |
| 五期 | 编排与实时调度迁入 Rust | 初拟 |
| 六期 | C ABI 实现与 CLI 迁入 Rust；删除 C++ SDK；构建以 Cargo 为主 | 初拟 |
| 七期 | 平台岛收缩、冻结参考退役 | 初拟 |

详细路线与退出条件见 [Rust 主导逆转路线](../architecture/RUST_INVERSION_ROADMAP.md)。

### 3. 不变的契约

- `include/adm/c_api.h`（ADR 0007）：
  - 符号、签名、枚举数值、`struct_size` 规则、对象生命周期、错误码数值与 `SOVERSION 1` 不变；
  - 库名仍为 `mradm_capi`，导出仍只限 `c_api.h` 声明的 `adm_*`；
  - 实现语言不属于承诺。
- `mradm` CLI 的子命令、选项、输出与退出码；GUI 的 `[LibraryImport("mradm_capi")]` 边界。
- 渲染结果：锁定矩阵的 PCM、内核与检查点逐位不变，规则见下文第 5 条。
- 错误语义：可恢复错误不跨边界展开，中文消息与上下文字段保持；Rust panic 不穿过任何 C 边界。

### 4. 不再承诺的接口

`include/adm/*.h`（`c_api.h` 除外）与安装的 C++ 静态库 SDK 不再作为对外接口：
- 这些头改为内部头，随代码迁出而收缩、删除；
- 对应的 `install(...)` 规则同步删除，并在发布说明中写明；
- 不提供建在 C ABI 之上的 C++ 封装层。

### 5. 迁移纪律

1. **结构迁移逐位不变。** 迁移前后，锁定矩阵的全部 PCM id、内核文件与既有诊断检查点逐位相同，迁移后的代码继续发出同名检查点。
2. **覆盖先行。** 迁移某条路径之前，该路径须先有门禁覆盖；没有覆盖的路径先补用例，三平台相同后再迁。
3. **数值变化单独成片。** 有意改变输出的规则（例如 S03 的文件流式对象平滑）单独成片，并单独说明依据。
4. **迁出即删除。** C++ 实现迁出后删除，不保留运行时回退。需要行为对照时，按
   [参考实现保留与退役](../architecture/RUST_REFERENCE_RETENTION.md) 冻结到 `tests/reference/` 并登记。
5. **不新增 C++。** 新功能、新工具与新测试用 Rust（工具脚本可用 Python）。平台岛内的例外须在提交中说明。
6. **C++ 规模只减不增。** `scripts/quality/language-surface.py --check` 对生产 C++（`src/` + `include/`，不含岛）
   与岛分别设棘轮，每个切片报告变化。
7. **unsafe 边界。** 算法 crate 保持 `#![forbid(unsafe_code)]`。unsafe 只写在 FFI crate 中，C ABI crate 出现后也计入此列。
8. **依赖。** 新 Rust 依赖按 ADR 0004 锁定精确版本、关闭不需要的 features、登记许可证。
   迁出 C++ 后不再使用的 C++ 依赖（CLI11、spdlog/fmt、nlohmann_json、tl-expected）随之移除。

### 6. 过渡期规则（四期）

- 一致性工具（`mr_adm_phase2_render`、`mr_adm_phase2_stream`、`mr_adm_phase2_scene`、`mradm` CLI）驱动内部 C++ 接口。
  四期保留 `IRenderer` / `IRenderStream` / `ILiveSceneRenderer`、`RenderService`、`SceneStreamEngine` 作为适配壳，
  工具不改，结构迁移因此能用同一套 oracle 对照。
- 工具改写为 Rust 的工作放在迁移这些接口的那一期。切换前先在同一构建上证明新旧工具输出逐位相同。
- 由 Rust 驱动的渲染循环经 C 回调宿主结构接收进度、日志、取消和 PCM sink，编码器沿用同一 sink 接口，留在 C 岛中。

## 对既有决策的影响

| 决策 | 处理 |
| --- | --- |
| ADR 0002 | 整体取代。它提出的 Rust-friendly 约束（C ABI、独立领域模型、golden fixture）继续作为迁移手段 |
| ADR 0003 | 自有领域模型、不泄漏第三方类型这两条继续有效。输入边界改为 `mradm-wav` / `mradm-adm` → Rust 渲染场景 → 后端；`include/adm/*` 的规则只对 `c_api.h` 保留对外含义 |
| ADR 0004 | `mr_adm_core_find_or_fetch()` 只用于保留的 C 库；Rust 依赖走 Cargo。FLAC / Opus provider 策略不变 |
| ADR 0005 | 错误码数值一一对应继续有效。`tl::expected` 与 `noexcept` 模板随 C++ 代码退出，改为“不跨 C 展开”。内存不足时的行为在 C ABI 迁移期另定 |
| ADR 0006 | CLI 改为 Rust 时由新 ADR 取代；解析规则保持三期 3.5 的十进制规则 |
| ADR 0007 | 兼容规则不变。“实现统一在 `adm_c_api.cpp`、全部 `noexcept`”和 CMake `SOVERSION` 等实现条款在 C ABI 迁移期改写 |
| ADR 0008 | 决策三、决策四由本 ADR 取代；确定性契约（决策六）、FFI 约定与参考保留规则继续有效 |
| ADR 0009 | 设备侧依赖边界不变；“socket 收发、快照和 C ABI 仍在 C++”在实时调度迁移期改写 |

## 风险

- **性能。** 渲染循环改由 Rust 驱动后，FFI 粒度、计量线程与工作池调度都可能变化。每个切片按审计“每个切片的性能回归”测量，
  中位数比超过 1.05 时调查。
- **冻结参考与 C++ 删除耦合。** `tests/reference/` 引用若干生产头。删除这些 C++ 之前，要么把被比较的旧实现连同参考一起冻结，
  要么按退役条件退役对应单元。退役条件包含正式版本发布，需要维护者安排。
- **文本输出格式。** 语义报告、策略模板等 JSON 文本改由 Rust 生成时，数字与缩进格式需保持逐字节相同，否则单独说明并更新测试。
- **构建。** Corrosion、Cargo 与 MSVC CRT、PIC、导出表的组合在每一期都要三平台验证。
