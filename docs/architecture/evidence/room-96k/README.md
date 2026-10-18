# 原生 96 kHz 验证记录

日期：2026-10-17。维护者验证矩阵为 macOS arm64 和 Windows x64。

- [验收摘要](acceptance.json)：三条渲染流程、公开 C ABI、零分配、时序及格式检查。
- [精确回放](replay-comparison.json)：两平台全部准备检查点、HRIR、分支和 PCM 逐位相同；旧 48 kHz 基线不变。
- [macOS 集合扫描](collection-macos.json)、[Windows 集合扫描](collection-windows.json)：九层、1850 个方向及 14800 个中点；节点和边界数值结果相同。
- [声学验证](acoustics-96k.json)：28 个参考位置全部通过既有门槛。
- [真实 96 kHz 渲染](native-render-macos.json)：保留原始 1920 taps；30/40 kHz 三分支响应及真实集合输出非零。
- [macOS 性能](benchmark-macos.json)、[Windows 性能及内存](benchmark-windows.json)：Release、每场景 512 次、每次 512 帧。

## 实时预算

96 kHz / 512 帧的调用预算为 5.333 ms。以下为 32 源实测；超时次数的分母均为 512。

| 场景 | macOS 均值 / p99（ms） | macOS 超时 | Windows 均值 / p99（ms） | Windows 超时 |
|---|---:|---:|---:|---:|
| static | 0.746 / 1.236 | 0 | 1.440 / 2.726 | 0 |
| pose | 2.457 / 3.237 | 0 | 5.975 / 7.270 | 423 |
| switch | 2.917 / 4.475 | 1 | 7.401 / 12.123 | 492 |
| ramps | 4.603 / 5.293 | 4 | 12.703 / 16.054 | 512 |
| distance-ramps | 4.426 / 6.468 | 7 | 11.297 / 13.516 | 512 |

Windows 的 16/32 源动态负载不能视为满足实时预算；macOS 的 32 源压力场景也有少量超时。没有删减反射、距离层或 FIR 来改变输出。

Windows 按 1/16/32 源分别启动独立进程，工作集峰值为 969.3 MiB / 1262.9 MiB / 1575.1 MiB。macOS 报告进程累计 RSS 峰值。
HRIR bank 的 1 GiB 预算不包含整个会话、分支滤波历史及操作系统分配器的 RSS。

所有个人 HRIR 数据均留在工作区之外；Windows 的临时数据副本已移除，原有未提交源码及 Git 状态已完整恢复。
