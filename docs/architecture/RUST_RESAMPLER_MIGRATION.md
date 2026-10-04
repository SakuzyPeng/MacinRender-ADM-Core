# Rust 重采样迁移

> 2026-10-04：全部直接使用 libsamplerate 的生产调用已迁入 Rust。

## 实现

| 位置 | 职责 |
|---|---|
| `rust/crates/mradm-dsp/src/resampler.rs` | 安全的固定倍率 interleaved f32 转换；rubato sinc、准备好的缓冲、消费/生成计数、延迟补偿、EOS、reset |
| `rust/crates/mradm-ffi/src/resampler.rs` | 指针与长度检查、完整帧和 EOS 标志验证、panic 边界；所有内存由 Rust 原分配方释放 |
| `src/adm_dsp/resampler.h` | 可移动 RAII owner 与 `Result` 错误接口 |
| `src/adm_realtime/scene_stream_engine.cpp` | 渲染后的输出重采样，保留 epoch/preroll/有理时间线；当前 slice 的暂存输出在更换 silence/preroll 标志前排空 |
| `src/adm_render_binaural/live_binaural_renderer.cpp` | 复用同一个准备好的 resampler，逐条 HRIR reset、转换并真正排空尾部 |

采用固定 `rubato 5.0.1`，默认 features 关闭；质量参数和时序选择见 [ADR 0011](../adr/0011-rust-fixed-rate-resampling.md)。
新增八个锁定包（rubato、三种 audioadapter 包、audio-codec-algorithms、windowfunctions、visibility 和 syn 2）；
不启用第三方 C FFI、FFT resampler 或运行时倍率调整。`mradm-dsp` 继续禁止 unsafe。

同采样率旁路保持 PCM，变采样率的算法和波形允许改变。准备后的处理、排空和 reset 经分配计数确认无分配；
整个 Scene renderer 的队列、场景及缓存分配不包含在此承诺中。内置 KEMAR 和外部 SOFA 的实时采样率转换
共用该实现；离线双耳的采样率限制保持原样。miniaudio 设备后端自己的重采样不属于 libsamplerate 调用。

## 数值边界

最终时长始终以整数有理数计算，避免每块独立取整。合法空输入只排空暂存输出；finish 明确关闭输入，
重复 finish 返回零，之后须 reset 才能继续输入。同采样率亦遵循相同生命周期。
缓冲必须含完整帧，输出至少容纳一帧；无效参数与非有限输入在状态推进前被拒绝。

不能直接裁掉 rubato 名义延迟的整数部分：其初始 stepping 和相位表约定会让早期脉冲提前。
当前封装按实际相位裁到最近输出帧。独立正弦拟合在 48→44.1、44.1→48、48→44.101、47.999→48、
96→48、192→8、8→192 kHz 检查残余相位，小于 0.51 个输出样本；这不是零相位差或跨平台位一致承诺。
tap-zero 测试同时防止短 FIR 的首脉冲被裁掉。

HRTF 的输出仍裁到输入持续时间对应的 ceil 长度，保留原来的幅值约定，未引入额外归一化。
短 IR 在文件起点前/终点后的 sinc ringing 会被截断，所以 DC 面积不能简单作为两种滤波器的增益对照。
人工 256-tap 短 HRIR 在极端 192→8 kHz 时，两库的裁剪后面积比约 0.9577；这项差异明确记录而未隐藏。
独立长脉冲测试在完整滤波支持范围内检验面积随倍率的变化，误差门限 0.1%。

## 验证

Release C 参考工具运行 40 组检查/测量：24 个通带频点、4 个降采样阻带频点、8 个短 HRIR 与
2/12/24/64 声道隔离检查。采样率组合覆盖 8/32/44.1/48/96/192 kHz。
macOS 测得通带最大幅度误差约 **0.00235 dB**，测试阻带频点的最小抑制约 **137.27 dB**。
旧 `SRC_SINC_MEDIUM_QUALITY` 在所测 90% Nyquist 频点约衰减 2.14 dB，新内核约衰减 0.002 dB。
这是所列频点的合成信号测量，不能当成全频段最坏值、主观听感或整机性能结论。

Rust 独立测试还覆盖 1/7/127/511/1024 帧分块、单帧输出空间、44.101/47.999 kHz 互质时钟、
静音、单帧音频、reset、新旧状态一致性、完整持续时间、非有限值及错误不污染状态。
新增 Scene C API 回归同时转换 KEMAR HRTF 与输出（输入 8/44.1/96/192 kHz，输出 48 kHz），
验证有限信号和 EOS 长度。已有回归覆盖 seek/preroll、多帧有理时间线、backend/policy 切换及设备输出。

| 环境 | 结果 |
|---|---|
| macOS arm64 Debug | 54/54 CTests |
| macOS arm64 Release，C 参考开启 | 55/55 CTests |
| macOS arm64 Release，最终生产配置 | 54/54 CTests，C 参考关闭 |
| Windows x64 canonical Release，SOFA ON，C 参考开启 | 54/54 CTests；40 组参考检查/测量通过 |
| Windows x64 canonical Release，最终生产配置 | 53/53 CTests，C 参考关闭 |
| Rust 质量 | fmt、Clippy `-D warnings` 通过 |
| C++ 质量 | 改动代码与新增测试/头文件检查通过；既有 Scene fixture 复杂度/嵌套条件建议保留 |
| 许可证 | 51 个 Cargo 锁定包、资源指纹、manifest、SBOM、许可证 bundle 一致性通过 |
| ABI 与生产链接 | 两端全部 139 个公开 C 函数保留，无私有 Rust 导出；生产链接不含 C libsamplerate |

Windows 原生验收及 ABI 结果记录在 [机器可读验收记录](evidence/rust-resampler/validation.json)。
两端的 [macOS 参考数据](evidence/rust-resampler/macos-reference.json) 与
[Windows 参考数据](evidence/rust-resampler/windows-reference.json) 保留逐用例测量。
Windows 继续使用原有工作树与 canonical MSVC/Ninja 目录，保存原字节并核验哈希；保留其它既有修改。
其旧许可证表按同步的 manifest 重建，正文保留；两个测试文件仅统一原有 CRLF 后合并本次改动。
该验证不代表干净的本地提交检出。本轮未运行 Linux CI，也没有进行主观试听或吞吐性能评估。
Windows 的原有 C++ 自动导出仍保留，稳定 ABI 只指上述公开 C 函数。许可证漂移检查仅将 CRLF/LF
视为相同，避免 Windows Git 检出造成误报；其它文本差异仍报错，既有许可证原字节保留。

## 复现

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-licenses.sh --build-dir build/debug

cmake --preset release -DMR_ADM_BUILD_SAMPLERATE_REFERENCE_TESTS=ON
cmake --build --preset release
build/release/mr_adm_resampler_reference_tests
ctest --test-dir build/release --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --preset release -DMR_ADM_BUILD_SAMPLERATE_REFERENCE_TESTS=OFF
```

`libsamplerate` 只在可选参考目标中使用；生产链接及全部 139 个公开 C 入口分别验证。
