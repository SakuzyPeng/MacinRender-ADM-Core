# Rust 峰值保护与实时增益迁移

> 2026-10-04：设备立体声峰值保护、单通道增益渐变和多通道增益平滑迁入 Rust。

## 实现与边界

`mradm-dsp` 的 `gain` 与 `peak_guard` 模块持有计算和状态，继续禁止 unsafe；
`mradm-ffi` 的 `output_dsp` 模块提供不透明句柄、显式浮点数组长度和批量操作。
C++ 适配层负责 RAII、移动所有权及现有调用语义。没有新增第三方依赖、构建开关或公开 C ABI。

EAR、VBAP、HOA 继续在空间混音前原地处理交错 PCM。双耳和 Apple 的每条总线一次填充一块
已分配的单通道包络；Triple Balance 一次填充交错增益包络。生产调用没有逐样本跨 FFI。
`LiveGainRamp::next()` 仅保留作内部单样本测试入口。
Triple Balance 的 seek 按原规则重新应用当前目标，但重置改为复用已准备的状态；
其他后端继续保留各自的增益历史和控制发布时间，没有统一改写 seek/reset。

Scene/Monitor 的线程、队列、设备生命周期、元数据解析和控制发布继续留在 C++。
本轮不改共享矩阵混音、Monitor 淡化、UI Peak/RMS 或 Triple Balance 专有算法。

## 行为契约

- 增益渐变使用原有 float 运算及累加顺序；时长为 `max(1, rate × ramp_ms / 1000)`，默认 20 ms。
  初次处理采用当前目标；中途改目标从下一样本的当前值继续；相同目标不重启。
  多通道目标不足时其余通道回到单位增益，超出通道数的目标被忽略。零帧不启动或推进渐变。
- 峰值保护保留 `max(1, rate / 200)` 帧前瞻、4096 帧输入容量加前瞻、100 ms 释放、
  双耳共用增益，以及 `0.89125094`（-1 dBFS）样本峰值上限；这是样本峰值保护。
  音量在 `pop` 时参与检测和输出，低于上限且无历史衰减时精确保留原音量缩放。
- HpTF 仍在保护器上游；暂停保留缓冲及包络，epoch 清空它们。EOS 排空原有帧，不增加补零；
  消费/呈现计数仍由 C++ 根据实际输出帧更新。非有限输入在峰值保护器中归零。
  普通增益乘法保持原有 PCM 传播行为，不新增清洗策略。
- 准备后的首次处理、目标更新、包络输出、原地增益、峰值缓冲操作、排空和 reset 无分配、无锁、无 I/O。
  创建/销毁及 Rust 测试使用的状态克隆属于非实时操作；不承诺整个场景引擎零分配。
- 私有 FFI 拒绝空句柄、非空缓冲的空指针、长度溢出、不完整帧、无效模式/结束标志及非有限增益目标。
  峰值音量保持 `[0,1]`。错误返回不修改音频、历史或输出计数；创建失败清空输出句柄。
  调用方仍须保证指针有效、对齐、生命周期和非别名。panic 不穿越 C ABI；实时内部契约违反按 ADR 0005 处理。

## 验证

测试专用 `output_dsp_legacy.h` 冻结 `53cc36e` 的原 C++ 算法，只参与比较可执行文件。
Release 比较覆盖 1/2/12/64 通道、1/8/44.1/48/96/192 kHz 增益、0/4/20 ms 渐变、
首次目标、重复目标、中途重定向、缺省目标、重置及移动所有权。
峰值对照覆盖 8/44.1/48/96/192 kHz、1/37/512/4096 帧输入与短输出缓冲、
环形回绕、音量变化、非有限样本和 EOS，有限测试输入绝对值不超过 8。

| Release 同平台新旧对照 | macOS arm64 | Windows x64 |
|---|---:|---:|
| 增益包络及乘法，11,642,376 个样本 | 逐位相同 | 逐位相同 |
| 峰值保护，440,120 个样本的最大绝对差 | `2.384185791015625e-7` | `0` |
| 峰值保护比较门限 | `2e-5` | `2e-5` |

Rust 独立断言另检查渐变端点、连续性、分块一致性、双耳比例、上限、透明/静音输出、
短 EOS、错误调用的原子性及准备后分配计数。既有设备捕获测试继续验证暂停、epoch、
呈现计数及 HpTF 增强后的保护；既有后端回归覆盖实时覆盖参数、seek 和头部跟踪。

- macOS Debug 全套 57/57；Release 受影响回归 16/16，Rust 独立输出测试 4/4、分配测试 7/7。
- Windows canonical Release 全套 56/56；SOFA ON，三个旧库参考开关均 OFF。
- 测试探针增加重定向覆盖及整理后，两端分别定向复查该 Release 对照，macOS 同时复查 Debug 对照。
- Rust fmt、Clippy `-D warnings`，C++ 改动检查、许可证/SBOM 检查通过；Apple 既有静态检查建议保留。
- 两端保留同一组 139 个公开 `adm_*` 入口，私有 Rust 入口不外泄。Windows 仍有附带的 C++ 导出。
- 基线 `53cc36e` 的 [CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37226615086)
  已通过 macOS/Linux/Windows Debug。实现提交 `496e108` 的
  [最终 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37228025183)
  通过 macOS 57/57、Linux 56/56、Windows 56/56。Windows 首次在测试程序链接/依赖部署时
  遇到文件占用，仅重跑失败任务后通过，期间没有修改源代码。

Windows 使用既有 canonical 工作树，并保留其原有修改；本次同步前核对全部相关文件，
四个已有 C++ 文件的差异仅为 CRLF，备份原字节后随同步转为 LF。验收不代表干净的 Git 检出。
本轮不声称跨平台位一致、吞吐/RSS 提升或主观听感变化。

机器可读验收见 [validation.json](evidence/rust-output-dsp/validation.json)，
数值对照见 [macOS](evidence/rust-output-dsp/macos-comparison.json) 和
[Windows](evidence/rust-output-dsp/windows-comparison.json)。

## 复现

```sh
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base 53cc36e --build-dir build/debug
scripts/quality/check-licenses.sh --build-dir build/debug

cmake --build --preset release
build/release/mr_adm_output_dsp_tests comparison.json
ctest --test-dir build/release -R '(output_dsp|stereo_peak_guard|hptf|scene_.*c_api|realtime|ear_fixture|vbap|hoa_.*fixture|binaural_fixture|triple_balance_stream|apple_smoke)' --parallel 6 --output-on-failure
```

比较器对增益使用 float32 位比较，对峰值 PCM 使用绝对误差门限；长度、非有限结果或
未输出区域被改写均直接失败。独立断言与参考算法共同约束行为，不使用音频文件大小推断一致性。

## 参考实现退出

- 单元 `output_dsp`（`tests/reference/output_dsp_legacy.h`）：随默认测试构建编译，由 `mr_adm_output_dsp_tests` 比较。

按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)的通用条件退役；登记与文件哈希见 [`tests/reference/retention.json`](../../tests/reference/retention.json)。当前状态：保留（Rust 实现尚未随正式 tag 发布，独立回归与二期需求待评审）。
