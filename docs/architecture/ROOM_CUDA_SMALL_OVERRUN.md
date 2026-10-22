# ROOM CUDA 小超时：CPU 执行降速与独立的 GPU 完成等待

日期：2026-10-20。基于保留候选 `05b297ab`，生产源码和 CUDA DLL 均未改变。

按维护者最新要求，已确认的硬件异常长停顿时段单列排除，保留原始记录，
本轮调查窗口外的普通小超时。排除依据是已关联的系统事件和时间线，不是简单删除所有较大的耗时。
日常后台负载仍保留，小超时不会因此作废。

**已复现并确认一类 CPU 小超时：执行期间的有效核心频率短时降到约 0.8–1.1 GHz，
使基本相同的指令量耗时数倍增加。** 另有一次 GPU 完成等待延长造成的小超时；
等待期间线程持续获得 CPU，尚未分解到 GPU 排队、设备执行或驱动完成处理。

## 本轮采集

Windows x64，i7-11800H，8 核／16 逻辑处理器；RTX 3060 Laptop GPU，驱动 610.88。
8 个 Audio MMCSS worker、普通调用线程、96 kHz、512 帧、32 源、realtime、密集距离运动，
32 次预热另计。继续使用相同认证资源、V2 紧凑位置控制与能力位 22 的冻结原生 DLL。
正常桌面应用保留，没有修改供电策略、线程亲和性或系统服务。

| 采集 | 调用数 | 原始超时总数 | 硬件异常／恢复段内超时 | 窗外小超时 |
|---|---:|---:|---:|---:|
| 上轮 CPU ETW | 22,500 | 540 | 538 | 2 |
| 本轮固定 PMU + CPU ETW | 22,500 | 550 | 548 | 2 |
| 本轮 GPU scheduler + CPU ETW | 22,500 | 545 | 545 | 0 |

本轮两种采集各先完成 32 次 smoke，均零超时。上表全部带诊断插桩，**不属于正式无插桩验收**。
硬件时段只是本轮归因分组，没有重写原始 JSON 或用事后筛选结果宣称零超时门槛通过。
GPU 跟踪的异常恢复边缘还包含一条 5.5775 ms 的 CPU 样本，保留在该段记录中，
没有将它解释为一次独立的普通超时。

本轮原始均值、p99、最大值、供电和后台负载快照见
[runs.json](evidence/room-cuda-small-overrun/runs.json)。没有另测峰值内存。
GPU 跟踪没有再次捕获窗口外小超时，不能据此消除 PMU 采集中已发生的两次。

## CPU 小超时的直接证据

PMU 采集中的 `benchmark_call=19649` 外层耗时为 **5.3739 ms**：

- voice batch 合计 2.2975 ms，其中已包含 0.2739 ms GPU 提交，不能重复相加。
- CPU late decoder 合计 1.6456 ms。
- GPU 完成等待仅 0.1424 ms。
- 调用线程在 CPU 上 2.4229 ms，就绪延迟 0.154 ms；处理协调线程两段就绪延迟合计 0.161 ms。
- 调用线程和协调线程在本次调用中的直接 DPC／ISR 重叠均为 0。

固定硬件计数器给出了完整 CPU 执行片段的差异：

| 片段 | 逻辑 CPU | 退休指令数 | 实际核心周期数 | 执行片段 ms | 有效核心频率 GHz |
|---|---:|---:|---:|---:|---:|
| 正常调用 1000，调用线程 | 8 | 5,331,872 | 1,609,623 | 0.385 | 4.181 |
| 小超时 19649，调用线程 | 12 | 5,338,407 | 2,181,369 | 2.214 | 0.985 |
| 小超时 19649，协调线程片段一 | 14 | 1,866,526 | 1,222,988 | 1.551 | 0.789 |
| 小超时 19649，协调线程片段二 | 1 | 1,638,102 | 1,439,204 | 1.340 | 1.074 |

表中每行是完整的相邻 CSwitch 区间，没有把硬件计数器按时间比例拆分。
前两行都包含少量调用前代码：正常片段在调用内重叠 0.3533 ms，慢片段重叠 2.1828 ms。
它们的指令数仅相差约 0.12%，耗时却相差约 5.75 倍。
慢片段还多消耗约 35.5% 的核心周期，因此不能声称全部差异都来自频率；
频率下降是其中最大的已观察因素。参考周期速率仍约 2.28–2.30 GHz，
没有把整段 off-CPU 等待误算成低频执行。

扩大到两组各 50 次调用，结论仍一致：

| 调用线程指标 | 正常窗口 975–1024 | 慢窗口 19624–19673 |
|---|---:|---:|
| 退休指令数 | 263,772,951 | 264,125,413 |
| 核心周期数 | 121,786,150 | 121,274,513 |
| 计数器覆盖的 on-CPU 时间 ms | 29.192 | 45.115 |
| 有效核心频率 GHz | 4.172 | 2.688 |
| IPC：指令／核心周期 | 2.166 | 2.178 |

这两组完整区间覆盖了调用线程约 98% 的 on-CPU 时间。指令量和周期量基本不变，
整体增加的 CPU 执行时间主要对应频率降低，不能解释成这一段场景固定需要更多计算。
同窗口协调线程的有效频率也从约 4.141 GHz 降至 2.954 GHz。

频率低的协调线程仍是 **优先级 21、Important QoS**，调用线程是普通优先级 8／9、
Utility QoS。这排除了 Audio 注册失败或协调线程被降为低优先级这一直接解释。
记录中的线程迁移和 SMT 共享仍可能影响周期成本，但本轮没有证明某个后台应用是原因。

**低频由温度、功耗限制、系统调频策略中的哪一项触发，尚未确认。**
没有据此更改电源设置，也没有将这次小超时归入先前的硬件异常例外。

证据：[完整计数器片段](evidence/room-cuda-small-overrun/critical-counter-spans.json)、
[50 次调用窗口与阶段覆盖率](evidence/room-cuda-small-overrun/pmu-selected.json)、
[CPU 状态及中断重叠](evidence/room-cuda-small-overrun/selected-cpu.json)、
[QoS 与优先级](evidence/room-cuda-small-overrun/qos-switches.json)。

## 上轮两次 CPU 小超时

原来的 18403／18539 号调用为 5.3636／5.7117 ms，GPU 等待合计仅 0.1053／0.4028 ms。
同样调用号在另一轮无 ETW 的 flight 对照中只需 2.1330／2.2095 ms，
在完整 GPU ETW 对照中只需 2.1718／2.3148 ms。

原来的异常附近，几十次调用的中位数整体升到 3.3–3.5 ms；
对照中的相同调用区间约 2.1–2.3 ms。原始跟踪没有记录渲染进程的硬缺页；
直接落在关键线程执行期间的中断不足以解释数毫秒的增量。
这种 CPU 阶段同时变慢的特征与本轮已确认的降速型小超时相符，
但原两次没有 PMU 频率计数器，**不能逐次断言它们也一定由降频造成**。

对应窗口和阶段对照见 [prior-controls.json](evidence/room-cuda-small-overrun/prior-controls.json)；
上轮全部原始证据继续保存在 [原诊断报告](ROOM_CUDA_TAIL_DIAGNOSIS.md)。

## 一次独立的 GPU 完成等待

PMU 采集中的 `benchmark_call=16973` 外层耗时 **5.9673 ms**。
CPU voice batch／late decoder 分别为 0.8303／0.8480 ms，GPU 提交合计 0.2034 ms，
GPU 完成等待合计 **3.8519 ms**。

第二次完成等待单独占 **3.4301 ms**，期间：

- 协调线程持续在逻辑 CPU 3 上运行，没有 off-CPU 就绪延迟。
- 记录到的直接中断重叠仅 0.008 ms。
- 包含该等待的完整 CPU 区间有效频率约 4.03 GHz，线程维持优先级 21。

因此这次主要停留在设备／驱动完成等待，不能归因于处理线程被抢占了 3 ms，
也不属于上一节的低频 CPU 计算样本。该次 PMU 采集没有开启 GPU scheduler provider，
现有证据不能把等待继续分摊为 GPU 排队、kernel 执行、其他 context 竞争或驱动完成处理。

随后单独增加 GPU scheduler 跟踪，使用 `0x8603` 关键词，关闭 PMU；
已验证采集到 QueuePacket、DmaPacket 和 NodePerfData 事件。
该轮 22,500 次没有再次出现硬件异常时段之外的小超时，
所以没有获得与这次 3.43 ms 等待对应的 GPU 时间线。该问题保留，未判为已解决，
也没有指认浏览器、桌面或其他应用。

## 方法与复现

沿用 [上一轮的诊断补丁与采集流程](ROOM_CUDA_TAIL_DIAGNOSIS.md#复现与恢复)，
复用冻结的 Rust EXE、辅助 DLL、原生 CUDA DLL、相同资源与 canonical 缓存。
本轮只恢复私有探针及资源副本，没有重新编译或修改生产源码。

PMU 使用以下固定计数器，由 `xperf -pmcsources` 确认主机可用：

```powershell
xperf -pmcsources
xperf -on LOADER+PROC_THREAD+CSWITCH+DISPATCHER+POWER+INTERRUPT+DPC+HARD_FAULTS `
    -pmc InstructionsRetiredFixed,UnhaltedCoreCyclesFixed,UnhaltedReferenceCyclesFixed CSWITCH strict `
    -ClockType PerfCounter -buffersize 1024 -minbuffers 32 -maxbuffers 128 `
    -f "$Case/kernel.etl"
```

同步标记 provider、启动目标、停止自建 logger、合并 ETL 的步骤与上一轮相同，
仍须先检查 logger 所有权并使用 `finally` 清理。目标命令：

```powershell
$env:MR_ADM_ROOM_WORKERS = '8'
$env:MR_ROOM_DIAGNOSTIC_JSON = "$Case/result.json"
$env:MR_ROOM_FLIGHT_LOG = "$Case/flight.bin"
& $Benchmark $Room96 22500 512 32 --hrir $Collection --quality realtime `
    --scenario distance-ramps --audio-workers --cuda-library $Plugin --cuda-compact-control
xperf -i "$Case/merged.etl" -a tracestats
python docs/architecture/evidence/room-cuda-small-overrun/decode-pmu.py $Case --xperf $Xperf
python docs/architecture/evidence/room-cuda-small-overrun/analyze-pmu.py $Case
```

最后一个脚本选择本次归档中的调用号；新采集应先从 flight 定位对应调用。
选取调用附近的 xperf CSV 后，可用
[analyze-focus-cpu.py](evidence/room-cuda-small-overrun/analyze-focus-cpu.py) 复核线程状态与中断重叠。
发布的分析脚本已在本次归档数据上重跑，生成的结果与报告证据一致。

GPU 对照去掉 `-pmc ... CSWITCH strict`，将独立 provider 改为：

```text
Microsoft-Windows-DxgKrnl:0x8603:5+973c1dc0-11c4-4ae9-8f78-316c8d586f8a:0x1:5
```

`0x8603` 为本机 provider 的 Base、Profiler、DxgKrnl_Power、DriverEvents、GPUScheduler。
捕获结果属于诊断，不能与冻结无插桩二进制的正式门槛混用。

计数器按相同逻辑 CPU 上相邻 CSwitch 的累积计数取差；
`Pmc` 行在对应 CSwitch 前，按时间戳和新入线程 ID 校验对应关系。
本轮匹配得到 4,988,380 个单调区间，没有负计数增量；一个线程连续性不符的区间被丢弃，没有补算。
阶段边界只截取 CPU 时间；硬件计数器只累计完整区间，不假设指令在区间内均匀分布。
这也是单个短阶段计数器覆盖率可能为 0、需要查看完整区间或 50 次窗口的原因。

有效核心频率为核心周期增量除以区间时长，IPC 为退休指令增量除以核心周期增量。
这些量描述该区间的实际执行速度，不是对处理器电压、温度或物理降频触发机制的测量。
flight／ETW 对齐不确定度约 ±17.5 µs；两个 CSwitch 间的周期速率使用同一 ETW 时基。
DPC 与 ISR 在同一 CPU 上取区间并集后求重叠，避免将嵌套中断重复相加。

方法参考：[Microsoft PMU 采集](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/recording-pmu-events)、
[Microsoft CPU 状态与中断分析](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis)。
原始 ETL 和详细进程信息保留在私有压缩归档，公开证据不含主机路径和用户标识。

## 对后续优化的含义

这轮没有发现“某个固定输入位置突然需要数倍计算”的证据。
CPU 路径需要在短时较低执行速度下仍留出余量；值得进一步细分调用线程的输入／元数据准备，
以及 worker 上仍由 CPU 完成的 voice、world／late 处理。
线程放置或功耗 QoS 可以另做受控对照，但 Important QoS worker 也出现低速，
不能预先承诺提高优先级就能消除问题。

GPU 的独立小超时继续需要同一次事件的设备时间线，或提交／设备完成的进一步分项计时。
本轮没有改变声学计算、完整 FIR、距离层、元数据时序、FIFO、淡化、延迟或尾声；
V2 的完整数值验收和正式长测仍待完成。

本轮原始跟踪压缩归档为 364,936,539 字节，58 个成员的 SHA-256 和 ZIP CRC 均已验证；
见 [归档验证](evidence/room-cuda-small-overrun/archive-verification.json)。
Windows 工作区仍为干净的 `05b297ab`，CMakeCache、canonical／Cargo／FetchContent 缓存保留，
自建 trace session 已停止，临时资源和 SDK 已清理。
见 [清理记录](evidence/room-cuda-small-overrun/cleanup.json) 与
[检查记录](evidence/room-cuda-small-overrun/checks.json)。
