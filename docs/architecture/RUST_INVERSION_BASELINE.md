# 方向与基线（四期 4.1）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)四期 4.1，
> 决策见 [ADR 0021](../adr/0021-rust-primary-project.md)，路线见 [Rust 主导逆转路线](RUST_INVERSION_ROADMAP.md)。
> 本切片不改变任何 PCM 或内核输出。

## 内容

| 项 | 实现 |
| --- | --- |
| 方向 | ADR 0021；ADR 0002 标为已取代，ADR 0008 记录决策三、四由 0021 取代，0003–0007、0009 各加修订指引；`CPP_ADM_PLATFORM_REWRITE.md` 标为历史规划 |
| 路线 | `RUST_INVERSION_ROADMAP.md`（终态、模块去向、期次、oracle 驱动面、冻结参考耦合）；审计文档新增四期一节，“C++ 剩余执行点”补四期归属 |
| C/C++ 规模棘轮 | `scripts/quality/language-surface.py` 与基线 `language-surface-baseline.json`；CI `version-metadata` job 执行 `--check`；测试在 `tests/unit/quality_scripts_test.py` |
| Rust 生产路径检查点 | `mradm-dsp` `diagnostics::process`：无需 scope，`MR_ADM_TRACE_DIR/<name>`，进程内首写有效，显式小端字，只在 `diagnostics` feature 下写出 |
| 场景检查点 | 离线渲染经 `RenderService` 时，在导入后与语义策略后写出 `render-scene/offline.05-import.i32`、`render-scene/offline.06-policy.i32`（只在诊断构建中编译） |
| 采集与比较 | `run-rust-phase2.py` 在诊断采集中要求每个离线用例都有两份场景检查点且编码版本为 1；`compare-rust-phase2.py` 按文件名把它们排在第 5、6 阶段，先于所有渲染阶段 |

### 检查点写出器为什么不合并

原计划让 C++ `consistency::dump` 经私有 FFI 转调 Rust，从而共用一个登记表。实施时发现有三个目标强制打开诊断、
但不链接 Rust，它们依赖 C++ 头自行写文件：`mr_adm_live_scene_trace_tests`、`mr_adm_numeric_probe`、`mr_adm_vbap_probe`
（后两个只在 SAF 参考开关下构建）。转调会让它们链接失败，或在非诊断 Rust 构建中静默不写。

因此两侧各保留一个登记表，约定一致：同一目录、首写有效、显式小端字，且**每个检查点名只归一方所有**。
四期迁移某段代码时，其检查点随代码一起转到 Rust 侧发出，C++ 侧同步删除，名称与内容不变。

## 场景检查点编码（版本 1）

两份文件内容都是 32 位小端字流（`.i32`）。各基本类型的写法：

| 类型 | 写法 |
| --- | --- |
| `u32` / `u16` / 枚举 | 1 个字 |
| `u64` | 低 32 位、高 32 位，共 2 个字 |
| `f32` | IEEE 位模式 1 个字 |
| `f64` | 位模式按 `u64` 写，共 2 个字 |
| `bool` | 0 / 1 |
| `optional<T>` | 存在标志一字，存在时紧跟 `T` |
| `string` | 字节长度一字，随后 UTF-8 字节每 4 个打包为一字（小端，末字补零） |
| `vector<T>` | 元素个数一字，随后依次写元素 |

顶层顺序：
1. 编码版本 `1`；
2. `info` 的 `sample_rate`、`num_channels`、`num_frames`、`source_kind`、`input_layout`、`input_channel_labels`；
3. `programmes`、`contents`、`objects`、`hoa_tracks`；
4. `import_warnings` 的条数。

各结构按 `include/adm/scene.h` 的字段声明顺序写出，生产实现见 `src/adm_engine/scene_checkpoint.h`。
四期 4.3 / 4.4 的 Rust 渲染场景必须按同一编码写出同名文件。

两类内容与运行环境有关，不写入编码：
- `info.file_path`：输入路径随平台和运行目录变化；
- 警告正文：可能含路径，只记条数。

预览会话复用已导入的场景，不发出这两份检查点；一致性采集只经 CLI 路径（每个离线用例的首个进程）取得它们。

## C/C++ 规模基线

4.1 结束时的测量见 `scripts/quality/language-surface-baseline.json`，三期结项时（`0a3b362`）的数字见
[逆转路线](RUST_INVERSION_ROADMAP.md#规模与棘轮)。4.1 只在诊断构建中新增临时的场景序列化器
（`scene_checkpoint.h`，四期 4.3 由 Rust 侧接管后删除），棘轮从 4.1 结束时的规模起算。

## 验证

**本机（Linux x64，GCC 13，Rust 1.98.0，config A）。** 基线为 `0a3b362`（独立 worktree），候选为本切片的工作树；
两边各做一次普通采集，再在原构建树打开 `MR_ADM_CONSISTENCY_DIAGNOSTICS` 做一次诊断采集：

- 普通采集：候选的 242 个 PCM id 与全部内核文件和基线逐字节相同，`run-rust-phase2.py` 的同进程 / 新进程重复性与分块检查通过。
- 诊断采集：两边的诊断无扰动检查都通过（诊断构建的 PCM 与普通构建相同）。
  - 基线已有的 26,358 个检查点文件和 201 个内核检查点在候选中全部存在且逐字节相同。
  - 候选另有 124 个新文件，即 62 个离线用例各两份场景检查点，编码版本均为 1。
  - 现有离线用例都不带语义策略，因此每个用例的 `06-policy` 都与 `05-import` 相同；带策略的覆盖属于 4.2。
- 诊断采集之后，`scene_checkpoint.h` 为消除 clang-tidy 告警把回调改为常量引用（`7950cfb`），编码不变。
- `ctest`（config A，Release）80 项全部通过，含 `phase2_tools_test`、`quality_scripts_test` 与 Rust 单元测试。
  `mr_adm_rust_quality`（rustfmt / clippy）、clang-format、新代码的 clang-tidy（打开诊断宏）、
  `check-reference-retention.py`、`version_metadata.py --check` 与 `language-surface.py --check` 通过。
  本机没有 cppcheck，未运行。
- 本切片没有改动私有 FFI，`mr_adm_ffi_header_check` 无需重跑。

**三平台。** [Consistency 38990075874](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/38990075874)
在本分支 `7950cfb` 上运行，macOS arm64 / Linux x64 / Windows x64 各采集 config A、B 的普通与诊断结果，全部成功。
`7950cfb` 之后的提交只改文档，结果适用于 4.1 的全部代码。

- 四份比较报告（A、A 诊断、B、B 诊断）都是 242 个 PCM id 三平台相同、0 个不同、平台集完整、门禁失败 0。
- 两种配置的诊断报告中，三个平台对各有 235 份内核、201 个内核检查点、26,800 条用例检查点全部相同。
- 26,800 条中包含新增的场景检查点：每个平台对 62 个离线用例 × 2 = 124 条，全部相同。场景检查点只写进诊断构建，
  普通构建的 PCM 不受影响，诊断报告的 242 个 PCM 同样三平台相同。
