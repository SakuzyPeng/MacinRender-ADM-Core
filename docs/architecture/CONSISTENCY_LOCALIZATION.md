# 数值差异定位实验

目标是定位当前 Release 渲染的首个分歧阶段，为 Rust 数学切片划定边界。这里的控制项是实验工具，不构成新的生产确定性承诺。

## 复现

在 Consistency workflow 的手动运行中勾选 `localize`。原 A/B 基线首先渲染、记录，然后只有 B 组执行追加实验。三个平台各输出 `localization/native`、`localization/portable-rng`；macOS 另输出 `openblas`、`openblas-portable-rng`。每个变体都保存自己的构建记录，不能拿最终构建目录的状态解释前一个变体。

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

脚本要求重置 RNG 后的同进程重复与固定分组的 worker 对照逐位相同。连续重复、分组变化及跨平台差异记录为测量结果。完整定位结论将在三平台诊断运行后记录于此。
