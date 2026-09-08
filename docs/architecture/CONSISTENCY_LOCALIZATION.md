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

`std::hypot` 的具体算法没有被统一，上表那条已改用 `render_common::canonical_vector_length`。对有限 binary32 分量，以 binary64 计算平方，固定按 `(x² + y²) + z²` 求和，再做 binary64 `sqrt` 和 binary32 窄化。契约采用默认最近偶数舍入，不允许加法重排。平方至多有 48 位有效位，在 binary64 中精确且不溢出或下溢，因此保持求和顺序的 FP contraction 不会改变这条路径；这不代表整个默认渲染器免疫 FMA。

两次加法仍有舍入，不能承诺任意分量置换后的位模式相同。反例的输入位模式为 `(0x3f800000, 0x39b5016c, 0x368ef881)`：按 xyz 的规范顺序返回 `0x3f800001`，按 zyx 的规范顺序返回 `0x3f800000`。测试分别固定这两个预期值，约束运算顺序；不能用两个非零分量的用例证明三项求和对称。

三处 `normalize` 辅助函数（`render_common.cpp`、`hoa_renderer.cpp`、`binaural_renderer.cpp`）统一走这个入口，各自的零长度兜底不变。

本机（Linux、GCC 13.3、Release）改前 / 改后的完整矩阵对照：**12 个 case 里只有 `hoa-hoa3-point` 变化**，277,692 / 768,000 个采样不同——与基线里 macOS 对 Linux/Windows 的差异数完全相同；首个不同样本从 `0x3d8e14b1` 变成 `0x3d8e14b0`，正是基线中 macOS 那一侧的位模式。两组门禁清单里的 case 全部逐位未变。

提交 `9025d39e2b053e78d9976569db2944169a67a4cf` 的三平台运行 [34213036405](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/34213036405) 已完成：B 的 `hoa-hoa3-point` 三平台逐位相同，总数从 6/12 提升到 **7/12**。A 仍为 **5/12**；该测例在 macOS 对 Linux/Windows 仍有 129,018 / 768,000 个采样不同，最大绝对差为 `5.96046e-8`。因此只把该测例加入 `expected-identical-controlled.txt`，A 的清单保持不变。

`tests/unit/render_common_numeric_test.cpp` 在本地 CTest 和三平台 consistency 的 Release A/B job 中执行。输入和期望值均为字面位模式，输入经过 volatile 读取以防 Release 常量折叠替代实际计算。用例包括三个非零分量的精确长度、三项求和的舍入边界、极大和次正规分量；最终 PCM 的验收仍由对应配置的门禁负责。

### 其余三参数 `std::hypot`：已替换，但当前矩阵测不到

上面那处是定位报告钉住的站点。同一类问题还留在渲染路径的另外 8 处三参数 `std::hypot` 上，已一并换成 `canonical_vector_length`（`hoa_renderer.cpp:159,178`、`vbap_renderer.cpp:158,166`、`live_vbap_renderer.cpp:360`、`binaural_renderer.cpp:266,1084`、`render_common.cpp:599`）。

替换的依据是实测而非推断。glibc 2.39 上以 200 万组随机 f32 输入对照规范写法：

| 形式 | 与 `canonical_vector_length` 不一致 |
|---|---|
| 三参数 `std::hypot` | 648,112 / 2,000,000（**32.4%**） |
| 双参数 `std::hypot` | 0 / 2,000,000 |

所以只替换三参数形式。双参数站点（`render_common.cpp:60`、`binaural_renderer.cpp:274`、`vbap_renderer.cpp:232`、`head_rotation.h:35`、`live_binaural_renderer.cpp:206`、`scene.h:167`）保持不动：标准同样没有精度约束，但没有可测到的分歧，改动只会带来无收益的风险。这是「glibc 上未观察到」，不是「跨平台已证明一致」。

**这次替换当前矩阵验证不了。** Linux Release 上改前 / 改后 A、B 各 12 个 case 全部逐位不变（24/24）。用逐站点执行计数器（临时插桩，不入库）跑整轮矩阵，只有两处真正被执行：

| 站点 | 矩阵中执行 | 说明 |
|---|---|---|
| `hoa_renderer.cpp:178`、`binaural_renderer.cpp:1084` | 是 | 阈值比较；值远离 `1.0e-4`，分支不翻转，故 PCM 不变 |
| `hoa:159`、`vbap:158`、`binaural:266` | 否 | `distance_from_position` 只在 `pos.cartesian` 时进入，现有 fixture 全部使用极坐标 |
| `vbap_renderer.cpp:166` | 否 | `mdap_spread_degrees` 的返回值在 2D 布局被丢弃，而矩阵里的 VBAP 测例是 5.1 |
| `render_common.cpp:599` | 否 | `extent_disk_cloud` 未被现有 fixture 触及 |
| `live_vbap_renderer.cpp:360` | 否 | 实时监听路径，不在离线矩阵内 |

三平台运行 [34248046673](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/34248046673) 证实了这一点：B 仍为 7/12，7 个门禁测例全部 ok，无回归也无收敛。

### 关闭覆盖缺口：`objects-cartesian` 与 5.1.4 测例

上表 4 个死代码站点的成因是矩阵内容，不是代码：`distance_from_position` 只在 `pos.cartesian` 为真时求长度，而全部 fixture 用极坐标；`mdap_spread_degrees` 只在非 2D 布局被调用，而 VBAP 测例是 5.1。新增一个 fixture 与四个测例（12 → 16）：

- `objects-cartesian` —— 唯一让 `block.position.cartesian` 保持为真的 fixture。坐标取 `(0.25, 0.75, 0.375)`：`0.25² + 0.75² + 0.375² = 0.765625 = 0.875²`，**真值恰好可精确表示**，正确舍入必然返回 `0x3F600000`，而 glibc 的三参数 `std::hypot` 返回 `0x3F600001`。这让该测例成为可判定对错的探针，而不是实现之间的任意平局。extent 取归一化值（`Width 0.5 / Height 0.25 / Depth 0.125`），这既是 BS.2076 对笛卡尔对象的规定，也是 `mdap_spread_degrees` 把 width 乘 60 时所假设的量纲。
- `saf-5_1_4-extent`（极坐标 3D）、`saf-5_1_4-cartesian`、`hoa-hoa3-cartesian`、`binaural-cartesian-cloud`。

用逐站点执行计数器复核，4 个站点全部转为活跃。各测例的实测灵敏度：

| 测例 | 覆盖站点 | 1 ULP（`hypot` 回退） | 相对 1e-4 扰动 |
|---|---|---|---|
| `hoa-hoa3-cartesian` | `hoa:159` | 检出 | 检出 |
| `saf-5_1_4-cartesian` | `vbap:158`、`vbap:166` | 未检出 | 检出 |
| `binaural-cartesian-cloud` | `binaural:266` | 未检出 | 检出（1e-2） |
| `saf-5_1_4-extent` | `vbap:166` | 未检出 | 未检出（见下） |

即这些测例覆盖了代码路径，但除 HOA 外都不是 1 ULP 级的绊线。测灵敏度时必须隔离扰动：`spread_scale = 1/distance`，所以缩放 `canonical_vector_length` 会在链路两端自相抵消，得到「毫无反应」的假象；只扰动 `mdap_spread_degrees` 的返回值才测得出 VBAP 路径对 1e-4 敏感。

`saf-5_1_4-extent` 不敏感的原因值得单独记：极坐标 fixture 的 `Width` 是 30（BS.2076 对极坐标对象以度为单位），而 `mdap_spread_degrees` 计算 `width * 60`，得 1800，`std::min(180.0F, ...)` **恒定饱和**。也就是说对极坐标 extent 内容，该函数的数值是惰性的。这条只作记录：`* 60` 对笛卡尔的归一化 width 是对的，对极坐标的度数不对，但改动它会改变既有渲染行为，不属于本轮范围。

`render_common.cpp:599`（`extent_disk_cloud`）在本矩阵中**结构性无法覆盖**：其唯一调用者是 macOS-only 的 Apple 后端，而跨平台矩阵按设计排除该后端。

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
