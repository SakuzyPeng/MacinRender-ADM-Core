# Rust Monitor 淡化、seek 过渡与 Peak/RMS 迁移

> 基线 `a54fdc3`。Monitor 数值计算和状态由 Rust 接管，设备与线程调度保持原样。

## 所有权与接入

`mradm-dsp::monitor` 继续禁止 unsafe，提供两个互不共享可变状态的实例：worker 独占
`Crossfade`，音频回调独占 `Output`。前者持有后端淡化位置，后者持有最后输出帧、seek 锚点、
剩余过渡长度和已观察的 generation。两个实例都在 worker 和设备启动前准备完成。

`mradm-ffi::monitor` 提供私有不透明句柄、显式浮点数组长度、帧数和 `uint64` generation。
C++ 可移动 RAII 适配器按块调用；worker 原地混合两路 PCM，回调在一次调用内完成 seek
过渡和 Peak/RMS，再将结果写入原有原子快照。没有逐样本跨 FFI，也没有重复保存数值状态。

C++ 继续负责 stream 所有权、控制发布、seek 取消、设备生命周期、SPSC 队列、I/O、
LUFS 调度和报告。既有 PCM 暂存与分块保留，Rust 只按声道准备历史，不新增回调帧数上限。
公开 C ABI、GUI 参数、支持范围、依赖和构建开关均未改变。

## 行为契约

- 后端淡化固定为 2048 帧：先以 double 计算 `min(1, (position + frame) / 2048)`，再转 float，
  按 `(old * (1-t)) + (incoming * t)` 求值。第一帧权重为零，下一块接续同一位置；
  C++ 继续取两路实际产出的最小值，并在完成或短读时切至 incoming stream。
- seek 过渡为 `max(1, rate * 10 / 1000)` 帧，权重保留 float 的 `elapsed / (total-1)`；
  单帧取 1。实时设备从最后输出帧衔接，推送设备从静音淡入。只有 active 且有真实帧的调用
  才消费新 generation；generation 只比较不等，不增加单调性限制。
- 非零帧 inactive 回调清零最后输出帧并取消过渡，未消费的 generation 保留。
  实时短读记录包含补零或 HpTF ring-out 的最后输出，并取消未完成过渡；推送设备只记录
  已产生的帧，允许过渡跨短调用延续。零帧不修改历史，但发布零 Peak/RMS。
- 顺序仍为 `HpTF → seek 过渡 → Peak/RMS`，LUFS 保持原有 pre-HpTF 取样位置。
  output-stage 切换继续硬切换、通知 generation 并清 HpTF 历史；没有引入双路淡化。
- Peak/RMS 对整次请求计量，包括未产生部分的补零，最多报告前 64 声道；过渡处理全部声道。
  Peak 保留 `std::max(accumulator, abs(sample))` 的 NaN 行为；RMS 按原帧顺序以 double
  累加平方，再 double sqrt、转 float。音频不增加清洗、限幅或端点乘法短路。
- Rust reset 恢复创建态；生产 seek 和暂停仍通过原有事件/回调时序工作，不统一 reset。
  连续切换或 seek 先结算 incoming stream，淡化数值进度按原调用点清零。

FFI 在修改前检查长度、完整帧、容量、标志、计数溢出、空/未对齐指针和缓冲重叠；
参数拒绝不修改 PCM、历史、计量输出或完成标志。调用方仍保证内存存活、句柄独占，
错误消息缓冲有效且与其他参数分离。panic 复用现有捕获和错误码边界。
C++ 在消费队列前检查回调容量与长度溢出；内部契约违反按 ADR 0005 终止，回调不创建错误消息或写日志。

内核、FFI、reset 和参数拒绝从准备后的首次调用起无分配、无锁、无 I/O。C++ 新增计量
传输使用固定栈数组；创建/销毁和测试状态克隆不属于实时操作，不承诺整个 Monitor 引擎零分配。

## 验证

测试参考 `tests/reference/monitor/legacy.h` 从基线直接提取三段循环和数值状态；仅隔离类型、
将原子读写替换为参数/输出，并格式化。来源与参考 SHA-256 见同目录 `provenance.json`。

Release 同平台对照覆盖 1/2/12/64/96 声道，1/1000/8000/44100/48000/96000/192000 Hz，
两种设备模式及 0～8193 帧不规则请求。一般有限 PCM 门限为 `2e-6 + 2e-6 * abs(reference)`；
精确端点、直通及简单混合逐位检查。Peak/RMS 对相同输入逐位比较，非有限值检查分类。

| 233,435,903 个数值的新旧对照 | macOS arm64 Release | Windows x64 canonical Release |
|---|---:|---:|
| 后端淡化最大 PCM 绝对差 | `5.960464477539063e-8` | 0 |
| seek 过渡最大 PCM 绝对差 | `1.192092895507813e-7` | 0 |
| 包含过渡的 Peak/RMS 最大绝对差 | `1.192092895507813e-7` | 0 |
| 相同 PCM 输入的 Peak/RMS | 逐位相同 | 逐位相同 |

独立 Rust 测试验证端点、重新定向、短读锚点、延迟消费 generation、暂停/恢复、generation
回绕、超过 64 声道、零帧、reset、计数溢出、错误原子性、实例隔离及无欠载时分块一致性。
Rust 内核和 FFI 分别有首次调用分配探针。原生对照另验证 RAII 移动与非有限传播。

Monitor 捕获回归增加整次回调计量、零帧计量、完整 2048 帧权重、半途再次切换或 seek、
短 incoming EOS 的精确输出长度。短读测试保留队列腾出完整 worker 块后才继续探测 EOS 的调度。
既有测试继续覆盖 HpTF、延迟设备 flush、快速 seek→play、output-stage、取消及 LUFS。

本地 macOS Debug 全套 62/62、Release 相关回归 16/16，Windows canonical Release 全套 61/61；
Rust Release workspace 106 项通过。Rust fmt/Clippy、改动 C++ 格式/clang-tidy/cppcheck、
冻结参考及许可证/SBOM 检查通过。两个原生平台保留原有 139 个公开 `adm_*` 入口，私有 Rust 符号不外泄。
[最终三平台 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37309535034)
验证 `04e4e81`：macOS Debug 62/62、Linux Debug 61/61、Windows Debug 61/61，全部通过。
生产实现提交为 `d70ac2e`；`04e4e81` 仅补齐比较测试的括号及指纹，两端 Release 比较已复查。
先前启动的 CI 主动取消，以最终测试源码重新验收。最终验收提交仅更新文档和证据。
完整记录见[机器可读验收记录](evidence/rust-monitor/validation.json)。

Windows 复用既有 dirty 工作树和 canonical Release（SOFA ON，旧库参考开关 OFF）。同步前核验
文件指纹并备份原字节，Monitor/realtime fixture 的两份已有差异通过三方合并保留；该原生
验证不代表干净 Git 检出。干净平台验证由最终 CI 单独记录。

## 复现

```sh
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build --preset release
build/release/mr_adm_monitor_dsp_tests comparison.json
ctest --test-dir build/release -R '(monitor_dsp|realtime|hptf|scene_.*c_api|c_api_tests|ear_fixture|vbap|hoa_.*fixture|binaural_fixture|triple_balance_stream|apple_smoke)' --parallel 8 --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base a54fdc3 --build-dir build/debug
scripts/quality/check-licenses.sh --build-dir build/debug
```

继续复用标准 Debug/Release 构建及共享 Cargo 缓存，不创建独立工作区。跨平台逐位一致
留到二期；本批不推断吞吐、延迟或 RSS 改善，也不改变 GUI 电平显示口径。
