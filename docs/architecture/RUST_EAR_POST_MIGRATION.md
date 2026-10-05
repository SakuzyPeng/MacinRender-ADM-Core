# Rust EAR 后处理与连续短块尾音修复

> 2026-10-04：去相关 FIR 卷积、直接声补偿延迟、双总线求和及历史迁入 Rust；基线为 `e12332b`。

## 所有权与接入

C++ `prepare` 继续调用 libear 的 `designDecorrelators<float>`，将每声道 512 个系数及
`decorrelatorCompensationDelay()` 的 255 样本延迟传入 Rust。临时系数随后释放。
`mradm-dsp::ear_post::FilterBank` 保存只读原始 FIR，通过 `Arc` 共享；每个输出实例的
`Processor` 独立持有 FFT plan、频域滤波器、511 样本 overlap、直接声环形延迟和工作缓冲。
FFT 长度仍为 `next_power_of_two(max_frames + 511)`，不同容量的实例不会误用同一频谱。

私有 `mradm-ffi::ear_post` 和 C++ `src/adm_dsp/ear_post.h` 提供创建/销毁、process 和 reset。
原准备句柄销毁后，已有实例仍持有自己的共享引用。C++ RAII 支持移动所有权，实例之间的历史独立。
算法库继续禁止 unsafe，未新增依赖、构建开关、公开 C ABI 或 GUI 参数。

离线与 `EarStream` 共用批量后处理接口：direct 原地替换为延迟后的 direct 加去相关 diffuse，
diffuse 只读且不得与 direct 别名。C++ 不再逐声道跨 FFI 调用 FFT，也不再分配原先的四个 FFT
临时 vector 和每声道直接延迟暂存。`wav71` 声道重排、I/O、计量、布局/ADM 算法和调度继续留在 C++。
本轮保留基线的时长裁剪块修复。

## 短块修复及行为契约

旧实现更新 overlap 时只保存当前块产生的新尾音。连续处理少于 511 帧的块时，尚未输出的旧尾音
会被丢弃。例如 FIR `[1, 0.5, 0.25, 0, …]` 接收一次冲激和两次零输入，按单帧调用时，
旧输出为 `[1, 0.5, 0]`，修复后为 `[1, 0.5, 0.25]`。

新输出仍为当前 IFFT 与旧 overlap 相加。新 overlap 在旧索引有效时计算
`y[frames+i] + old_overlap[frames+i]`，否则直接保存 `y[frames+i]`。
正向更新不会覆盖尚未读取的旧尾音。现有 renderer 采用至少 1024 帧的固定块加最后短块，
因此修复不改变正常调用网格，也不在 EOF 自动追加尾音或补零；输出时长保持原样。

- FFT 逆变换继续复用 `RealFft` 的 `1/N` 缩放；不增加归一化，保留“延迟 direct + 卷积 diffuse”的求和顺序。
- 255 样本直接声延迟使用预分配环形缓冲，任意合法分块的时序与原实现一致。
- 零帧不推进 FFT、尾音或延迟。reset 清零历史并保留准备资源；暂停不增加 reset。
  流式 seek 按旧规则清历史，离线窗口继续按原块网格预热。
- 创建验证通道、512-tap 形状、255 样本补偿、有限 FIR、有限频谱及 FFT/缓冲容量。
  处理前检查长度、完整帧、容量及乘积溢出；参数错误不改写输出或历史，未产出后缀保持原值。
  非有限音频继续按原有策略传播，不新增清洗或限幅；reset 后正常输入可恢复。
- Rust 内核与 FFI 的首次处理、连续短块、参数拒绝和 reset 均无分配、无锁、无 I/O。
  创建/销毁、libear 系数设计、C++ I/O 及整个 EAR 渲染器不包含在此承诺中。
  FFI 捕获 panic；调用方负责指针有效性、对齐、互不别名及可变句柄独占访问。

## 验证

`tests/reference/ear_post_legacy.h` 冻结旧实现，只参与测试。
Release 探针对真实 libear 的 2/6/12/24 声道布局、1024/1536/2048 帧容量，
以及 1/7/200/254/255/256/510/511/512/1023 帧最后短块进行同平台前后比较。

| 验证 | macOS arm64 | Windows x64 |
|---|---:|---:|
| 正常路径 4,520,868 个旧/新样本 | 逐位相同 | 逐位相同 |
| 连续短块与独立 double 时域 FIR 的最大绝对差 | `9.610994311515242e-8` | `9.535415301797912e-8` |
| 短块回归旧/新第三个输出样本 | `0 → 0.25` | `0 → 0.25` |

正常路径门限为 `2e-6 + 2e-6×|old|`；独立时域 FIR 门限为 `2e-5 + 2e-5×|reference|`。
Rust 测试另覆盖最大容量 1/37/1024/2048、单帧及不规则分块、255/511/512 附近边界、
静音尾音、声道隔离、直接延迟逐位验证、独立实例、reset、非有限历史恢复和失败原子性。
该短块能力按线性 FIR 数值正确性验收，不承诺不同 FFT 分块逐位相同。

- macOS Debug：59/59 CTest；Release 定向回归：10/10。
- Rust Release：5/5 EAR 后处理测试、9/9 分配测试；FFI 边界和共享句柄生命周期随 workspace CTest 验证。
- Windows canonical Release：58/58，SOFA ON，三个旧库参考开关 OFF。
- 13 组 Release CLI 对照全部逐位相同，包括 6 个 EAR 用例及 7 个未修改后端控制用例。
  这些是小型合成输入；不作主观听感、吞吐/RSS 或跨平台位一致结论。
- EAR 流式回归明确验证 float32 文件、不同 pull 分块逐位一致及 stream 与离线结果逐位一致。
  既有窗口/整段切片逐位检查、seek、C API 和时长裁剪块回归继续通过。
- Rust fmt/Clippy、改动 C++ 格式/clang-tidy/cppcheck、冻结参考检查及许可证/SBOM 校验通过。
- macOS/Windows 保留同一组 139 个公开 `adm_*` 导出，无私有 Rust 入口外泄；Windows 附带 C++ 导出不属于稳定 ABI。
- 最终三平台 CI 的提交、链接及结果记录在机器可读验收文件中。

Windows 继续使用既有 canonical 工作区，原有修改和原始字节均保留。
本轮同步包含 `e12332b` 的时长裁剪修复；涉及文件在同步前核对基线，之后核验源码指纹。
其他 Windows 工作区差异不属于本轮，原生验证不描述为干净的 Git 检出。

证据：[validation.json](evidence/rust-ear-post/validation.json)、
[macOS 数值](evidence/rust-ear-post/macos-comparison.json)、[Windows 数值](evidence/rust-ear-post/windows-comparison.json)、
[Release CLI 对照](evidence/rust-ear-post/macos-audio.json)。

## 复现

```sh
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base e12332b --build-dir build/debug
scripts/quality/check-licenses.sh --build-dir build/debug
cmake --build --preset release
build/release/mr_adm_ear_post_tests comparison.json
python3 scripts/consistency/compare-pcm-mix.py \
  --reference /path/to/e12332b-release/mradm --candidate build/release/mradm \
  --pcm-bits build/release/mr_adm_pcm_bits --fixtures /path/to/fixtures --output comparison-cli.json
```

复用现有构建目录和共享 Cargo 产物根目录。音频对照使用 Release，比较器仅保留摘要和指纹。
