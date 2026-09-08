# 数值差异定位实验

目标是定位当前 Release 渲染的首个分歧阶段，为 Rust 数学切片划定边界。这里的控制项是实验工具，不构成新的生产确定性承诺。

## 2026-09-08 定位结论

完整三平台诊断运行：[34199241959](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/34199241959)，测试提交 `dd47753`。六个 A/B job 及 Compare platforms 均成功；原基线仍为 A **5/12**、B **6/12** 三平台逐位一致，既有门禁全部通过。macOS 的四个诊断变体、Linux/Windows 的两个变体分别保留构建记录及实验结果。

可机读的输入 SHA-256、8 组实验结果、主要检查点及 PCM 比较摘要保存于 [consistency-localization-2026-09-08.json](consistency-localization-2026-09-08.json)。三种 fixture 在全部 8 个变体中均按文件字节验证一致。

前一次运行 [34197749008](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/34197749008) 在 macOS 的“统一 RNG 后重复应相同”假设处中断。后续完整运行保留这些不相等结果，并继续采集所有变体。成功表示测量完整有效，不表示所有假设成立或所有 PCM 相同。

| 原始分歧 | 定位到的阶段 | 控制实验与实际含义 |
|---|---|---|
| `ear-5_1-hoa-input`，仅 A 不一致 | f32 通道混加的融合乘加 | 四组合实验中，严格 FP 单独就能收敛，单独关闭 EAR SIMD 不能；实际参与混加的 24 个 f32 解码增益全相同。FMA 重放精确复现首个不同 PCM。 |
| `hoa-hoa3-point` | 三参数 `std::hypot` 归一化 | 极坐标转换和未归一化向量相同；macOS 返回长度 `0x3f800000`，Linux/Windows 返回 `0x3f7fffff`。更换 BLAS 不改变这个差异。 |
| `ear-5_1-extent` | 扩散 FIR 的 SAF FFT 路径 | B 的直接/扩散 double 增益一致；未插桩的 macOS OpenBLAS/KissFFT 对照与原 Linux/Windows PCM 收敛。 |
| `binaural-point` | HRIR → HRTF 的 FFT，另有 VBAP 数值边界敏感性 | 同一 HRIR 的 vDSP 和 KissFFT 频谱不同；原未插桩后端对照可收敛，但诊断构建还暴露了测量网格附近微小插值权重受三角化/求逆影响的情况。 |
| `binaural-extent-cloud` | RNG 三角化，其后为小矩阵求逆/插值权重 | 原始顶点一致，C RNG 不同导致凸包三角形不同。统一 RNG 后三角形一致；Linux/Windows 首次剩余分歧已经出现在 `invertLsMtx3D → utility_sinv → LAPACKE_sgetrf_work / LAPACKE_sgetri_work`。 |
| 两种 `binaural-extent-spreader` | 去相关延迟 RNG、几何 RNG；叠加 FFT/矩阵数学及分组拓扑 | 同平台连续重复即不同。统一 RNG 后延迟表收敛，仍不能推定全部 PCM 收敛；macOS 的剩余重复差异已在 HRIR → HRTF 阶段复现。 |

### 已落地的修复：向量归一化的长度计算

`std::hypot` 在标准里没有精度约束，上表那条已改用 `render_common::canonical_vector_length`：把三个分量转成 `double` 累加平方再取 `sqrt`。这条路径每一步都由 IEEE-754 精确规定——float 的平方在 double 中精确（至多 48 位有效位，double 有 53 位）、double 加法与 `sqrt` 都要求正确舍入、double→float 的窄化同样；`hypot` 用来防溢出的那套缩放也不再需要，因为 float 分量的平方在 double 里既不会溢出也不会下溢。用 double 累加还顺带免疫 FP contraction：平方既然精确，融合乘加与分开的乘、加结果相同，所以默认构建与受控构建一致。

三处 `normalize` 辅助函数（`render_common.cpp`、`hoa_renderer.cpp`、`binaural_renderer.cpp`）统一走这个入口，各自的零长度兜底不变。

本机（Linux、GCC 13.3、Release）改前 / 改后的完整矩阵对照：**12 个 case 里只有 `hoa-hoa3-point` 变化**，277,692 / 768,000 个采样不同——与基线里 macOS 对 Linux/Windows 的差异数完全相同；首个不同样本从 `0x3d8e14b1` 变成 `0x3d8e14b0`，正是基线中 macOS 那一侧的位模式。两组门禁清单里的 case 全部逐位未变。

据此**预期** `hoa-hoa3-point` 在下一次三平台运行中三平台收敛，但这仍是预期：macOS 与 Windows 的实测尚未跑。按门禁只填实测结果的约定，这次不动 `expected-identical*.txt`，等下一次 consistency 运行确认后再加。

`tests/unit/render_common_numeric_test.cpp` 把这条约束拉到本地 CTest：输入与期望值都是字面位模式，期望值按 IEEE-754 各步规则独立推出，不是抄某台机器的输出。改回 `std::hypot` 会让其中 4 条断言在 Linux 上立即失败（已实测）；macOS 上该测试对新旧实现都通过，因为 Darwin 的 `hypot` 恰好返回 1.0——所以它是本地的单向守卫，跨平台验收仍靠 consistency workflow。

这里给的是已验证测例的分歧边界。定位每一个后续残差、修复这些边界和验证任意 ADM 内容，是之后的实现与覆盖工作；不能把阶段 0 的有限测例当作全产品的确定性证明。

### EAR HOA 输入：排除错误的 SIMD 归因

在同一台 macOS、相同 Release 依赖与 Accelerate 后端上，分别改变 `MR_ADM_STRICT_FP` 和 `MR_ADM_EAR_SCALAR_REFERENCE`：

| Strict FP | EAR scalar | 相对 Linux/Windows 原 B 基线 |
|---|---|---|
| OFF | OFF | 92,188 / 288,000 样本不同 |
| OFF | ON | 同样的 92,188 个样本不同 |
| ON | OFF | 逐位相同 |
| ON | ON | 逐位相同 |

部分 libear double 解码矩阵末位会随 FP 模式改变，但转换成渲染实际使用的 f32 后，四组 24 个增益完全相同。第一次导致 PCM 分歧的混加在 `accumulate_channel_segment` 的 `col_d[f] += ch_in[f] * gd`：重放 fixture 的前四个输入采样和 f32 增益，逐步独立乘加得到 `0x3eb47e0a`，显式 `std::fma` 得到 `0x3eb47e0b`，分别匹配 B/A 的首个不同样本（输出 frame 255、channel 0；直接声补偿延迟为 255）。这不是 EAR extent SIMD 分派的因果证据。

### FFT 的 handle 生命周期也是确定性边界

不仅存在 vDSP/KissFFT 的跨实现差异：在本地 macOS Release 中，同一份 HRIR、同一 handle 的两次变换一致，但销毁并重新创建 handle 后会出现另一套位模式。独立于渲染引擎的 `mr_adm_fft_lifecycle_probe` 复现了这一点：第一次 DC 为 `0x3e73035c`，后续为 `0x3e73035b`，1025 个复数频点中有 996 个不同；KissFFT 的 32 次创建/执行结果一致。

在完整重复渲染的配对检查点中，原始 HRIR 相同，`HRIRs2HRTFs` 的输出已经不同。SAF 的这条路径仅做零填充、`saf_rfft_forward` 和布局复制；macOS 内部调用 `vDSP_DFT_zrop_CreateSetup`、`vDSP_DFT_Execute`。这些证据足够将它归到 FFT 计划/实现边界，尚未证明 Accelerate 内部究竟按地址、初始化历史还是其它条件选择了计算路径。

最小复现（Release，选择相应后端后构建）：

```sh
cmake --build build/consistency-b --target mr_adm_fft_lifecycle_probe
build/consistency-b/mr_adm_fft_lifecycle_probe /path/to/checkpoints/binaural-point/binaural.01-hrir.f32
```

这个独立 probe、四组合 EAR 实验和更细的 EAR HOA 检查点是完整 CI 之后补充的本地 Release 证据；不要误记为提交 `dd47753` 的 CI 已执行这些新增工具。

### Cloud 的剩余矩阵差异

在三平台统一 RNG 的变体中，Linux/Windows 的 2508 个顶点分量、5004 个三角形索引和 3,427,600 个 HRTF 浮点分量一致。15012 个逆矩阵元素里有 2230 个不同，第一个在下标 9：`0xc0b57b24`（`-5.671281814575195`）与 `0xc0b57b23`（`-5.671281337738037`）。输入三角形顶点索引是 `(0, 2, 1)`，对应列向量矩阵：

```text
[ 6.123234262925839e-17,  0.15038372576236725,  0.1736481785774231 ]
[ 0,                      0.08682408928871155,  0                  ]
[-1,                     -0.9848077297210693,  -0.9848077297210693 ]
```

此后压缩 VBAP 权重和部分索引不同，实际使用的 HRTF 插值结果也不同；例如查询网格 16455 的三个原始 HRTF 幅值相同，而加权复数和已不同。因此原有“都是 OpenBLAS，所以只剩 libm sin/cos”的推断不成立。预编译 LAPACK/BLAS 的版本、架构内核和内部运算顺序仍是独立控制边界。

### 对实现排期的影响

固定分组预算为 4 后，Linux/Windows 和 macOS OpenBLAS 两种 RNG 变体的 1/2/4 worker 对照均相同；macOS Accelerate 的统一 RNG 变体仍有重复和 worker 对照残差，因此不能宣称所有后端的线程数不变性已经成立。分组预算 4 → 1 则在共同后端下也会改变多轨结果，预算 4 → 2 对当前三轨 fixture 保持同样的三组拓扑。必须按实际组数解释结果。

插桩构建会改变二进制和分配历史；本次诊断构建的部分 point 结果与未插桩基线不同。原 A/B 门禁始终使用未插桩产物，独立后端、RNG、FP 与 FFT handle 实验负责验证因果边界。不能将单次查询落到一个测量点时的相等性推广到整个插值表。

第一批应同时覆盖稳定声源/实例身份对应的 RNG、确定的三角化与分组策略，以及规范运算顺序的向量归一化、FFT 和小矩阵求逆。仅换 Rust 或仅换 OpenBLAS 都不足够。后续还需逐项处理 spreader 的协方差/最优混合及其它尚未进入这批测例的路径，逐步扩大既有 bit-equality 门禁。

### 验证范围

- 三平台完整 workflow：六个 A/B job、Compare platforms、8 个诊断变体完成；原 A/B 门禁通过。
- Python 回归：工具 18 项、构建/记录 11 项、检查点比较器 4 项，共 33 项通过。
- Debug：EAR、HOA encode、双耳 fixture 和三组 consistency CTest 均通过；最后新增的 probe 在 Debug/Release 编译验证。
- `scripts/quality/check-changed.sh --base 306ec45 --build-dir build/debug` 通过；保留既有 EAR/HOA clang-tidy 告警。C 状态结构被脚本按 C++ 解析产生的构造函数误报，有局部说明和抑制。
- 独立 FFT 生命周期及四组合 FP/SIMD 实验使用 Release；诊断结束后，本地常用 Debug/B 构建已关闭实验 RNG 与插桩选项并恢复 Accelerate 配置。

## 复现

在 Consistency workflow 的手动运行中勾选 `localize`。原 A/B 基线首先渲染、记录，然后只有 B 组执行追加实验。三个平台各输出 `localization/native`、`localization/portable-rng`；macOS 另输出 `openblas`、`openblas-portable-rng`。每个变体都保存自己的构建记录，不能拿最终构建目录的状态解释前一个变体。

`run-localization.py` 会原位重配传入的测量构建目录，并在结束时保留最后一个变体。下一次基线测量应重新选择 preset，显式关闭两个诊断选项，并恢复目标 `SAF_PERFORMANCE_LIB`；普通 `build/release` 不应拿来运行这个变体脚本。

本地需 Release、测试工具和可用的依赖缓存：

```sh
cmake --preset consistency-b -DMR_ADM_CONSISTENCY_DIAGNOSTICS=ON
cmake --build build/consistency-b --target mradm_exe mr_adm_pcm_bits mr_adm_make_fixture mr_adm_repeat_render mr_adm_numeric_probe
python3 scripts/consistency/localize.py build/consistency-b /tmp/mradm-localization-new
python3 scripts/consistency/compare-checkpoints.py /path/to/first /path/to/second
```

`localize.py` 要求新的输出目录；输入仍由原确定性 fixture 生成器创建。跨平台分析前必须验证 fixture 的字节一致性。所有渲染输出均为 f32，只比较提取后的 PCM，不比较带时间戳的 WAV 容器。

## 检查点

- EAR：传入 libear 的对象参数、返回的 double 直接声与扩散声增益。
- HOA：极坐标转换的三角函数结果、未归一化方向、归一化方向、最终 SN3D 系数。
- 双耳：原始 HRIR、方向表、SAF 三角化顶点与三角形、FFT 后 HRTF、压缩 VBAP 索引和权重、实际查询网格的插值 HRTF。
- cloud：每个非零权重采样点的几何量、展开方向、量化网格索引。
- 内核：固定输入的 SAF 实 FFT 前后值、实际中心频率和 SAF `getDecorrelationDelays()` 输出。

`.f32` / `.f64` / `.i32` 以显式 little-endian 字保存，`.c32` 为交错的实部和虚部 binary32。比较器保留负零，不降精度。命名检查点仅记录每个进程的首次调用；HRTF 以查询网格索引命名，避免把 worker 调度顺序当成计算顺序。因此这些检查点定位的是当前静态测例，不代表对任意动态 ADM 时间线的完整跟踪。

## 独立控制量

1. **进程历史**：重复工具默认不重置 RNG；`--reset-rng` 在每次完整渲染前重置为 1。此实验只证明同一运行库内的状态依赖，不能统一 glibc / UCRT / Darwin 的算法。
2. **RNG 算法**：`MR_ADM_DIAGNOSTIC_PORTABLE_RNG=ON` 仅在诊断构建中给 SAF 注入统一的 32 位 LCG 和 `RAND_MAX`，仍保留进程全局状态。这样可以单独检验算法与取值范围的影响；生产实现仍需实例私有、稳定声源身份对应的随机流。
3. **worker 数量**：`MR_ADM_DIAGNOSTIC_WORKERS=1/2/4` 控制离线双耳渲染的两个真实 worker pool，日志记录实际数量。单任务优化可能直接在调用线程运行；多轨 fixture 提供 3 路 OLA 与多个 spreader 组，4 worker 时最多有 3 个独立任务。
4. **分组拓扑**：`MR_ADM_DIAGNOSTIC_GROUP_BUDGET` 单独控制 `build_spreader_groups` 的预算。worker 实验固定预算为 4；拓扑实验固定 worker 为 1。预算不是最终组数，以日志为准。
5. **数学后端**：macOS 的 OpenBLAS 变体同时改变 SAF BLAS 和 FFT（vDSP → KissFFT）。必须结合增益/FFT 检查点归因，不能把整个变体等同于只改 BLAS。

重置 RNG、固定分组 worker 对照的相等性也是待检验假设，记录在 `experiments.json` 的 `hypothesis_identical` 中；任一 PCM 差异都保留，不能因实验假设失败而停止采集后续变体。缺失检查点、错误输入、构建失败和比较器错误仍使任务失败；原 A/B 基线的一致性门禁保持有效。

首轮追加实验发现 macOS runner 在统一 RNG 后，spreader 重置重复仍有约 `3.87e-7` 的残差，因而推翻了“重置 RNG 足够覆盖全部内部状态”的假设。补充检查点追踪每个 spreader 实例的 filterbank HRTF、Voronoi 权重、外积、前 16 帧的 STFT、去相关、协方差及混合矩阵；只捕获实际活动通道，避免把未使用的 scratch 空间误当计算数据。
