# Rust 双耳卷积与滤波过渡迁移

> 2026-10-04：实时 Scene、离线双耳和旧流式双耳接口均使用项目自有 Rust 内核。

## 范围与边界

`mradm-dsp/src/convolution.rs` 接管卷积、滤波器过渡、输入历史、输出 overlap 和 FFT 工作区；
`diffuse.rs` 合并两处相同的八 tap diffuse 延迟，并处理实时输入的 gain/diffuse 逐样本 ramp。
算法库继续使用 `#![forbid(unsafe_code)]`，复用 RealFFT/RustFFT，没有增加依赖或构建开关。

`mradm-ffi/src/convolution.rs` 提供私有句柄 API，C++ 的 `src/adm_dsp/binaural_dsp.h` 只负责 RAII、
借用缓冲及错误传递。频谱按每 bin 的 `[L.re, L.im, R.re, R.im]` 浮点数组传递，不在 Rust 中强转
C++ complex 对象。句柄由 Rust 创建和释放；FFT 工作区不能并发使用，每个 OLA worker 持有独立工作区。
Live workspace 按声源顺序复用，各声源的 state 独立拥有全部存储，可先于或晚于 workspace 销毁。

FFI 检查空句柄、缓冲长度、尺寸溢出和 ramp 标志；算法入口先检查有限输入、增益及频谱。
无效参数不会推进历史或改写输出；合法的空音频调用不推进状态。Panic 在私有边界转为错误码，
C++ 内部沿用 `dsp::check` 及现有 renderer/worker 异常边界，不增加公开 C ABI。

C++ 继续负责 ADM/Scene 语义、方向与 extent 解析、HRTF 幅度/相位插值和缓存、线程调度、声源求和及 I/O。
设备回调和公开 C ABI 的职责不变。此次未迁移重采样、HPTF、通用监听 gain ramp 或后端拓扑切换。

后续进展：[重采样迁移](RUST_RESAMPLER_MIGRATION.md)已接管 Scene 输出和实时 HRIR 转换；
[HRTF 迁移](RUST_HRTF_MIGRATION.md)已接管幅度/相位插值、连续方向查询及相关频域状态。
上文保留本轮卷积迁移时的边界，当前所有权以这两份后续记录为准。

[Scene 数值迁移](RUST_SCENE_NUMERIC_MIGRATION.md)随后接管 Live 双耳剩余的数值控制、
cloud 合成和累加，并通过共享 HRTF 表与整帧预检查补齐状态所有权和错误原子性。

## 保留的两种卷积契约

| 路径 | 历史与过渡 |
|---|---|
| Live Scene overlap-save | 保留完整 N 点 HRTF 逆变换 FIR 及 N−1 输入历史；两端滤波器使用同一输入 FFT。控制跳变持续淡变 10 ms，短调用可跨块继续；新目标从当前滤波器状态出发。显式元数据 ramp 的端点在下一段生效。输出覆盖调用方缓冲。 |
| 离线 / BinauralStream overlap-add | 保留测量 HRIR 长度对应的 overlap 及短块残留；silent gap 在原时间位置输出尾音。块内 crossfade 包含首尾端点，共享进入块前的 overlap，之后保留结束滤波器的尾音。单帧块输出起始端、保留结束端尾音。输出累加至调用方缓冲。 |

两条路径的原有滤波支持范围和增益作用位置保持原样；没有借迁移统一成同一算法。
特别是 batch 仍使用测量 HRIR 长度的 overlap，不宣称获得 Live 完整插值 FIR 的分块不变性。
离线和旧流式接口共用同一个 Rust OLA 实现，继续满足它们之间的逐位相同契约。

Live gain/diffuse 在保存输入历史之前混合，历史样本不重新施加当前增益；全程 mute 时延迟线输入为零。
尾音计数沿用从最后一个有信号的分块末尾计算的保守值，不能把它解释成最后非零样本的精确位置。
Reset 清空信号历史和过渡状态，保留准备好的内存；已进入静音休眠的 Live 声源不反复清空大缓冲。

OLA 的旧实现每次 crossfade 复制两个含 vector 的状态并各做一次输入 FFT。
新实现只计算一次输入 FFT，以预分配的起始输出缓冲配合结束端更新 overlap，不再复制/分配状态。
Rust 分配探针从准备后的第一次处理开始计数，确认滤波展开、持续/中断过渡、多个 Live state、
OLA crossfade、静音推进、diffuse 及 reset 均无 alloc/realloc；这不是整个 C++ 场景渲染器零分配承诺。

## 验证

| 验证 | 结果 |
|---|---|
| macOS arm64 Debug | 53/53 CTests；最后的静音跳过调整后，双耳/Scene/实时定向复查 5/5 |
| macOS arm64 Release | 53/53 CTests；另以 Cargo release 运行独立卷积/延迟对照及分配探针，8/8 |
| Windows x64 canonical Release，SOFA ON | 52/52 CTests |
| Rust 质量 | fmt、Clippy `-D warnings` 通过 |
| C++ 质量 | 改动文件与新增头文件的格式、clang-tidy / cppcheck 检查通过 |
| 公开 ABI | macOS / Windows 均保留全部 139 个 `adm_*` 函数，无新增私有 Rust 导出 |
| Release CLI 对照 | 8 组迁移前后 PCM 在本机逐位相同，最大绝对误差均为 0 |

独立 double DFT/时域 FIR 对照覆盖双耳不同滤波器、完整 FIR、1/7/37/64 帧分块、静音尾音、
10 ms 淡变中断、显式 ramp、OLA 单帧/短段/增益变化及 silent gap。另验证 reset、独立 state、
移动所有权、长度/非有限值错误和失败不污染历史。既有 renderer fixture 覆盖真实 KEMAR 的运动、
headLocked、窗口、离线/旧 stream 一致性及 Scene C API。SOFA fixture 继续验证原始数据与渲染接入。

CLI 对照以迁移前 `56f4b56` 的 Release 程序为基线，覆盖点声源、固定头部姿态、cloud/diffuse、
多声源、spreader/diffuse、Cartesian、DirectSpeakers 和裁剪窗口。这些是合成、固定姿态用例，
不能据此承诺所有节目或跨平台逐位相同；动态过渡由上述独立内核和 renderer 测试验证。

Windows 复用原有工作树与 canonical 构建目录，同步前逐文件验证与本次本地基线一致并备份原字节。
该工作树的其它既有修改（包括此前保留的 Monitor/realtime fixture 差异）保留，验收不代表干净的
本地提交检出。Windows 仍有自动导出的 C++ 实现符号，它们不属于稳定 C ABI。本轮未运行 Linux CI。

音频对照与源文件指纹见 [验收记录](evidence/rust-binaural-dsp/validation.json) 和
[Release PCM 对照](evidence/rust-binaural-dsp/macos-release-audio.json)。

[五轮交替 Release 测量](evidence/rust-binaural-dsp/macos-release-performance.json)中，
point/cloud/spreader 的新旧耗时比分别为 1.076/1.041/0.981，RSS 比为 1.006/0.951/1.000。
这些 1 秒合成 fixture 包含进程启动与 HRTF 准备；未修改的 EAR/VBAP 控制用例也有约 1.6%/6.3%
耗时变化，不能据此归因内核变化或宣称吞吐提升。零分配结论来自独立分配计数，不来自这组计时。

## 复现

```sh
cmake --build --preset debug
ctest --preset debug -R '(rust|binaural|scene_.*c_api|realtime)' --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base HEAD --build-dir build/debug

cmake --build --preset release
ctest --test-dir build/release --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
# reference 指向预先保存的迁移前 Release mradm；fixtures 使用 mr_adm_make_fixture 的输出。
python3 scripts/consistency/compare-binaural-dsp.py \
  --reference /path/to/reference/mradm --candidate build/release/mradm \
  --pcm-bits build/release/mr_adm_pcm_bits --fixtures /path/to/fixtures \
  --output out/binaural-comparison.json
```
