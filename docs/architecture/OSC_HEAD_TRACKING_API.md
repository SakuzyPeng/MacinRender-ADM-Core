# 原生 PoseBridge 接收接口

状态：原生 C++／C ABI 已实现；GUI、时钟同步、预测、延迟补偿与音频整体延迟验收后置。
当前 PoseBridge 0.3／协议 3，MacinRender C ABI 1.42。

经维护者确认，此前头追接口没有实际消费者且未作为正式接口交付，本轮统一替换旧草案。
旧 OSC 地址、get_pose_v2 和旧头追结构布局已删除；已有音频 C ABI 与 SONAME 保持不变，见 [ADR 0007](../adr/0007-c-abi-stability-policy.md)。

## 边界

接收器只在 IPv4 回环 UDP 收取 PoseBridge 数据，默认 9000，端口 0 请求系统分配。它不连接传感器、不发送设备控制、不调用 GUI／Monitor／Scene。
宿主在控制线程读取完整快照，处理来源、回正、平滑和失联，再调用既有 listener-orientation setter。
收到合法姿态与设备校准准确度是不同事实；未知设备状态不伪装成已校准。

公开入口见 [head_tracking.h](../../include/adm/head_tracking.h) 与 [c_api.h](../../include/adm/c_api.h)。

## 当前协议

每个数据报一个 OSC 普通消息，最多 8192 字节。拒绝 bundle、尾部数据、错误零填充、未知地址、非法整数或非有限姿态。
四元数用 double 计算范数，小于 1e-6 拒绝，其余归一化。

| 地址 | 类型 |
|---|---|
| `/posebridge/quaternion` | `,ishhhhhhhhihhffff` |
| `/posebridge/euler` | `,ishhhhhhhhihhfff` |
| `/posebridge/info` | `,s`，UTF-8 JSON |
| `/posebridge/status` | `,s`，UTF-8 JSON |

姿态前 13 个参数依次为：protocol_version（i，必须为 3）、source_id（s）、instance_id、session_id、
sequence、tx_sequence、reference_epoch、metadata_revision、received_ns、age_at_send_ns（上述整数为 h）、
sample_time_kind（i）、sample_time_ms（h）、sample_clock_epoch（h），随后为完整姿态。
source_id 为非空白、无控制字符的 UTF-8，最长 256 字节。标识、序号、参考和描述版本为正 int64；时间为非负值。
kind=0 缺失，time/epoch 必须同为 0；kind=1 设备日历（自 2000-01-01，**不是 UTC**）；kind=2 模拟经过时间。
有时间时 epoch>0。age_at_send_ns 表示源主机停留时间，须小于 500 ms，不含 USB/BLE 传输或设备融合延迟。

JSON 共同字段：schema=3、kind、source_id、instance_id、session_id、reference_epoch、metadata_revision、message_seq。
所有 64 位标识、计数和时间为十进制字符串。info/status 消息序号独立，嵌套 descriptor/status 的关联字段须与消息头一致。
info 最迟每 5 秒重复；status 每秒且状态变化时发送。设备观察包含时间和有效性标记，采集期间不后台轮询配置。

输出坐标为 Hamilton XYZW，`q=q_y(yaw) q_x(pitch) q_z(roll)`，yaw 左、pitch 上、roll 右倾。
四元数轴不是渲染场景轴；提交既有 listener setter 使用输出 Euler，安装与数学边界见[跨仓库设计](HEAD_TRACKING_OSC.md)。

## 来源与时效

配置可指定 source_id；未指定时锁定首份有效姿态的逻辑来源。元数据不能抢占该绑定，其他来源计入 ignored_sources。
重新 start 清除绑定。相同逻辑来源的新实例可接替旧实例，保留最近 16 个退出实例以拒绝迟到包。
该机制不是身份认证；每个逻辑来源应只有一个发送者。

同一实例内 tx_sequence 递增，参考／描述版本不倒退；同一采集会话内采样序号递增、源接收时间不倒退。
换采集会话须推进参考和描述版本。同一采样时钟代次内时间递增；缺失时间不会清空此前时钟检查状态。
旧会话、旧实例、重复／乱序消息不能刷新有效数据。

- 姿态 fresh 依据本地接收间隔加源主机停留时间，阈值 500 ms。没有新姿态时冻结最后值，宿主停止音频保活。
- 心跳单独采用 3 秒阈值，不能产生姿态或更新姿态接收时间。heartbeat_alive 表示近期联系，仍须检查源状态是否 stopped/failed。
- 已知源停止、参考变化会使已有姿态失效；较旧的 stopped 状态不能覆盖后来的新采样。
- source/session/revision 匹配的元数据通过 info_matches_pose、status_matches_pose 标记；元数据未到不阻塞合法姿态。
- 接收器统计姿态包、元数据包、拒绝、忽略来源、版本不匹配、tx 缺口。源采样跳号不等于 UDP 丢包。
- 设备时间、PoseBridge 接收时间、Render 接收时间独立，不直接相减计算总延迟。

## API 与示例

```text
adm_create_osc_head_tracking(config 或 NULL, &receiver)
adm_osc_head_tracking_start(receiver)
  → adm_osc_head_tracking_get_pose(receiver, &pose)
  → adm_osc_head_tracking_get_status(receiver, &status)
  → adm_osc_head_tracking_snapshot_json(receiver, &json)
  → adm_free_string(json)
adm_osc_head_tracking_stop(receiver)
adm_destroy_osc_head_tracking(receiver)
```

当前 64 位平台上 config／pose／status 大小分别为 16／160／120 字节。调用前设置 struct_size；容量不足不部分写入，较大结构的未知尾部保留。
config.source_id 为可选 UTF-8 字符串，create 时复制。JSON 原子复制姿态、来源和状态；成功输出使用 adm_free_string 释放，失败置 NULL。
旧草案布局不可混用，调用方应检查 ABI 1.42 及协议 3。no-data／stale 是快照状态，不是错误码。

同一句柄的宿主调用串行化，销毁不与任何访问重叠；stop/destroy 等待后台线程退出并释放端口。
这些接口不用于音频回调；绑定端口失败会报告错误，不静默改用其他端口。

- [C 控制线程示例](../../examples/head_tracking_consumer.c)：来源过滤、新采样、参考变化、失联停保活、快照释放。
- [C++ 无 GUI 接收程序](../../tests/manual/osc_head_tracking_probe.cpp)：独占绑定、最新快照、停止释放；`[port] [seconds] [source_id]`。
- [真实 CLI 与动态库联调](../../tests/manual/osc_posebridge_check.py)。

```sh
cmake --build --preset debug --target mr_adm_osc_head_tracking_tests mr_adm_c_api_header_smoke mr_adm_c_api_tests
ctest --preset debug -R '(osc_head_tracking|c_api_header_smoke|^mr_adm_c_api_tests$)' --parallel "$(sysctl -n hw.ncpu)" --output-on-failure
cmake --build --preset release --target mradm_capi_bundle mr_adm_osc_head_tracking_c_example
python3 tests/manual/osc_posebridge_check.py --library build/release/libmradm_capi.dylib --posebridge /path/to/posebridge
```

验证记录在交付时按平台补充。Windows 缺完整渲染依赖时，只报告头追源文件、C ABI 翻译单元和原生接收器检查，
不将其记为完整 Windows Render DLL 运行验证。硬件安装、校准效果、GUI 与音频延迟始终单列。
