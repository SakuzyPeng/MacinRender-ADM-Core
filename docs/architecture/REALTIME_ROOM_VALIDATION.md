# 房间 v1 验证记录

`room-binaural` 已接入离线渲染、文件流式渲染和 Scene 流式渲染，包含 CLI、C ABI v1.46、
房间资源、3DoF、异步切换、seek / epoch reset 和确定长度的排尾。
详细数据和已测核心源码哈希见 [机器可读报告](ROOM_V1_VALIDATION.json)，接口和限制见
[设计文档](REALTIME_ROOM_RENDERING.md)。

## 数值与声学回归

Rust 1.98.0 Release，在 macOS arm64、Windows x64、Linux x64 上使用同一资源和样本事件，
准备系数、HRIR 查询、直达 / 早反射分支、最终 PCM 和有效帧数逐位相同。
回放包含距离 / 头姿 / 源控制、不同几何房间切换及固定 / 碎片输入块。

- 输出：163,398 帧双耳 f32。
- PCM SHA-256：`b3a3ec3747db934c995289036c7d65eb0224b0726ffc7b8f74aad62a1ffe453d`。
- 22 个标准全频参考位及 6 个补点：28/28 波形重建通过。
- 最大相对 L2：`3.4903576e-5`，门槛 `2e-4`。
- 资源完整性与声学标准判断分开：原 22.2 的固定条件结论保持；正下方补点原有 RT 诊断没有改为通过。

正确性检查包含 13 项 Rust 房间集成测试、7 项内核单元测试、全工作线程零分配检查和
浮点环境恢复检查。macOS Debug 与 Windows 原生检查覆盖 C API、离线 / 文件流式逐位对照、
seek、窗口裁剪、Scene EOS、资源持有、后台切换失败及旧尾声。
C ABI 导出为 152 个 `adm_*` 函数；私有 FFI 247 个函数、79 个结构体与 Rust 一致。

## 桌面实时容量

全部为 Release。内核测试调用 512 帧（预算 10.667 ms），每次调用更新头姿；Scene 流式测试
按 100 Hz 提交头姿，每 5 秒请求一次房间切换，使用默认缓冲策略及 null-device。
未扩大 2048 帧追踪预渲染上限，没有按负载删反射或更改音质。

| 机器 | 32 源静态 P99 | 32 源连续头姿 P99 | 32 源双房间 P99 |
| --- | ---: | ---: | ---: |
| Apple M4 Pro | 1.306 ms | 2.668 ms | 3.606 ms |
| Intel Core i7-11800H / Windows | 2.600 ms | 5.648 ms | 8.432 ms |

上述内核场景均无单次预算超时；1 / 16 源结果也保存在 JSON 报告。

| Scene 场景 | 时长 / 切换数 | 欠载 | 进程 CPU¹ | 峰值 RSS |
| --- | --- | ---: | ---: | ---: |
| macOS，几何变化 | 60 s / 12 | 0 | 164% | 672 MB |
| Windows，几何变化 | 120 s / 24 | 0 | 237% | 511 MB |
| Windows，材料 / 晚场发送变化 | 120 s / 24 | 0 | 248% | 511 MB |

¹ 100% 表示占用一个逻辑 CPU 的处理时间。输出有效帧数均等于素材帧数加声明尾声，EOS 正确。
Windows 房间 DSP 池成功注册 8 个 Audio MMCSS 线程；流式协调线程也采用对应调度。

## Linux 云主机与延迟边界

Linux 云主机完成了房间数值与正确性验证，但没有通过 32 源实时计算预算。
默认 8 个工作线程、512 帧调用时，连续头姿 P99 约 17.389 ms，双房间约 24.273 ms；
16 / 32 线程对照也未满足单块预算。256 帧、187.5 Hz 头姿压力测试同样存在超时，完整结果保留。
这里的 `deadline_misses` 是内核计时，不能改称实际设备欠载，也不能以此推断所有 Linux 机器的能力。
该云主机不在已通过的 32 源实时容量范围内。

macOS / Windows 的控制到 PCM 首次变化均为 257 帧，即 5.354 ms。
这项测量不含传感器、现有预渲染队列、系统或设备缓冲；尚未测量传感器到耳机的实际延迟。
当前交付仍为 48 kHz、worker 头追、单半径 HRIR 距离近似；HOA、GUI、输出阶段头追和多普勒不在 v1 范围内。

## 复现入口

```sh
# 使用当前构建缓存，以 Release 构建 mradm-dsp examples / 原生基准目标。
room_replay /path/to/room.json /new/output-directory
python3 scripts/room/compare-replays.py \
  macos-arm64=/path/mac/manifest.json windows-x64=/path/win/manifest.json \
  linux-x64=/path/linux/manifest.json --output /path/comparison.json

mr_adm_room_scene_benchmark /path/room.json /path/other-room.json 10 kernel-worker
mr_adm_room_scene_benchmark /path/room.json /path/other-room.json 120 stream-cycles
room_bench /path/room.json 938 512
```

性能结果只适用于记录中的配置和机器；新房间包应重新做声学与容量验证，不能继承参考房间的通过结论。
