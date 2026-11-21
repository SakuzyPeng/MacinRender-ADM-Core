# 七期 7.5 Windows 系统空间输出与 MMCSS

Windows 系统空间监听已迁入 `mradm-ffi::windows_spatial`。文件流式渲染监听和 Scene 流式渲染都通过原有设备表调用它。房间 DSP 工作线程和 Scene 工作线程共用 `mmcss::AudioTask` 来注册和撤销 MMCSS 的 `Audio` 任务。C++ 对照版本为 `d5e3a323`。[7.1 评估标准](RUST_ISLAND_BASELINE.md) · [布局与恢复机制](ADM_WINDOWS_SYSTEM_SPATIAL.md)

## 原型与取舍

在 Windows x64 默认输出端点上，Rust 原型用 MMDevice 取得 `ISpatialAudioClient`，以 48 kHz 单声道 float32 激活 7.1.4 的 12 个静态对象。32 次更新共提交 15,360 帧。该端点报告每周期最多 480 帧、最多 128 个动态对象。原型还在同一线程成功注册、撤销了 MMCSS。

| 7.1 评估项 | 7.5 结果 |
|---|---|
| API 覆盖 | `windows` 提供端点枚举、流激活、静态与动态对象、事件等待和 AVRT 接口。9.1.6 申请 4 个动态对象，22.2 申请 11 个。动态对象在每个周期按所选扬声器几何重设坐标。[微软 Rust 绑定](https://microsoft.github.io/windows-docs-rs/doc/windows/Win32/Media/Audio/struct.ISpatialAudioClient.html) |
| 实时约束 | `Stream::open` 按设备声明的最大帧数预分配交织缓冲和对象槽。音频线程独占这两块存储，控制线程通过事件停止播放。五种布局各测 32 个周期，从首次对象激活起共 76,800 帧，Rust 分配器均计为零。计数范围是调用线程上的 Rust 分配。[GetMaxFrameCount](https://learn.microsoft.com/en-us/windows/win32/api/spatialaudioclient/nf-spatialaudioclient-ispatialaudioclient-getmaxframecount) |
| unsafe 与规模 | 删除 Windows 岛的 708 行 C++。新 sink 和 MMCSS 模块的生产代码有 31 个 `unsafe` 块，按含起止行的并集计 46 行，相当于 708 行的 6.5%。另有一个 `unsafe impl Send` 将引擎的 pull 与上下文移交音频线程，停止设备会先等最后一次回调退出。 |
| 构建 | Windows 专用依赖 `windows = "=0.62.2"` 使用预生成绑定。Windows 产品树引入的 11 个 crate 已登记许可证和 SBOM。macOS 产品依赖树不含这些 crate。两个平台均沿用仓库的 Rust 与平台 SDK。 |

流、对象与 COM apartment 在同一线程创建和释放。激活参数的 `VT_BLOB` 同步借用栈上的内容，`ManuallyDrop` 保证绑定的析构函数不会释放这块借用内存。每次成功开始更新都对应一次结束更新，取得的对象缓冲只在这一周期写入。MMCSS 句柄在注册线程释放。[对象缓冲的有效期](https://learn.microsoft.com/en-us/windows/win32/api/spatialaudioclient/nf-spatialaudioclient-ispatialaudioobjectrenderstreambase-endupdatingaudioobjects) · [AVRT 撤销约束](https://learn.microsoft.com/en-us/windows/win32/api/avrt/nf-avrt-avrevertmmthreadcharacteristics)

## 原生行为验证

- **Windows 文件流式渲染监听。** 用同一份 12 秒静音 ADM、EAR 和系统默认端点，对 C++ 参考与 Rust 实现分别执行播放、暂停、两次读取暂停位置、seek 到第 3 秒和继续播放。两版的 5.1、5.1.4、7.1.4、9.1.6、22.2 均完成，暂停期间播放头固定，继续后超过第 3 秒。公开 C ABI 返回的五项布局顺序、ID、显示名及声道数均已核对。
- **Windows Scene 流式渲染。** `MR_ADM_TEST_SYSTEM_SPATIAL=1` 下公开 C 消费者测试通过。7.1.4 系统空间输出消费和呈现的帧数在 EOS 都恰为 33，切换 epoch 后进度归零。
- **错误与恢复。** 原生测试覆盖错误布局、声道数不符、端点拒绝对象格式时的 `unsupported` 和 HRESULT、失败后重启、重复启动和停止回调。真机故意缩短预分配缓冲，确认本次更新报错后下一个周期可以正常拉取，短读余量全部补零。另用故障注入验证旧流先释放、重开失败后退避、重试成功后消除 `recovering`。
- **数值和线程。** 回归检查覆盖标准与 Apple 几何、5.1 与 7.1.4 的环绕顺序、22.2 动态位置和共享 LFE。LFE 按实际写出的帧判定两路活动，只有两路均高于阈值时才应用功率补偿。MMCSS 实机测试验证了房间 DSP 线程退出时撤销注册，以及 Scene 工作线程随房间启停撤销并重新注册。Windows 的 19 项 DSP 和 3 项 Scene 实时分配测试均通过。
- **macOS arm64。** 系统空间输出私有工厂移除了只有 Windows 用过的几何参数。Debug 的 Apple 岛、Apple 冒烟、LFE、C ABI、C 头与 CLI 六项 CTest 通过。Release 的真实 Scene 输出 C 消费者测试、7.1.4 文件流式监听的播放、暂停、seek 与继续均通过。两平台共享库都实测导出 164 个公开函数。

### seek 前后的欠载

五布局首测中，C++ 参考的 5.1 在 seek 后记了 1 次欠载。Rust 的首轮均为零，第二轮的 22.2 在 seek 后记了 1 次。随后用同一端点、输入和 Release 探针，在两种布局各做 8 轮 C++ / Rust 对照，轮换运行顺序。每次都完成暂停与 seek。下表为各组累计欠载。

| 实现 | 5.1，8 次会话 | 22.2，8 次会话 |
|---|---:|---:|
| C++ `d5e3a323` | 1，第 3 轮播放阶段 | 1，第 1 轮 seek 后 |
| Rust 7.5 | 0 | 0 |

佩戴耳机的逐声道方位、22.2 双 LFE 听感，以及在 Windows 声音设置中关闭或切换空间格式时的实际表现，仍待维护者确认。
