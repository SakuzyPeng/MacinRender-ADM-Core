# Rust 二期：OM spreader 收敛

> 决策见 [ADR 0017](../adr/0017-deterministic-spreader.md)。前序：[Rust 二期重采样收敛](RUST_PHASE2_RESAMPLER.md)、
> [Rust 二期 FFT 收敛](RUST_PHASE2_FFT.md)、[Rust 二期基线](RUST_PHASE2_BASELINE.md)。

重采样切片后，三平台 76/78 个 PCM 用例逐位相同。剩余两例 `binaural-extent-spreader`、`-multi`
在三个平台对上都最早于 `spreader/s0-g0.50-left.f32` 分歧。本切片让分组与硬件无关，并把 spreader
与 nalgebra SVD 用到的数学函数全部换成纯 Rust 实现。

## 改动

- `src/adm_render_binaural/binaural_renderer.cpp`：spreader 分组预算由 `hardware_concurrency()`
  改为固定的 `k_spreader_group_budget = 8`；worker 数仍按硬件，诊断覆盖
  `MR_ADM_DIAGNOSTIC_GROUP_BUDGET` / `MR_ADM_DIAGNOSTIC_WORKERS` 保留。
- `rust/Cargo.toml`：nalgebra 启用 `libm-force`（simba `libm_force`），新增 workspace 依赖
  `libm = "=0.2.16"`，供 `mradm-dsp` 直接使用。
- `mradm-dsp`：
  - `geometry::portable_direction(s)`（`libm::sincos`）供 spreader 使用，原 `direction(s)` 不变；
  - `voronoi_weights` 的两处 `atan2` 改为 `libm::atan2`（只有 spreader 调用）；
  - `Spreader::set_source` 的锥角阈值改为 `libm::cos`；
  - `filterbank::fir_coefficients` 的 `from_polar(gain, arg())` 改为 `libm::atan2f` / `cosf` / `sinf`，
    公式与运算顺序不变；
  - `mixing.rs` 注明 `num_complex` 的 `norm`/`sqrt` 只作用于虚部恰为 0 的协方差对角元，结果与 libm 无关。
- 依赖登记：许可证清单新增 `rust-libm-0.2.16`（MIT），SBOM、依赖表与许可证原文 bundle 已重新生成。
- 二期探针新增 4 个内核，清单为 67 项：
  - `spreader.10-input.f32`：26 方向网格、64 tap 合成 HRIR 与两路输入 PCM；
  - `spreader.20-voronoi.f32`：球面 Voronoi 权重；
  - `spreader.30-fir.c32`：133 频带 × 2 耳 × 26 方向的频带系数；
  - `spreader.40-output.f32`：两个声源在 6 块内移动并把扩散从 15°/120° 扩到 165°/345°。
  门禁新增 `om-*` 与 `spreader.*` 内核。

## Linux x64 本地验收（2026-10-07）

- 默认 Debug CTest 69/69；`mr_adm_rust_quality`、FFI 头校验（209 函数 / 65 结构体）、许可证与冻结参考校验通过。
- Release `MR_ADM_BUILD_LIBEAR_REFERENCE_TESTS=ON`：`mr_adm_libear_reference_tests`、EAR fixture/post 测试，
  以及 SAF、重采样、ebur128、libbw64 参考对照仍在原阈值内。
- 二期 A 组采集（GCC 13 Release，SOFA/IAMF OFF，vendored FLAC/Opus），对比 `a01b285`：
  - 内核：原有 63 项逐位不变（包括 `om-*` 与全部 EAR FIR），新增 4 个 `spreader.*`；
  - PCM：只有两个 spreader 用例改变（最大绝对差 0.0068 和 0.0147），其余 76 个逐位不变，EAR/HOA
    用例不受 `libm-force` 影响；
  - 同进程/新进程重复性、Scene 健康与帧数门禁通过。
- 端到端（60 秒 fixture，Release `mradm render`，5 次中位数，`a01b285` 对本改动）：VBAP 0.97×、EAR extent 1.03×、
  双耳 point 1.06×、双耳 cloud 1.02×、双耳 OM spreader（3 轨）0.99×，均在噪声范围内。

## 三平台验收

待 Consistency CI 结果补充。
