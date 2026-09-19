# 原生 OSC 头部姿态接收接口

> 状态：原生 C++／C ABI 已实现；GUI 适配、设备安装验收和音频整体延迟留待后续。
> C ABI：v1.40，additive，既有接口与结构布局不变。
> 日期：2026-09-19。

## 边界

PoseBridge 负责 BLE／USB 设备、校准和安装轴转换。本接口接收已经转换为统一坐标语义的姿态。
接收器是独立对象，不需要 ADM 文件、音频设备或 GUI；后台线程只维护最新快照，不调用 Monitor／Scene 控制函数。

宿主在自己的控制线程轮询姿态，决定来源仲裁、回正和平滑，随后调用已有的
`adm_monitor_set_listener_orientation` 或 `adm_scene_stream_set_listener_orientation`。
它们既有的线程与生命周期要求继续适用。GUI 今后可以用该接口实现 `IHeadTrackingSource`，本轮没有修改 GUI 源码或绑定。

## 协议与坐标

- IPv4 回环 UDP，绑定 `127.0.0.1`，默认端口 9000；配置为 0 时由系统分配临时端口，可从状态读取。
- 每个数据报只接受一份完整 OSC 普通消息，不接收 bundle、timetag、回正或其他控制命令。
- `/posebridge/v1/quaternion`：`,ffff`，x、y、z、w。
- `/posebridge/v1/euler`：`,fff`，yaw、pitch、roll，单位度。
- 严格校验地址、类型、字符串零填充、完整长度、有限数值；多余尾部、截断、未知地址与非法四元数均拒绝。
- 四元数范数以 double 计算，小于 `1e-6` 拒绝，其余归一化。接收器不进行回正、额外平滑或预测。

四元数采用 Hamilton x/y/z/w，`q = q_y(yaw) * q_x(pitch) * q_z(roll)`，与 PoseBridge 和现有 GUI 相同。
输出角度为 yaw 向左、pitch 向上、roll 向右倾斜。GUI 四元数分量不是渲染场景的 XYZ 轴；
向现有渲染入口提交应使用快照中的 yaw/pitch/roll。坐标向量和边界详见[跨仓库设计](HEAD_TRACKING_OSC.md#5-统一姿态与安装转换)。

## C ABI 生命周期

公开声明位于 [c_api.h](../../include/adm/c_api.h)。

```text
adm_create_osc_head_tracking(config 或 NULL, &receiver)
adm_osc_head_tracking_start(receiver)
  -> adm_osc_head_tracking_get_status(receiver, &status)
  -> adm_osc_head_tracking_get_pose(receiver, &pose)
adm_osc_head_tracking_stop(receiver)
adm_destroy_osc_head_tracking(receiver)
```

创建不绑定端口；start 同步绑定并返回端口占用等错误，不静默改用其他端口。
运行中重复 start 幂等；stop 幂等并等待接收线程结束、释放端口，保留最后姿态但使 `fresh=0`。
stop／失败后重新 start 会清空姿态与统计并分配新 session_id。直接销毁运行中对象也会停止并释放连接。

同一句柄的调用由宿主串行化，销毁不得与任何访问重叠；不同句柄可以在不同线程使用。
内部后台接收经过同步保护，无回调进入调用方。这些函数不属于实时音频回调接口。

配置、姿态与状态都使用固定宽度类型和 `struct_size`：

| 类型 | v1.40 大小 | 含义 |
|---|---:|---|
| `adm_osc_head_tracking_config_t` | 8 字节 | 大小、监听端口；NULL 配置选择 9000 |
| `adm_head_tracking_pose_t` | 80 字节 | 是否有数据、新鲜度、会话、序号、接收时间、归一化四元数与角度 |
| `adm_osc_head_tracking_status_t` | 64 字节 | 接收状态、实际端口、包计数、拒绝计数和恢复次数 |

调用输出函数前设置 `struct_size = sizeof(相应类型)`。容量不足返回 `ADM_ERROR_INVALID_ARGUMENT`，不部分写入。
调用方传入更大结构时只写本版已知字段，未知尾部保持不变。没有首帧和数据陈旧是正常快照，不作为错误码返回。

例：宿主已有一个 monitor，本段放在其控制线程轮询回调中（应用自己的回正／平滑时在提交前处理）：

```c
adm_osc_head_tracking_t* receiver = NULL;
adm_osc_head_tracking_config_t config = {0};
config.struct_size = sizeof(config);
config.listen_port = 9000;
if (adm_create_osc_head_tracking(&config, &receiver) != ADM_ERROR_OK) {
    /* report allocation/configuration error */
    return;
}
if (adm_osc_head_tracking_start(receiver) != ADM_ERROR_OK) {
    /* copy/show adm_osc_head_tracking_last_error_message(receiver) before destruction */
    adm_destroy_osc_head_tracking(receiver);
    return;
}

/* Repeat on the host control thread, including while waiting or stale. */
adm_head_tracking_pose_t pose = {0};
pose.struct_size = sizeof(pose);
if (adm_osc_head_tracking_get_pose(receiver, &pose) == ADM_ERROR_OK && pose.has_pose && pose.fresh) {
    adm_error_code_t rc = adm_monitor_set_listener_orientation(monitor, pose.yaw_deg, pose.pitch_deg, pose.roll_deg);
    /* handle rc using the monitor's existing error-reporting contract */
}

/* At shutdown, after all polling has stopped: */
adm_destroy_osc_head_tracking(receiver);
```

上述只是控制流程片段；宿主负责 monitor 的创建、寿命以及系统空间音频等已有渲染边界。
receiver 不持有 monitor，不会因宿主销毁／重建 monitor 而形成悬空指针。

## 状态与数据时效

| state | 数值 | 行为 |
|---|---:|---|
| IDLE | 0 | 已创建，未启动 |
| WAITING | 1 | 已绑定，无首份有效姿态；`has_pose=0` |
| ACTIVE | 2 | 最近有效姿态不足 500 ms；`fresh=1` |
| STALE | 3 | 至少 500 ms 无有效姿态；保留最后值，`fresh=0` |
| STOPPED | 4 | 已释放端口；保留最后值，`fresh=0` |
| FAILED | 5 | 绑定或接收发生错误；需要处理错误并显式重启 |

时间采用接收端单调时钟。`received_ns` 相对于本次 receiver start；`age_ms` 仅在 `has_pose=1` 时有意义。
相同数值的新有效包仍增加 sequence；重复轮询不改变 sequence／received_ns。慢消费者只得到最新值，没有历史姿态队列。
不合法的包只增加 `rejected_packets`，不能维持 ACTIVE 或把 STALE 恢复为 ACTIVE。

失联后首个有效包增加 `recovery_count`，会话保持不变。宿主可据此提示检查回正，接收器不自动清除宿主参考方向。
宿主须在 `fresh=0` 时停止音频姿态保活并冻结实际已呈现的朝向；不要继续向陈旧目标平滑。
等待／失联期间也应继续轮询接收器，否则无法观察首次接收或恢复。

OSC v1 不携带源序号或采样时间：这里的 session／sequence 属于接收器，不能识别发送端重启、网络重排或设备采样时刻。
只支持预先选定的本机来源；多个本机进程同时发送时按有效报文接收次序覆盖，不自动做来源仲裁。

错误消息经 `adm_osc_head_tracking_last_error_message` 取得，指针由 receiver 持有，直到下一次错误查询或销毁。
畸形包不逐包输出错误日志，避免消息洪泛；通过计数器诊断。公共 C ABI 保持 noexcept。

## C++ 与验证

[head_tracking.h](../../include/adm/head_tracking.h) 提供 `mradm::OscHeadTrackingReceiver` 与完整快照，
链接现有 `MacinRender::ADMRealtime`。公开头文件不暴露平台 socket 或第三方 BLE 类型。

```sh
cmake --build --preset debug --target mr_adm_osc_head_tracking_tests mr_adm_c_api_header_smoke mr_adm_c_api_tests
ctest --preset debug -R '(osc_head_tracking|c_api_header_smoke|^mr_adm_c_api_tests$)' --parallel "$(sysctl -n hw.ncpu)" --output-on-failure
cmake --build --preset release --target mradm_capi_bundle
python3 tests/manual/osc_posebridge_check.py --library build/release/libmradm_capi.dylib --posebridge /path/to/posebridge
```

单元测试覆盖七组姿态向量、组合姿态、q／−q、±180° 邻接、世界固定声源方向、畸形数据、非有限数值、溢出／零范数、
最新值覆盖、无首帧、非法包不能保活、失联恢复、端口占用与释放、重复启停、句柄销毁和结构尺寸。
集成脚本通过真实动态库 C ABI 接收 PoseBridge 模拟器的两种格式，验证跨越 ±180°、陈旧冻结、恢复和重启。

另有[最小 C++ 接收程序](../../tests/manual/osc_head_tracking_probe.cpp)，可独立检验平台 UDP 实现：
`mr_adm_osc_head_tracking_probe [port] [seconds]`。端口 0 会打印系统分配的端口。
本轮不以接口测试代替 GUI、物理安装方向、个人 SOFA 或音频整体延迟验收。
