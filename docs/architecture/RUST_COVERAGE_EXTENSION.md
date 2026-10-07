# Rust 后续覆盖：高采样率、HRTF 与退化 OM

2026-10-07，接续[二期结项](RUST_PHASE2_CLOSEOUT.md)。dr_flac 替换按用户决定暂缓，
现有[选型记录](RUST_FLAC_DECODER_PLAN.md)保留。本批扩展内部回放与内核测量；生产数值算法、
公开 C ABI、产品 CLI 及现有参考实现保持原样。

## 新增范围

- Scene 加入 96→96、48→96、96→48 kHz：立体声 VBAP、双耳 point/cloud 各用 fixed/fragmented
  两种分块；7.1.4 VBAP 加入 96→96 kHz。每个配置继续包括正常与 seek/preroll/短尾两个 epoch。
  新增 20 个配置、40 份 PCM，矩阵合计 34 个离线场景、42 个 Scene 配置、118 份 PCM。
  样本时钟、控制事件、输入信号和分块契约保持原来的 v1 描述。
- 重采样探针增加 96→96、48→96、96→48、48→192、192→48 kHz，记录实际生产 `Resampler`
  的输入和输出；96 kHz Scene 同时覆盖 KEMAR 准备与输出转换。192 kHz 的本批证据限于独立内核。
- OM 增加零矩阵、重复奇异值、秩一、反相、近秩一、微小能量、复数秩一及 floor 相邻值八组输入。
  实数和复数路径都直接调用 `real_mix` / `complex_mix`；采集前检查 Hermitian/半正定及退化标签，
  避免名为退化输入却实际满秩。实数路径使用这些矩阵的实部，`complex-rank-one` 的秩一声明
  针对复数矩阵，其实部为满秩。记录混合矩阵、残差及已有 SVD 诊断。
- HRTF 使用 26 方向、64 tap 的合成 HRIR，在 256/1024 点 FFT 下调用生产 `Grid` / `Filters`。
  方向、HRIR 和查询输入只用整数 PRNG、精确二进制缩放及固定浮点字面量。
  覆盖 ±180° 接缝、±90° 极点及邻域、分数角度、±540° 环绕；量化查询先单独检查 trace 开关，
  再检查缓存开关，连续查询也检查缓存开关，均要求位模式不变。记录完整网格权重/索引、频谱、
  查询结果及量化查询的幅度、复数和与归一化中间值。内核文件通过缓冲写出，结束前显式 flush；
  缓冲前后 133 个文件的字节保持相同，I/O 失败继续使采集失败。

共 133 个内核文件（原 67 个加 66 个），清单由 `phase2_common.kernel_outputs()` 严格核对。
新增 Scene id 纳入 PCM 门禁；新的 OM/重采样输出沿用现有内核模式。HRTF 新探针经首轮
三平台确认相同后，追加 `hrtf-[0-9]*` 门禁。当前为 118 个 PCM id 和 8 个内核模式（123 个文件），
其余 10 个旧观察项保留；当前仍只有 `fft-twiddles.10-libm.f64` 存在最多 1 ULP 差异。

## 验证方式

继续使用同一 Consistency workflow 的三平台 Release A/B 及诊断对照。先确认原 78 份 PCM 和
67 份内核结果未改变，再验证新增配置的重复性、有效信号、帧数、EOS、无欠载和诊断无扰动。
跨分块差异仍单独记录；新增输入上的首个观测分歧不自动归因为某个数学函数或 SVD 内核。

本机已通过 118 份 PCM 的回放契约与重复性、133 份内核的清单及重复性、诊断无扰动，且原 78 份
PCM / 67 份内核逐位不变。首轮 [Consistency 37599500795](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37599500795)
在 `36cb2b6` 上完成三平台 A/B 及诊断采集，四份报告均为 118/118 PCM 相同；新增 66 个内核文件
全部相同，据此加入 HRTF 门禁。

## 最终验收

最终实现 `77b796738f437110501bdbfe4cd20aac0b98f674` 的
[Consistency 37602151191](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37602151191)、
[CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37602159351)及
[Quality](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37602180003)全部通过。
源码/资源指纹为 `cee14021136a9d4e519b84de9d19adf752cc357b4c11286eaf32b0f129b3fa55`。

| 项目 | 原二期矩阵 | 本次扩展 |
|---|---:|---:|
| 离线场景 | 34 | 34 |
| Scene 配置（各两个 epoch） | 22 | 42 |
| 三平台逐位相同且设为门禁的 PCM | 78/78 | 118/118 |
| 内核测量文件 | 67 | 133 |
| 显式内核门禁匹配文件 | 57 | 123 |
| 观察项 | 10 | 10 |

四份报告（A、B 及各自诊断版）的三个平台对均为 118/118 PCM 相同，门禁零失败；同一 PCM
在平台、A/B 配置和诊断开关之间的 SHA-256 也全部相同。133 个内核文件中 132 个相同，唯一
不同项仍是未设门禁的平台 libm f64 twiddle 观察列，最大 1 ULP。本批未发现新增跨平台分歧，
无需修改生产算法；这些有限输入也不构成 HRTF/SVD 全输入域的确定性证明。

本地相关 Debug CTest 9/9、数值工具测试（含原 78 项参数指纹保护）、Rust fmt/Clippy
（含全部 features）、许可证和参考冻结校验通过。公开 C ABI 仍为 139 个导出。原有 78 份 PCM
及 67 份内核文件另与旧 Release 产物做了完整原始位比较，全部保持不变。

机器可读[验收记录](evidence/rust-coverage-extension/validation.json)、逐用例 PCM 哈希、内核摘要
及原矩阵对照保存在同目录；完整 PCM/检查点保留于 CI artifacts。历史二期结项 JSON 保持冻结，
新结果独立登记。仓库内文本证据按 UTF-8/LF 校验，原始 CI 报告哈希对应下载产物的原始字节。
本批不据采集时间推断生产 DSP 性能。

本机继续复用现有工作区、Release/Debug 构建树和共享 Cargo 目录。结束后恢复 SOFA ON、
installed deps ON、strict FP OFF、diagnostics OFF，重新构建 CLI/bundle，`backends` smoke 通过。
