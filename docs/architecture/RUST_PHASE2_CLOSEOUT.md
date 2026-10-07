# Rust 二期结项

> 状态：已完成（2026-10-07）。结项范围是已锁定、已验收的三平台 Release 矩阵。
> 机器可读记录：[closeout.json](evidence/rust-phase2/closeout.json)。

> 后续覆盖已扩展到 118 份 PCM / 133 份内核测量，见[扩展记录](RUST_COVERAGE_EXTENSION.md)。
> 本文及结项 JSON 保留原 78 项矩阵的验收范围与证据。dr_flac 已按用户后续决定暂缓。

## 验收结论

二期完成了基线与分歧定位、Scene 算术统一、FFT/重采样/OM spreader 收敛，以及保持位模式的
首批性能回收。本范围内没有未解决的 PCM 分歧或失败门禁。公开 C ABI、产品 CLI 与格式保持兼容。

最终实现提交为 `666fe49fd4da0a2b6f454e3c19220f4978adee62`，源码/资源指纹为
`aef16671d84baf9a5a2561e9d6ec41cb13d5b9d04c26dbcb05db81210e111b31`。
[Consistency](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37588314154)、
[CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37588322470)及
[Quality](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37588335394)均通过。
本次收尾只整理状态、范围和证据，保持该实现指纹与现有门禁不变。

| 切片 | 三平台共同逐位相同的 PCM | 记录 |
|---|---:|---|
| 初始基线 | 29/78 | [基线与定位](RUST_PHASE2_BASELINE.md) |
| FFT 收敛（同时解决所测 EAR 后处理分歧） | 52/78 | [FFT](RUST_PHASE2_FFT.md) |
| 重采样收敛 | 76/78 | [重采样](RUST_PHASE2_RESAMPLER.md) |
| OM spreader 收敛 | 78/78 | [spreader](RUST_PHASE2_SPREADER.md) |
| 保持位模式的 FFT 性能回收 | 78/78 | [性能与原版对照](RUST_PHASE2_PERFORMANCE.md) |

最终 A、B、A 诊断版、B 诊断版四份报告全部为 78/78，数值门禁零失败。同进程/新进程重复性、
诊断无扰动、输入/事件完整性、有效信号、帧数、EOS 和无欠载契约全部通过。
本机 Debug 71/71；公开 C ABI 保持 139 个导出，私有 FFI 保持 209 个函数 / 65 个结构体。

常用 1024–4096 点 FFT 耗时相对二期统一标量版本减少约 22%–33%（平台不同），本机双耳
cloud 端到端减少约 7%–8%。这些是限定用例的 Release 证据，不表示已经追回早期 SIMD 后端的
全部性能；完整样本、RSS 和测量限制保留在性能报告中。

## 冻结的验收范围

- 平台为 macOS arm64、Windows x64、Linux x64；Rust 1.98.0 / LLVM 22.1.8，锁定 Cargo.lock、
  源码、资源与有效参数。测量配置使用 vendored FLAC/Opus、IAMF/SOFA OFF 和内置 KEMAR。
- 34 个离线场景，以及 22 个 Scene 配置各两个 epoch，共 78 份 PCM。覆盖 EAR、VBAP、
  Triple Balance、HOA、双耳 point/cloud/spreader、窗口/旋转，以及 −23 LUFS 与 −6 dBTP 反馈。
- Scene 配置包含 512 帧和循环 `[1,7,127,511,1024]` 分块，48→48、48→44.1、44.1→48 kHz，
  控制渐变、头追、generation/policy/backend 切换、seek/preroll、短尾/EOS 和虚拟设备 DSP。
  各分块配置分别比较；跨分块位相等未被新增为契约。
- A 是默认数值配置，B 是 **C/C++ 严格浮点实验**。两者共用 Rust 的数值实现；B 不充当另一套
  确定性参考。RustFFT 继续使用标量规划器，独立列允许编译器按原有运算顺序自动向量化。

门禁以 [`phase2-gates.json`](../../scripts/consistency/phase2-gates.json) 为唯一清单：
**78 个精确 PCM id、7 个内核模式（展开为 57 个内核文件）**。共采集 67 个内核文件，另外
10 个保留为观察项：EAR 布局输入 3 个、Scene 输入/输出 2 个、HpTF 输入/系数 4 个，以及
平台 libm 的 f64 twiddle 列。前 9 个本轮结果相同；f64 twiddle 列仍有最多 1 ULP 的平台差异，
当前 f32 表和受门禁保护的最终 PCM 均相同。本次收尾不扩充内核承诺清单。

这些结果不扩展为所有输入、采样率、布局或编译器版本的证明。Apple 系统渲染器、真实设备/
系统混音、外部 SOFA 数据集、独立 Monitor API 和编码文件字节一致性继续在本范围之外。

## 维护与参考保留

`Consistency` 继续在 push main 和手动触发时运行。后续数值、资源、工具链或相关依赖修改，
合入前应在分支手动运行同一三平台矩阵；失败时定位实现或无效采集，不能放宽门禁或删除失败用例。
新增 PCM 用例须同时登记精确 id，改变数值规则须另行记录决策和原版对照。
首次观测分歧仍须与已证实根因区分；不同分块、分组预算或性能实验不改变原有契约。

历史基线、各切片的原始验收 JSON、SAF 定位记录和冻结参考保持原样，切片文档中的旧计数表示
当时结果。完整 PCM/检查点保留在 CI artifacts，仓库保留摘要、指纹及必要复现数据。

16 个参考单元、45 个文件继续保留。跨平台相等不能代替旧算法行为、误差阈值和错误路径的独立
回归，也不能代替正式发布。结项不批量解除 `retention.json` 的退出条件：

| 参考分组 | 单元 | 本次决定 |
|---|---|---|
| 数值与渲染 | ear_post、output_dsp、pcm_mix、scene_numeric、hoa、live_vbap、triple_balance、saf、libear、resampler、ebur128 | 保留行为对照；当前矩阵不覆盖各参考测试的全部职责 |
| 独立监听 | monitor | 独立 Monitor API 不在本矩阵内，继续保留 |
| 元数据与容器 | wav_container、libadm、libbw64、dr_wav | PCM 验收不替代元数据、容器及 I/O 错误语义对照，继续保留 |

后续按[参考退出政策](RUST_REFERENCE_RETENTION.md)逐单元满足正式 tag、独立回归及退役审查条件，
再决定删除；历史验收记录不删除。本次没有新增发布 tag 或调整发布版本。

## 后续独立任务

本范围的阻塞项为零。以下工作不挂作二期未完成项：

1. **dr_flac 替换**：独立评估 Rust 解码器，验证 PCM、seek、短读/损坏输入和元数据兼容性；
   libFLAC 编码继续按原边界维护。
2. **扩展覆盖与确定性加固**：更多采样率/FFT 长度、HRTF 插值方向、退化 SVD、长时序 Scene，
   以及本矩阵之外的 Monitor/SOFA 等路径。新增证据通过后显式扩充门禁。
3. **继续性能回收**：基础蝶形、卷积和重采样热点分别推进，每项都保留原版位比较与 Release 基准。
4. **发布与参考退役**：在正式版本周期单独完成，不由本次技术验收自动触发。
