# ADR 0009：外置头部追踪的独立仓库与 OSC 接入边界

> 状态：已接受（PoseBridge 与 MacinRender 原生 OSC 接口已实现；GUI 适配、各平台实机与音频验收分别记录）。
> 日期：2026-09-17；更新：2026-09-19。
> 适用范围：独立 PoseBridge 项目、MacinRender 原生 OSC 接收与后续 GUI 来源、OSC 协议及双方 C ABI。

## 背景

个人 SOFA 双耳监听需要实时头部朝向，使世界固定声源在听者转头时保持空间位置。
首款设备为维特 BWT901BLECL5.0，它通过 BLE 和 USB 串口输出姿态；其发现、校准、寄存器配置、
安装方向与断线恢复具有设备特性，与 ADM 导入、HRTF 和音频渲染属于不同职责。

MacinRender 已有 `IHeadTrackingSource`、手动与 AirPods 来源、四元数回正和平滑管线，以及
既有 listener orientation C ABI。当前 GUI 的 OSC 来源尚不存在，应复用这些入口；维特输入已在独立 PoseBridge 初版中实现。

同时，设备采集应能用于其他程序，并提供 Rust CLI 和可嵌入的 C ABI。
实现不能依赖先运行 MacinRender，也不能在 CLI、ABI 或两个仓库中分别维护姿态解析逻辑。

## 决策

### 独立设备仓库

独立项目名称为 **PoseBridge**，采用 Rust Cargo workspace，分为核心库、CLI、C ABI 三层。
首批目标为 macOS Apple Silicon、Windows x64；选用 `btleplug`、`tokio-serial`、Tokio、`rosc`、`clap` 和 `cbindgen`。
这些依赖位于独立项目，不向 MacinRender 引入 Rust 包、BLE 库或构建步骤。

核心库负责设备发现、连接、校准、参数设置、原始帧解析、安装转换、最新姿态快照与诊断。
CLI 和 C ABI 共用核心库；首版提供命令行操作，桌面 GUI 后续按需要增加。

### MacinRender 首版通过 OSC 集成

PoseBridge CLI 通过本机 OSC/UDP 发送统一姿态。按本轮先完成接口、暂不适配 GUI 的范围，
MacinRender 新增独立原生接收器和 C ABI 轮询接口；后台只维护最新姿态，不直接调用播放器控制。
GUI 后续通过该接口适配既有 `IHeadTrackingSource`。默认地址为 `127.0.0.1:9000`，首版仅覆盖本机单一来源。

协议采用版本化消息 `/posebridge/v1/quaternion` 与 `/posebridge/v1/euler`。
四元数参数为 x/y/z/w，角度参数为 yaw/pitch/roll（度）；消息语义、坐标、测试向量和超时规则以
[跨仓库设计文档](../architecture/HEAD_TRACKING_OSC.md)为准。

设备校准和安装转换由 PoseBridge 负责；听音正前方回正、用户侧平滑、SOFA 与音频渲染由 MacinRender 负责。
OSC 与 AirPods 互斥，手动模式保留现有优先级。500 ms 无有效数据时标记失联、冻结最后呈现朝向并停止活动保活。

### C ABI 提供独立嵌入能力

PoseBridge 的 C ABI 用于宿主进程内采集，首版以轮询快照提供姿态与状态，内部拥有异步运行时。
使用不透明句柄、固定宽度类型、调用方缓冲区和明确错误码；Rust panic 不得跨越 C 边界。
生命周期、线程与所有权契约遵循设计文档。

PoseBridge ABI 初期为 experimental，使用独立符号前缀与版本查询；其版本与 OSC v1、ADM ABI 分开管理。
当前稳定 `adm_*` ABI 的承诺继续由 [ADR 0007](0007-c-abi-stability-policy.md)约束，
姿态仍经既有 listener orientation 接口进入渲染；v1.40 的网络接收使用独立句柄，不把网络线程或设备管理绑定到这些 setter。
MacinRender 接收器的生命周期、状态和调用范例见[原生 OSC 接收接口](../architecture/OSC_HEAD_TRACKING_API.md)。

### 文档与协议维护

当前仓库保存跨仓库设计及协议快照。PoseBridge 独立仓库已建立，承接 BLE / USB 设备实现和协议维护，
MacinRender 引用明确协议版本并共享契约测试向量；破坏消息结构或坐标含义时使用新协议版本。
发布说明分别记录接口实现、平台真机验收和性能测量，不能用“架构已接受”表示这些工作已经完成。

## 备选方案与取舍

| 方案 | 取舍 |
|---|---|
| 全部放入当前仓库 | 首次联调集中，但设备权限、依赖和发布与音频产品绑定，其他软件复用不便 |
| MacinRender 首版直接加载 PoseBridge C ABI | 少一个进程，但同时引入原生库打包、蓝牙权限和 ABI 绑定；保留为未来集成选择 |
| 独立 CLI 经 OSC 接入，同时提供 C ABI | 采用；设备与音频可独立调试和发布，代价是运行桥接进程并维护协议契约 |

本次拆仓面向独立设备采集产品，不改变 [ADR 0008](0008-rust-entry-and-saf-replacement.md)
针对本仓库内部音频算法 Rust 化的路线。

## 验收与后续

先以模拟姿态完成 OSC 闭环和坐标验证，再验证 BLE 与 C ABI，最后进行两平台真机和 Release 音频测量。
必须覆盖回正、组合旋转、±180° 连续性、非法数据、无首帧、失联保活、恢复及来源切换。

厂商标称的 200 Hz、0.2° 和续航不作为验收结果。尤其需要核实四元数实际更新方式、航向磁干扰，
以及 BLE、GUI 平滑与音频缓冲共同形成的整体延迟。完整实施阶段见
[头部追踪与 OSC 跨仓库设计](../architecture/HEAD_TRACKING_OSC.md)。
