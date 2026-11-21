# 七期 7.4 Apple 平台岛

APAC worker 通过 objc2 的 AudioToolbox 绑定直接编码。AUSpatialMixer 后端与 AVSampleBufferAudioRenderer 系统空间输出继续通过各自的岛表调用。参考实现为 `d3c7ae68`，采用 [7.1 的评估标准](RUST_ISLAND_BASELINE.md)。

## 三条路径的取舍

| 路径 | 决定与依据 |
| --- | --- |
| APAC | 迁入 Rust。ExtAudioFile 的创建、同步写帧、关闭，AudioConverter 属性和 CoreFoundation 文件 URL 均有绑定。单个 worker 线程拥有文件，逐块同步借用 PCM，适合用一个 Rust 类型管理文件生命周期。删除 C++ 实现、私有头和函数表。 |
| AUSpatialMixer | 保留 C++ 岛。绑定覆盖所用的 AudioUnit 属性、空间参数、`AURenderCallback` 与 `AudioUnitRender`。回调持有每条输入总线的上下文，使用引擎预备的交错样本与逐帧增益，并遍历系统提供的变长 `AudioBufferList`。这些指针要一直有效到 AudioUnit 停用。沿用准备、离线渲染、打开文件流和销毁四个入口，通道计划由 Rust 提供。[AURenderCallback 绑定](https://docs.rs/objc2-audio-toolbox/0.3.2/objc2_audio_toolbox/type.AURenderCallback.html) |
| AVSampleBufferAudioRenderer | 保留 Objective-C++ 岛。AVFoundation 和 CoreMedia 的绑定覆盖空间化设置、入队、时钟、块与样本缓冲区。Renderer 的类型没有实现 `Send` 或 `Sync`。当前实现由串行队列协调供数、暂停、seek 和释放，停止时先取消定时器并排空队列，再释放回调状态。保留 7.3 的设备表可沿用这套线程与 ARC 所有权约束。[Renderer 绑定](https://docs.rs/objc2-av-foundation/0.3.2/objc2_av_foundation/struct.AVSampleBufferAudioRenderer.html) |

## APAC 的资源与接口

`mradm-ffi/src/apac/encoder.rs` 中的 `Writer` 在创建文件后即取得所有权。打开过程中配置失败或写帧提前返回时由 `Drop` 关闭文件，正常收尾由 `finish` 取回关闭状态并传播错误。AudioConverter 由文件持有，PCM 只在同步写入期间借给框架。客户端格式为 48 kHz 交错 float32，布局、码率、内容来源、DRC、同步频率及配置提交顺序沿用 C++ 实现。[AudioToolbox 绑定](https://docs.rs/objc2-audio-toolbox/0.3.2/objc2_audio_toolbox/)

Rust 新编码器有 11 个 `unsafe` 块。按包含起止行计算共 57 行，相当于删除的 142 行 C++ 实现与接口的 40%。原 Rust 函数表适配也随之删除。文件句柄、属性指针和音频缓冲区均局限在 worker 内，设备回调沿用已有实现。岛接口由 1,220 行降到 1,078 行，公开 C ABI 在两个平台均实测导出 164 个函数。

APAC 子进程在独立线程上创建、使用并关闭编码器，将 AudioToolbox 的调用栈与 CLI 主线程的启动栈隔离。线程创建失败按 I/O 错误报告。父进程按子进程的心跳和退出状态处理进度、取消和超时。

三个直接依赖 `objc2-audio-toolbox`、`objc2-core-audio-types` 和 `objc2-core-foundation` 均锁定 0.3.2，只为 macOS 启用所需模块。绑定随 crate 发布，构建使用现有 Rust 工具链和 Apple SDK。macOS 独立 Cargo Release 构建及 CMake 测试构建均通过。锁文件新增的 7 项已登记到许可证清单与 SBOM，其中可选依赖 `dispatch2` 只出现在锁文件。Windows 产品依赖树已核对。

## 原生行为验证

- macOS arm64 的 7 项定向 Release CTest 通过，覆盖 APAC 编解码、Apple 岛表、AUSpatialMixer、离线裁剪、LFE 路由、C ABI 和 CLI。原有断言验证了 7.1 侧后声道次序、HOA3、CAF、MP4 标签、进度和模拟收尾停滞。Rust 的 66 项 FFI 库测试与 19 项实时分配测试通过。新增错误路径测试直接触发 ExtAudioFile 创建失败，核对 I/O 错误码与底层 OSStatus。
- macOS 的系统空间监听使用 12 秒静音合成 ADM，经公开 C ABI 打开 AUSpatialMixer 的 7.1.4 文件流式渲染、播放、暂停、seek 到第 3 秒并继续。暂停的两次状态读取均为第 48,128 帧，继续后读到第 192,128 帧，各次欠载计数均为零。AirPods 转头后的声场定位仍需佩戴者听音确认。
- Windows x64 的规范 MSVC/Ninja Release 构建通过，APAC、C ABI 和 CLI 三项 CTest 通过。worker 在该平台返回退出码 1 与 `E 2 APAC encoding requires macOS (AudioToolbox)`，134 条 CLI 对照全部一致。
- 工作区 rustfmt、Clippy、cbindgen 头文件核对、C/C++ 规模棘轮与完整许可证检查通过。

### macOS 的 APAC 载荷复测

最初两轮 134 条 CLI 对照各有一处差异，均为 `render-apac` 的 7.1.4 M4A。首个差异在偏移 830 的包长字段，两个载荷分别对应 154,477 和 154,478 字节的文件。两版各运行 32 次时都生成过这两种载荷。它们也出现在 7.2 的 APAC 复测中。目标目录缺失、声道数不匹配和布局不支持的退出码与诊断逐字节一致。[7.2 APAC 复测](RUST_ISLAND_ENCODERS.md#release-验收) · [初次验收数据](evidence/rust-apple-island/validation.json)

跟进复测固定同一份 48 kHz 交错 float32 暂存 WAVE、可执行文件位置和输出路径，在每种启动条件内交替执行 C++ `d3c7ae68`、Rust `226cfbae` 和独立线程版本。调整环境长度时，前两版对同一输入都会切换载荷。独立线程版本在 1 秒样本的 13 种环境下 26 次均得到同一载荷，在 12 秒样本的 7 种环境及另一组路径长度下 60 次也各得到同一载荷。两个样本的载荷都能在 C++ 参考中逐字节复现。[逐对复测数据](evidence/rust-apple-island/review-follow-up.json)

在部分启动条件中，C++ 参考会输出另一种载荷。复跑 134 条 CLI 命令，唯一差异仍是 `render-apac` 的 7.1.4 M4A。两种 1 秒载荷解码后均为 48,000 帧、12 声道、48 kHz，float32 样本的最大绝对差为 0.0000561。12 秒样本各载荷均解码为 576,000 帧、12 声道、48 kHz，独立线程版本与 C++ 参考的最大绝对差为 0.000258，最大 RMS 差为 0.00000211。验收依据是实测载荷均出现在 C++ 参考、解码规格一致及上述样本差异。CLI 比较器保留同条件下的完整载荷差异。

## 离线耗时与内存

最初直接编码的耗时中位数增加 6.15%。后续 24 轮的总体中位数相差 1.16%，其中继承环境且载荷相同的四轮仍有回退。worker 由 410.9 ms 增至 433.3 ms，收尾由 227.7 ms 增至 250.4 ms。[初次验收数据](evidence/rust-apple-island/validation.json)

在接电的 M4 Pro 上，用 12 秒 `objects-long` 合成样本复测 7.1.4 APAC。三版各预热一次，轮换六种执行顺序。在复现收尾回退的启动条件下，三版 12 轮均输出相同的 `mdat`。收尾从 worker 发出 `F` 计到成功的终态进度。下表为中位数。

| Release 测量 | 每版轮数 | C++ 参考 | Rust 修复前 | Rust 独立线程 |
| --- | ---: | ---: | ---: | ---: |
| 同一暂存 WAVE 直接编码 | 12 | 444.5 ms | 470.2 ms | 442.4 ms |
| 同条件直接编码的收尾 | 12 | 244.7 ms | 270.6 ms | 244.0 ms |
| 完整离线渲染至 M4A | 12 | 660.3 ms | 666.1 ms | 659.5 ms |

同载荷的直接编码中，修复前总耗时比 C++ 参考增加 5.8%，收尾增加 10.6%。独立线程版本两项中位数分别比参考低 0.5% 和 0.3%。在另四种载荷相同的环境长度下，独立线程版本的总耗时中位数相对参考介于 −1.0% 到 +0.4%。

复现收尾回退的 12 轮中，三版峰值 RSS 中位数分别为 62.0、63.5 和 64.2 MiB。另一组继承环境的直接编码出现参考 61.2 MiB、独立线程 81.8 MiB，追加 24 轮后分别为 63.8 和 62.4 MiB，单次最高值分别为 85.0 和 85.4 MiB。完整离线渲染中，C++ 参考与独立线程版本的 RSS 中位数分别为 82.9 和 82.8 MiB。[各启动条件的逐轮数据](evidence/rust-apple-island/review-follow-up.json)
