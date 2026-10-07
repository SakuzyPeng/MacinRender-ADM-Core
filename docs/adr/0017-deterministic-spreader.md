# ADR 0017：OM spreader 固定分组预算与可移植数学函数

> 状态：已接受。日期：2026-10-07。
> 数值一致性二期的第四个切片，接续 [ADR 0016](0016-deterministic-resampling.md)。

## 问题与依据

重采样切片后三平台有 76/78 个 PCM 用例逐位相同，剩余两例 `binaural-extent-spreader` 与
`-multi` 在所有平台对上都最早于 `spreader/s0-g0.50-left.f32`（Rust spreader adapter 输出）分歧；
它之前的配置与输入检查点相同。FFT 已固定为标量路径，去相关器只用基本运算与 `sqrt`，组输出按组号
顺序归约。剩下的平台相关输入有三类：

1. **分组预算取自硬件线程数**。`build_spreader_groups` 以 `std::thread::hardware_concurrency()`
   为预算，决定是否在内存压力下把多条轨道打包进同一 adapter。打包改变了共享滤波器组与求和结构，
   因此核数不同的机器可能得到不同的位。
2. **spreader 几何与 HRTF 频带系数使用平台 libm**：测量方向的 `sin_cos`、球面 Voronoi 权重的
   `atan2`、扩散锥阈值的 `cos`，以及 `fir_coefficients` 中 f32 `from_polar(gain, arg())`。
3. **OM 协方差混合的 SVD 使用平台 libm**。nalgebra 的 2×2 SVD 经 simba 调用 `atan2`、`sin_cos`、
   `hypot`，默认落到 std，即 glibc、UCRT 或 Apple libm。

OM 求解对输入的最低位很敏感：FFT 切片时上游 1e-7 级的差异在 spreader 输出上放大到 0.013–0.033。
只要有一个 1 ULP 的 libm 差异，整个用例就不会逐位相同。

## 决策

- **分组预算固定为 8**（`k_spreader_group_budget`）。它与核数无关，worker 数仍按硬件决定；
  组输出按组号归约，worker 数不影响数值。诊断构建的 `MR_ADM_DIAGNOSTIC_GROUP_BUDGET` 覆盖保留。
  实时链路不使用 spreader 分组。
- **nalgebra 打开 `libm-force`**，simba 的全部标量函数改走纯 Rust `libm` crate（rust-lang/libm，
  musl 移植，0.2.16 精确锁定）。`libm` 默认的 `arch` 特性只把 `sqrt`、`fma`、`rint` 换成硬件指令，
  三者都由 IEEE 754 精确规定，不引入平台差异。影响范围是 workspace 内所有 nalgebra 标量函数：
  `mradm-dsp` 的 OM 混合，以及 `mradm-ear` 的几何、HOA 与 panner。
- **spreader 专用几何与系数改用 `libm`**：新增 `geometry::portable_direction(s)`；
  `voronoi_weights`（只被 spreader 使用）的 `atan2`、锥角阈值 `cos`、`fir_coefficients` 的
  `atan2f`/`cosf`/`sinf` 均改用 `libm`。VBAP、HRTF 等共用的 `geometry::direction` 不变，以免改动
  已收敛的用例。
- OM 混合中 `num_complex` 的 `norm`/`sqrt` 只作用在协方差对角元上。对角元是 h·conj(h) 的和，
  虚部恰为 0，`hypot(x, 0)` 与实数 `sqrt` 在任何符合 IEEE 的 libm 上都精确或正确舍入，因此不改，
  只在代码中注明。
- 二期内核探针新增 `spreader.*`（26 方向合成 HRIR 网格的 Voronoi 权重、频带系数，以及两个移动、
  扩张声源的多块处理），门禁加入 `spreader.*` 与此前只测量的 `om-*`。三平台验收后，按精确 id
  把两个 spreader 用例加入 PCM 门禁。

选择 `libm` 而不是扩展 `mradm-math`：SVD 在 simba 内部调用数学函数，只能经 simba 的 `libm_force`
开关整体替换；spreader 自身也需要 `atan2`。`libm` 是 Rust 官方维护的 musl 移植，已被 compiler-builtins
使用，并且 simba 已经提供了这条依赖路径。

## 代价

纯 Rust `libm` 的 `atan2`/`sin`/`cos` 比平台 libm 稍慢，但只出现在 2×2 SVD 和准备阶段。Linux x64
端到端测量在噪声内（双耳 OM spreader 0.99×），见[二期 OM spreader 收敛记录](../architecture/RUST_PHASE2_SPREADER.md)。

## 影响与边界

Linux x64 上只有两个 spreader 用例输出改变；EAR、HOA 与 OM 内核在本机逐位不变，说明本机 glibc
与 `libm` 在这些输入上结果相同。其他平台可能出现最低位变化，由现有门禁统一验证。
公开 C ABI、CLI、格式与分配约束不变。

退出条件：若 simba/nalgebra 改变 `libm_force` 的语义或移除该特性，需改为项目内实现并重新通过同一
门禁。改变 `k_spreader_group_budget` 是算术规则变更，需要同时更新门禁证据。
