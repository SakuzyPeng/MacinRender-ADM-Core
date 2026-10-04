# ADR 0011：固定采样率转换迁入 Rust

> 状态：已接受
>
> 日期：2026-10-04

## 决策

以 `rubato 5.0.1`（关闭默认 features）的异步 sinc 内核替换生产 `libsamplerate 0.2.2`，
用于 Scene 空间渲染后的输出转换及实时双耳准备期的 HRTF 转换。
采用项目自有安全 `Resampler` 和私有 FFI；C++ 不接触第三方对象或缓冲适配器类型。
不新增公开 C ABI、质量枚举或动态倍率接口。所有处理仍在 preparation/worker 线程，设备 pull 不变。

固定使用 Blackman–Harris 平方窗、256 基础 taps、0.94 相对截止频率、128 倍相位表和 cubic 插值。
降采样按输入/输出倍率扩展 tap 数并对齐到 8，以维持相对于较低 Nyquist 的过渡宽度。
单次输入上限 1024 帧，实际可接受任意较短分块；输出不足时保留剩余帧，调用方继续排空。
准备后 process、finish、reset 无分配；同采样率直接复制。

选择 Async 而不是同步 FFT resampler，是为了支持公开范围内的任意整数采样率及短 SceneFrame，
避免互质采样率要求很大的 FFT 分块。Scene 输入/输出仍为 8–192 kHz；私有重采样器保留旧库的
1/256–256 倍率范围以覆盖 HRTF 数据。过小/过大倍率、无效通道、非有限 PCM、错误帧形状明确报错。

## 时间与质量契约

- 整数有理数计数保持 EOS 长度 `ceil(total_input * output_rate / input_rate)`。Scene 自己的
  preroll、target、backend tail 和最终 timeline 裁剪保持原有职责。
- 滤波等待导致的启动延迟在输出端裁掉；末尾以零延伸真正排空 sinc，不用补零替代滤波尾部。
- `rubato 5.0.1` 的首个输出先前进一个 ratio step，相位表还有 1/128 输入样本的偏移。
  因而按实际相位将延迟舍入到最近输出帧，不能直接向下取整名义滤波延迟。测试锁定这一约定，
  依赖升级必须重做 tap-zero、相位、reset 和分块对照。仍允许小于约半个输出样本的剩余相位差，
  不宣称与旧库的采样网格严格相同。
- HRTF 逐滤波器 reset，输出保留 `ceil(old_length * ratio)` 个 taps，不额外归一化或乘反倍率。
  两耳使用相同采样网格，历史互不混合。不同 sinc 内核及有限长度裁剪会改变边缘 ringing 和短 IR 面积。
- 验收采用独立通带/阻带、脉冲、相位、时长、分块、通道隔离与 C 参考测量，不要求旧库逐样本相等。

## 依赖与发布

`MR_ADM_BUILD_SAMPLERATE_REFERENCE_TESTS` 默认 OFF；只有开启时才解析/构建 C 库，生产 target 不链接它。
Cargo.lock、manifest、许可证文本、生成的许可证表与 SBOM 同步更新。继续复用现有 Cargo target 和原生构建目录。
设备库 miniaudio 内部的采样率适配属于设备后端，本决策不替换该部分。

验收及测量限制见 [迁移记录](../architecture/RUST_RESAMPLER_MIGRATION.md)。
