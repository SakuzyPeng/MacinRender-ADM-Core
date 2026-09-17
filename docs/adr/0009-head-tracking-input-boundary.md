# ADR 0009：外置头部追踪的独立仓库与 OSC 接入边界

> 状态：已接受（架构方向；设备接入尚未实现，性能尚未实测）。
> 日期：2026-09-17。
> 适用范围：独立 MacinTrack 项目、MacinRender GUI 头部追踪来源、OSC 协议及 MacinTrack C ABI。

## 背景

个人 SOFA 双耳监听需要实时头部朝向，使世界固定声源在听者转头时保持空间位置。
首款计划设备为维特 BWT901BLECL5.0，它通过 BLE 输出姿态；其发现、校准、寄存器配置、
安装方向与断线恢复具有设备特性，与 ADM 导入、HRTF 和音频渲染属于不同职责。

MacinRender 已有 `IHeadTrackingSource`、手动与 AirPods 来源、四元数回正和平滑管线，以及
既有 listener orientation C ABI。维特和 OSC 来源尚不存在，应复用这些入口，而不将设备协议扩展到渲染核心。

同时，设备采集应能用于其他程序，并提供 Rust CLI 和可嵌入的 C ABI。
实现不能依赖先运行 MacinRender，也不能在 CLI、ABI 或两个仓库中分别维护姿态解析逻辑。

## 决策

### 独立设备仓库

独立项目暂名 **MacinTrack**，采用 Rust Cargo workspace，分为核心库、CLI、C ABI 三层。
首批目标为 macOS Apple Silicon、Windows x64；选用 `btleplug`、Tokio、`rosc`、`clap` 和 `cbindgen`。
这些是新项目的计划依赖，本次不向 MacinRender 引入 Rust 包、BLE 库或构建步骤。

核心库负责设备发现、连接、校准、参数设置、原始帧解析、安装转换、最新姿态快照与诊断。
CLI 和 C ABI 共用核心库；首版提供命令行操作，桌面 GUI 后续按需要增加。

### MacinRender 首版通过 OSC 集成

MacinTrack CLI 通过本机 OSC/UDP 发送统一姿态，MacinRender GUI 新增通用 OSC 来源，
接入既有 `IHeadTrackingSource`。默认地址为 `127.0.0.1:9000`，首版仅覆盖本机单一来源。

协议采用版本化消息 `/macintrack/v1/quaternion` 与 `/macintrack/v1/euler`。
四元数参数为 x/y/z/w，角度参数为 yaw/pitch/roll（度）；消息语义、坐标、测试向量和超时规则以
[跨仓库设计文档](../architecture/HEAD_TRACKING_OSC.md)为准。

设备校准和安装转换由 MacinTrack 负责；听音正前方回正、用户侧平滑、SOFA 与音频渲染由 MacinRender 负责。
OSC 与 AirPods 互斥，手动模式保留现有优先级。500 ms 无有效数据时标记失联、冻结最后呈现朝向并停止活动保活。

### C ABI 提供独立嵌入能力

MacinTrack 的 C ABI 用于宿主进程内采集，首版以轮询快照提供姿态与状态，内部拥有异步运行时。
使用不透明句柄、固定宽度类型、调用方缓冲区和明确错误码；Rust panic 不得跨越 C 边界。
生命周期、线程与所有权契约遵循设计文档。

MacinTrack ABI 初期为 experimental，使用独立符号前缀与版本查询；其版本与 OSC v1、ADM ABI 分开管理。
当前稳定 `adm_*` ABI 的承诺继续由 [ADR 0007](0007-c-abi-stability-policy.md)约束，
本决定通过既有 listener orientation 接口工作，不扩展其蓝牙或网络职责。

### 文档与协议维护

当前仓库保存完整设计及初始协议快照。建立独立仓库后，MacinTrack 承接设备和协议维护，
MacinRender 引用明确协议版本并共享契约测试向量；破坏消息结构或坐标含义时使用新协议版本。
发布说明分别记录接口实现、平台真机验收和性能测量，不能用“架构已接受”表示这些工作已经完成。

## 备选方案与取舍

| 方案 | 取舍 |
|---|---|
| 全部放入当前仓库 | 首次联调集中，但设备权限、依赖和发布与音频产品绑定，其他软件复用不便 |
| MacinRender 首版直接加载 MacinTrack C ABI | 少一个进程，但同时引入原生库打包、蓝牙权限和 ABI 绑定；保留为未来集成选择 |
| 独立 CLI 经 OSC 接入，同时提供 C ABI | 采用；设备与音频可独立调试和发布，代价是运行桥接进程并维护协议契约 |

本次拆仓面向独立设备采集产品，不改变 [ADR 0008](0008-rust-entry-and-saf-replacement.md)
针对本仓库内部音频算法 Rust 化的路线。

## 验收与后续

先以模拟姿态完成 OSC 闭环和坐标验证，再验证 BLE 与 C ABI，最后进行两平台真机和 Release 音频测量。
必须覆盖回正、组合旋转、±180° 连续性、非法数据、无首帧、失联保活、恢复及来源切换。

厂商标称的 200 Hz、0.2° 和续航不作为验收结果。尤其需要核实四元数实际更新方式、航向磁干扰，
以及 BLE、GUI 平滑与音频缓冲共同形成的整体延迟。完整实施阶段见
[头部追踪与 OSC 跨仓库设计](../architecture/HEAD_TRACKING_OSC.md)。
