# Rust HpTF 耳机补偿 DSP 迁移

> 2026-10-04：系数设计、频响与自动预衰减、级联处理和双级联热切换迁入 Rust。

## 模块与边界

`mradm-dsp/src/hptf/design.rs` 实现 RBJ 系数设计、量化后稳定性检查、频响及可听频段峰值搜索；
`hptf.rs` 持有滤波历史、双级联状态、淡化进度和处理缓冲。算法库继续禁止 unsafe，
复用现有复数实现，没有增加第三方依赖或构建开关。

`mradm-ffi/src/hptf.rs` 提供私有 C 边界。C++ `HptfCascade` / `HptfProcessor` 通过不透明状态
持有 Rust 句柄，显式复制系数与 revision，不把 C++ 对象布局交给 Rust。`HptfCascade` 的复制仍为
独立历史的深拷贝；创建、复制和销毁属于非实时操作。

C++ 保留 AutoEq 文本解析、文件读取、参数验证的用户错误上下文，以及原有三槽 mailbox。
两个控制侧 mutex 仍只串行化发布者和状态读取者；音频线程不获取它们。
公开 C ABI、HpTF 参数结构、选项枚举及设备适用边界不变：仅处理项目生成的耳机馈送，
离线母版、多声道设备、system-spatial 床和裸 Scene pull 不增加补偿。

## 数值与运行行为

- 最多 32 个启用段；禁用段仍验证，设计时跳过中心频率达到或超过 Nyquist 的段。
  PK、LSC、HSC、LP、HP、BP、NO 均保留，搁架使用 Q 参数化。
- 系数以 double 设计后量化为 float，并对量化结果检查 Jury 稳定条件。
  运行使用 float PCM、float 系数和 double TDF2 状态。
- `auto_trim` 搜索 20 Hz 至 `min(20000 Hz, 0.49×采样率)`，保留极值候选、区间上界、
  4096 次细分预算、0.001 dB 余量和向零方向舍入的最终增益。预算耗尽时使用剩余上界。
  Rust 极值多项式使用 f64；原 C++ 的 long double 在本次 macOS arm64 / Windows x64 上也是 64 位，
  不据此承诺其他平台与旧实现逐位一致。
- 每次 `process` 内按最多 1024 帧处理；两条级联吃同一输入，新级联从零状态开始，
  按 2048 帧线性淡化。保留起点、末尾及跨短调用的进度。
- 淡化期间 C++ mailbox 保留最新待处理目标；当前淡化结束后，下一次 `process` 才接收新目标，
  即使当前调用跨过淡化结束点，也不会在同一次调用中开始下一次切换。
- seek/reset 优先采用最新待处理目标，其次采用当前淡化目标，再清空两条历史。pause 不增加 reset。
  Rust 返回一致的系数/revision，C++ 在处理调用返回后发布完成状态。原实现可以在内部 chunk 完成时
  发布；现在大块调用的状态可见时刻推迟至该调用结束，音频的淡化完成位置不变。
- 纯 bypass 精确短路，零帧不推进历史。非有限或全部小于阈值的历史按原策略在块末清零；
  非有限音频输入可能影响当前块输出，不会永久污染后续块。极小尾音的块末清理规则也保持原样。

准备后的首次处理、更新目标、完成淡化和 seek/reset 均无分配、无锁、无 I/O。
频响与系数设计在控制线程执行，可以分配峰值搜索队列。

## 错误边界

Rust 检查空句柄、长度溢出、不完整交错帧、参数和模式、系数有效性、目标采样率及当前淡化状态。
无效参数不修改音频、历史或返回快照。设计错误继续通过 C++ `Result` 返回；正常实时调用仅使用
已准备状态和已设计目标，违反其内部前置条件按 ADR 0005 视为编程错误。

私有 FFI 捕获 panic。HpTF 新增中文设计错误后，共用错误缓冲的截断改为保留完整 UTF-8 字符，
并增加小容量缓冲的终止符与越界保护测试；ASCII 错误的原行为不变。

## 验收结果

- macOS：Debug 56/56 CTest，Release 56/56。
- Windows canonical Release：55/55，SOFA ON，旧 SAF / ebur128 / samplerate 参考开关均 OFF。
- 最后一次 UTF-8 边界修正后，定向复测 macOS Debug 11/11、Release 10/10，Windows Release 11/11。
- Rust fmt、Clippy `-D warnings`，修改及新增 C++ 的格式、clang-tidy、cppcheck 通过；许可证与 SBOM 校验通过。
- 两个平台均保留 139 个公开 `adm_*` 导出，无私有 Rust 入口外泄。Windows 仍有 1,984 个附带 C++ 导出。

迁移前后的 Release `mr_adm_hptf_dsp_tests` 分别保存数值基线，参考代码为 `18da74f`。
覆盖 7 种滤波器 × 6 个采样率 × 2 种前级模式，共 84 组设计、频响和分块处理；
另有 5 组热切换、排队更新与 seek PCM。

| 最大绝对差 | macOS arm64 | Windows x64 | 门限 |
|---|---:|---:|---:|
| 84 组系数 / 前级增益 | 0 | 0 | `2e-6` |
| 84 组频响 / 峰值与衰减元数据（dB） | `8.52651283e-13` | `8.52651283e-13` | `0.002` |
| 89 组 PCM | `9.31322575e-10` | 0 | `2e-5` |

以上是同平台迁移前后的数值比较，未声称跨平台位相同。输入为程序生成信号，HpTF 不经过离线 CLI render。
既有测试继续覆盖 MDR-MV1 频响锚点、独立冲激 DFT、高 Q 低频和窄峰衰减、并发发布/查询、
文件与内存参数等价性、设备链中的峰值保护及 seek。新增 Rust 测试使用独立 Direct Form I 递推核对 TDF2，
并覆盖声道隔离、复制状态独立、无效调用不改变历史、非有限历史恢复及准备后分配计数。

Windows 原工作区的既有修改予以保留。同步前核对并备份原始字节；两个 HpTF C++ 文件原有差异仅为 CRLF，
随本轮同步转为 LF。12 个构建所需源码/测试文件与 macOS 一致，其余远端改动不纳入本轮。
本轮未运行 Linux CI、ThreadSanitizer、吞吐/RSS 基准或主观听音。

机器可读记录见 [validation.json](evidence/rust-hptf/validation.json)，
数值明细见 [macOS](evidence/rust-hptf/macos-comparison.json) 和 [Windows](evidence/rust-hptf/windows-comparison.json)。

## 复现

```sh
cmake --build --preset debug
ctest --preset debug -R '(hptf|realtime|stereo_peak_guard|scene_stream_c_api|rust_unit)' --parallel 6 --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base HEAD --build-dir build/debug

cmake --build --preset release
build/release/mr_adm_hptf_dsp_tests after.txt
python3 scripts/consistency/compare-hptf-dsp.py \
  --reference before.txt --candidate after.txt --output comparison.json
```

`before.txt` 必须由旧 C++ 实现的 Release 构建运行同一测试探针产生：先在 `18da74f` 应用测试探针
及其 CMake 注册，保持生产源文件不变，再生成基线。文本捕获是数值记录，
比较器拒绝缺失、重复、不等长或非有限记录。新增且尚未跟踪的 C++ 文件需另显式传给静态检查工具。
