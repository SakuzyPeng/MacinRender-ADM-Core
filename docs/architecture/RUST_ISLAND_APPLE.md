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

三个直接依赖 `objc2-audio-toolbox`、`objc2-core-audio-types` 和 `objc2-core-foundation` 均锁定 0.3.2，只为 macOS 启用所需模块。绑定随 crate 发布，构建使用现有 Rust 工具链和 Apple SDK。macOS 独立 Cargo Release 构建及 CMake 测试构建均通过。锁文件新增的 7 项已登记到许可证清单与 SBOM，其中可选依赖 `dispatch2` 只出现在锁文件。Windows 产品依赖树已核对。

## 原生行为验证

- macOS arm64 的 7 项定向 Release CTest 通过，覆盖 APAC 编解码、Apple 岛表、AUSpatialMixer、离线裁剪、LFE 路由、C ABI 和 CLI。原有断言验证了 7.1 侧后声道次序、HOA3、CAF、MP4 标签、进度和模拟收尾停滞。Rust 的 66 项 FFI 库测试与 19 项实时分配测试通过。新增错误路径测试直接触发 ExtAudioFile 创建失败，核对 I/O 错误码与底层 OSStatus。
- macOS 的系统空间监听使用 12 秒静音合成 ADM，经公开 C ABI 打开 AUSpatialMixer 的 7.1.4 文件流式渲染、播放、暂停、seek 到第 3 秒并继续。暂停的两次状态读取均为第 48,128 帧，继续后读到第 192,128 帧，各次欠载计数均为零。AirPods 转头后的声场定位仍需佩戴者听音确认。
- Windows x64 的规范 MSVC/Ninja Release 构建通过，APAC、C ABI 和 CLI 三项 CTest 通过。worker 在该平台返回退出码 1 与 `E 2 APAC encoding requires macOS (AudioToolbox)`，134 条 CLI 对照全部一致。
- 工作区 rustfmt、Clippy、cbindgen 头文件核对、C/C++ 规模棘轮与完整许可证检查通过。

### macOS 的 APAC 载荷复测

两轮独立的 134 条 CLI 对照各有一处差异，均为 `render-apac` 的 7.1.4 M4A。第一轮参考为 154,477 字节，候选为 154,478 字节，第二轮长度对调。两轮的首个差异都在偏移 830 的包长字段。按长度交叉比较两轮完整记录，两个长度各得到一份 134 条零差异结果。CAF 及其余命令在原配对中也一致。

固定 CLI、输入与输出的路径，直接运行两个版本的 worker，每版 32 次。两个版本收到的 48,000 帧 PCM 逐字节相同，心跳序列相同，两版的 `mdat` 均只有两种摘要，出现次数也都是 25 次和 7 次。这两个摘要也出现在 [7.2 的 APAC 复测](RUST_ISLAND_ENCODERS.md#release-验收)。目标目录缺失、声道数不匹配和布局不支持三种错误的退出码与诊断逐字节一致。CLI 比较器继续核对完整载荷。

## 离线耗时与内存

在接电的 M4 Pro 上使用 12 秒 `objects-long` 合成样本和 7.1.4 输出。两版使用相同的可执行文件位置，分别预热，每轮对调先后顺序。表列中位数，峰值 RSS 来自 macOS `wait4`。

| Release 测量 | 每版轮数 | C++ 参考耗时 | Rust 候选耗时 | 参考峰值 RSS | 候选峰值 RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| 完整离线渲染至 M4A | 12 | 625.1 ms | 594.1 ms | 58.3 MiB | 58.5 MiB |
| 直接编码同一暂存 WAVE | 12 | 413.0 ms | 438.4 ms | 58.3 MiB | 58.4 MiB |
| 直接编码并记录各阶段心跳 | 24 | 413.9 ms | 418.8 ms | 58.3 MiB | 58.5 MiB |

直接编码首轮的耗时增加 6.15%，超过 5% 的调查阈值。复测交替使用六种进程环境长度，两版在 12 秒样本上均产生同样的三种载荷。按相同载荷分别比较，三组块写入中位数的差都小于 0.4 ms，复测整体耗时中位数相差 1.16%。各阶段和峰值内存的逐轮结果保留在[验收数据](evidence/rust-apple-island/validation.json)。
