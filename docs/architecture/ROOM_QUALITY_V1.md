# 房间精确／实时档位第一版验证

日期：2026-10-13。实现基于 `009e8ad`，策略见 [ADR 0020](../adr/0020-room-quality-tiers.md)。
完整数字、源码摘要、资源哈希与验证命令结果见 [JSON 记录](ROOM_QUALITY_V1.json)。

## 已实现

默认 `exact` 保留既有标量 FFT 与逐源控制更新。`realtime` 显式启用原生 FFT，以及按声源 ID
错峰的 1024 帧反射更新周期。直达路径继续按原有更新点处理；头姿、跳变、离散语义和字段清除
立即刷新。镜像阶数、路径、HRIR、材质、EQ、晚场与 FIR 支持保持原模型。

两档覆盖离线渲染、文件流式渲染和 Scene 流式渲染，可在同一进程并存；档位固定到会话。
新增 CLI、C++ 与 C ABI v1.48 入口，旧入口仍默认精确。公开 C POD 布局不变。
直达／反射缓存分开，频谱与卷积历史按 FFT 策略隔离；seek 和换房准备包含反射更新历史。

```sh
build/release/mradm room-check room.json --quality realtime
build/release/mradm render -i programme.wav -o binaural.wav --renderer room-binaural \
  --room room.json --room-quality realtime
```

第一版尚未引入共用世界坐标早反射声场；头姿变化仍会重建各声源反射。这解释了下表中
持续运动收益明显高于纯头姿场景的原因。

## Release 性能

固定私人参考房间与 HRIR：`reference-7x6x3.2-corrected-v1`、SADIE II D2，48 kHz、256 taps。
Rust 1.98.0，默认 worker 数，关闭 profile／trace；复用已有构建缓存。
计时为 `Session::process` 整次调用，包含 worker 调度，不含解码、资源准备或设备链路。
每次 512 帧，期限 10.667 ms；每轮先预热 32 次，再测 938 次。
32 源测三轮，第二轮交换档位顺序；1／16 源各测一轮。均值与 p99 取三轮中位数，
超时数累计三轮共 2814 次。各主机测量前完成其构建及正确性测试。

密集运动场景：32 源在各自不同样本偏移提交 512 帧位置斜坡，头姿以 93.75 Hz 更新。

| 主机 | 精确均值 ms | 实时均值 ms | 耗时下降 | 实时 p99 ms | 超时：精确 → 实时 / 2814 |
|---|---:|---:|---:|---:|---:|
| macOS / M4 Pro | 5.654 | 3.165 | 44.0% | 4.163 | 13 → 6 |
| Windows / i7-11800H | 15.706 | 8.879 | 43.5% | 11.428 | 2814 → 126 |
| Linux / Xeon Platinum 8352V | 14.124 | 8.880 | 37.1% | 20.375 | 1881 → 544 |

Linux 为共享云主机，进程 affinity／CPU quota 均为 32，不能视为独占桌面机。
Windows 与 Linux 的实时档位平均耗时已低于期限，但 p99 仍超限；Mac 也有少量离群超时。
本轮三台机器均不满足该密集运动场景的“零超时”验收。

另外三个 32 源场景的平均耗时下降：

| 主机 | 静态 | 持续头姿 | 持续头姿＋一次换房 |
|---|---:|---:|---:|
| macOS | 7.1% | 7.0% | 10.2% |
| Windows | 21.2% | 10.0% | 1.3% |
| Linux | 5.5% | 8.8% | 8.4% |

这些是同轮两档对比，不能与前置算术实验的单独运动场景时间直接混用。单次换房在一轮
中的占比有限，且 Windows 差值很小；不将 1.3% 视为稳定的换房加速保证。

## 数值与时间线

三平台精确档位的**完整回放 manifest** 与原基线一致，包括准备数据、8 个 HRIR 查询、
8 个静态 BRIR 和最终 PCM。普通、屏幕及密集运动屏幕回放的 PCM SHA-256 分别为：

- 普通：`b3a3ec3747db934c995289036c7d65eb0224b0726ffc7b8f74aad62a1ffe453d`
- 屏幕：`1f0dfebf5f8a603ce6c06ecfd9ffe76aa999f7c0792a0ad8c6646ca699883910`
- 密集运动＋屏幕：`e21985a58dc875ac944f1c13533ce409b418cd0c256d858a135b3928fb18ca83`

两档的三类回放都通过同机固定／碎片分块逐位比较。每份输出 163398 帧，包含控制、头姿、
换房、源移除及 EOS 尾声。精确和实时档位的 FIFO 同为 256 帧，参考包尾声同为 67352 帧；
有限 seek 预热由 69119 增至 70143 帧，增加的 1024 帧不增加处理 FIFO 延迟。

实时相对精确的静态 HRIR／BRIR 最大相对 L2 为 Mac `2.19e-7`、Windows／Linux `2.75e-7`。
连续控制回放的相对 L2 约 0.449%，屏幕回放约 0.472%，密集运动屏幕回放约 0.964%
（整份 PCM，含尾声）。这些运动差异包含有意降低的反射更新率，不能解释成纯浮点舍入误差，
也不能据此判定听感无差别；本轮没有做主观试听或设备端到端延迟测量。

## 回归覆盖

- macOS 全量 Debug CTest：80/80；Release 房间集成：1/1。
- 三平台 Rust：各 83/83，包含 57 项库测试、21 项房间测试、4 项档位测试和两档零分配测试。
- Windows canonical MSVC／Ninja 构建成功，房间集成与 C header 测试 2/2。
- 新覆盖：默认 f32／f64 FFT 与显式标量逐位一致；原生／标量同进程共存；跨档位资源拒绝；
  反射网格各相位的有限 seek；碎片输入与非块边界换房；持续斜坡期间离散事件旁路。
  屏幕映射合成位置有效位不能掩盖原始位置清除的边界已有专门回归。
- C++ 集成覆盖两档离线／文件流式渲染、seek、窗口、异步换房、Scene 48／44.1 kHz、
  屏幕与原生 SOFA；同时持有两档实例，检查准备缓存隔离。旧 C 房间切换继承当前档位。
- Release CLI 两档都完成真实 ADM fixture → WAV；检查 JSON 与实际预热／尾声数据吻合。
- Rust clippy（dsp／ffi 全 targets、sofa＋room-trace）、变更 C/C++ 的 clang-format／clang-tidy／cppcheck、
  FFI 头校验均通过。FFT 参考工具事务测试 7/7，二期一致性工具 40/40；原精确位门禁未放宽。
- Windows／Linux 使用临时源码覆盖验证，逐文件恢复原字节；Windows 原 Git 状态也已恢复。
  未创建独立依赖缓存，私人资源及完整日志保留在忽略的 `local/room-v1/realtime-v1-20261013/`。

探针支持 `room_bench ... --quality exact|realtime` 和
`room_replay ROOM_JSON NEW_OUTPUT_DIRECTORY [--screens] [--dense] --quality exact|realtime`。
所有波形及性能证据都来自 Release；Debug 仅用于正确性回归。
