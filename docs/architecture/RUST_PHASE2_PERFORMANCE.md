# Rust 二期：保持位模式的 FFT 性能回收

> 二期已按锁定矩阵结项，当前范围和维护规则见[结项记录](RUST_PHASE2_CLOSEOUT.md)。
> 本文保留本切片当时的测量、门禁状态和阶段边界。

2026-10-07，`666fe49` 在统一标量 FFT 的基础上回收了一部分性能：常用的 1024–4096 点
实数 FFT 在本机 M4 Pro 上耗时下降约 26%–31%，Linux CI 上约 29%–33%，Windows CI 上
约 22%–25%。双耳 cloud 的本机端到端耗时下降约 7%–8%。全部 78 个 PCM 用例继续保持
三平台逐位一致；本机优化前后的 78 份 PCM、67 份内核结果也逐位不变。

基准是本轮开始时的统一标量实现 `c182872`。这些数据表示本批的增益，不表示已经追回
相对早期自动 SIMD 后端的全部性能。前序数值决策继续按 [ADR 0015](../adr/0015-rust-fft-scalar-path.md)
执行，当前 RustFFT/RealFFT 的 SIMD features 仍为空。

## 实现与证据边界

Release 双耳 cloud 的采样剖析首先指向 RustFFT 的 radix-4 列计算，其次是小型基础蝶形和
OLA 卷积。此次通过本地 RustFFT 6.4.1 补丁，把四个独立列的实部和虚部分别放入定长数组，
并在准备期按相同布局存放各层系数，使编译器能够同时计算这些独立列。

- 每列的复数乘法、加减树、旋转符号、系数位模式和变换分解保持原样。基础蝶形继续使用
  上游实现，FMA 和运行时后端分派均未引入；不能重新打开 RustFFT 的 AVX/SSE/NEON features。
- 新模块禁止 unsafe，处理阶段不分配内存，非四倍数的自定义基础宽度由保留的标量表达式
  处理余列。公开 C ABI、产品 CLI、采样率及延迟契约不变。
- `fft_columns.rs` 直接编译生产补丁模块，以原有复数乘法和上游 `Butterfly4` 为对照，逐个
  比较 f32/f64 位模式。覆盖两个方向、1–8192 列、成批边界、符号零、抵消、下溢及不同量级。
- `array::from_fn` 的定长构造是性能相关的实现细节。本轮试验中改用数组迭代器后，位模式
  仍相同但生成代码明显变慢；后续此类重构必须同时通过位比较和基准。
- 重采样的独立点积试验表明，M4 Pro 上现有八路累加已被自动向量化，显式 NEON 没有
  明显收益。因此本批保留重采样实现，未据此推断 x64 重采样的优化上限。

补丁来源、79 个经行尾/尾随空白规范化后保持不变的上游文件及改动范围见
[`PATCHES.md`](../../rust/vendor/rustfft/PATCHES.md)与
[`PROVENANCE.json`](../../rust/vendor/rustfft/PROVENANCE.json)。新增模块使用 Rust 1.63 的
`array::from_fn`，项目工具链仍固定为 Rust 1.98.0 / LLVM 22.1.8。许可证原文同步保留。

## Release 测量

独立 FFT 基准调用生产私有 FFI，包含正常缓冲复制和逆变换归一化，准备时间不计入。
输入来自整数 LCG 与精确二进制缩放。预热一轮，然后新旧程序交替运行七轮，取中位数；
每次同时记录频谱和逆变换的位指纹并拒绝变化。表中为“补丁版耗时 / 原标量版耗时”，越低越快。

| 实数 FFT 长度 | M4 Pro 正/逆 | Linux x64 CI 正/逆 | Windows x64 CI 正/逆 |
|---:|---:|---:|---:|
| 512 | 0.794 / 0.797 | 0.757 / 0.745 | 0.811 / 0.800 |
| 1024 | 0.721 / 0.727 | 0.708 / 0.699 | 0.777 / 0.777 |
| 2048 | 0.736 / 0.741 | 0.708 / 0.701 | 0.772 / 0.773 |
| 4096 | 0.688 / 0.688 | 0.676 / 0.666 | 0.755 / 0.747 |
| 32768 | 0.697 / 0.700 | 0.684 / 0.676 | 0.745 / 0.724 |

macOS CI 同样记录了收益，但共享 runner 的波动更大；本机 M4 Pro 数据作为 macOS 性能描述的
主要依据。完整 128–32768 点记录及每轮样本均保留，性能比例不作为共享 CI 的细粒度硬门禁。

端到端使用 `mr_adm_make_fixture` 的固定整数信号，将静态元数据 fixture 的 PCM 重复到
60 秒和 300 秒。Release CLI 渲染为 float32 WAV，关闭峰值限制；采集 PCM 后删除输出 WAV。
基准 CLI 和动态库一起冻结，避免新旧程序误用同一套库。macOS 的运行库路径覆盖通过
`/usr/bin/time` 之后的 `env` 设置，避免被系统工具移除。计时包含渲染、计量和文件输出，
PCM 提取和哈希不计时。每轮、每版的 PCM SHA-256 都必须一致。

| 场景 | 60 秒输入耗时比（11 轮） | 300 秒原版 / 补丁版（7 轮中位数） | 300 秒耗时比 |
|---|---:|---:|---:|
| EAR extent | 0.961 | 1.912 s / 1.827 s | 0.956 |
| VBAP point（对照） | 1.031 | 2.175 s / 2.088 s | 0.960 |
| 双耳 point | 0.978 | 0.919 s / 0.876 s | 0.954 |
| 双耳 cloud | 0.916 | 2.550 s / 2.360 s | 0.926 |
| 双耳 OM spreader，3 轨 | 0.995 | 4.461 s / 4.378 s | 0.981 |

RSS 比值在 0.997–1.006 之间，未观察到明显增长。VBAP 对照也有约 ±4% 的变化，因此
约 2%–4% 的端到端变化暂不单独归因为这次优化。双耳 cloud 在两种时长下都减少约 7%–8%。
这些合成素材数据不代表所有节目、实际声卡延迟或听感；Windows/Linux 本批的性能证据限于 FFT。

## 验收与复现

[Consistency 37588314154](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37588314154)
在 `666fe49fd4da0a2b6f454e3c19220f4978adee62` 上通过。A、B 及其诊断版四份报告均为
78/78 PCM 三平台逐位相同，数值门禁零失败。B 仍仅是 C/C++ 严格浮点实验。源指纹：
`aef16671d84baf9a5a2561e9d6ec41cb13d5b9d04c26dbcb05db81210e111b31`。

同进程/新进程重复性、诊断无扰动、帧数、有效信号和无欠载门禁全部通过。平台 libm 的
f64 twiddle 观察列仍有原有差异，不在位相等门禁内。本机对优化前版本的 78 份 PCM 和
67 份内核结果均做了完整原始位比较。

- 本机 Debug CTest 71/71；新增列计算的 f32/f64 对照在 Debug、Release 都通过。
- [常规 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37588322470)和
  [Quality](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37588335394)通过；
  本地 Rust fmt/Clippy（含全部 features）、C++ 质量、许可证及冻结参考校验通过。
- 公开 C ABI 保持 139 个导出；私有 FFI 209 个函数 / 65 个结构体一致；16 个历史参考单元的
  45 个文件哈希保持不变。

CI 的 A 组先在同一构建树保存未打补丁的原标量 FFT 基准，再恢复 Cargo 文件、重建候选版，
交替计时后执行常规 PCM 采集。原版准备失败也必须恢复文件；依赖锁定检查拒绝变动其他包。
Windows 继续使用 canonical MSVC/Ninja `build/win-canon`。本机 Windows 检出有既存改动，
此次未同步；Windows/Linux 证据来自原生 CI runner。

在已配置为 Release、诊断 OFF 的构建树中复现 FFT 测量：

```sh
python3 scripts/consistency/benchmark-fft.py prepare-reference build/release local/fft-reference
cmake --build build/release --target mr_adm_fft_benchmark
python3 scripts/consistency/benchmark-fft.py compare local/fft-reference \
  build/release/mr_adm_fft_benchmark local/fft-performance.json
```

`prepare-reference` 暂时切换 Cargo 的 RustFFT 来源并恢复源文件，后续必须重建所需生产目标；
进行二期采集时重建 `mr_adm_phase2_tools` 以更新来源戳。端到端工具是
`benchmark-dsp.py --pcm-tool ... --require-identical-pcm`，共享库构建同时传入冻结的
`--legacy-runtime` 与候选的 `--candidate-runtime`。

机器可读[验收记录](evidence/rust-phase2/performance/validation.json)、各平台 FFT 样本、
60/300 秒端到端数据和逐用例原版对照均在同目录。完整 PCM/检查点保留于 CI artifacts。
本地复用了现有构建树和 Cargo 目录，结束后恢复 SOFA ON、installed deps ON、strict FP OFF、
diagnostics OFF，并重新构建 CLI/bundle、通过 `backends` smoke。
