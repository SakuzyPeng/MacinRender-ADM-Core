# ROOM CUDA V2 超时归因

日期：2026-10-20。基于 `05b297ab84f8e449472c9c6eab6ae17aad9ab0de` 的保留 V2。
本次只做诊断；生产算法、默认 CPU 路径、exact、公共 C ABI 和延迟均未改变。

已确认一类大幅超时与测试机的电源状态异常、显卡调度暂停有关。调用既会卡在 GPU 提交中的
Windows 资源等待，也会长时间等待 GPU 完成。维护者确认电池状态短暂消失是已知硬件缺陷，
无法改善。本页保留初次诊断的全部原始计数。

2026-10-20 后续范围调整：维护者要求将已确认的硬件长停顿时段单列排除，集中调查普通小超时。
后续 PMU 结果已确认一类 CPU 执行降速，并保留一例独立的 GPU 完成等待，
见 [小超时调查](ROOM_CUDA_SMALL_OVERRUN.md)。原始 JSON 和本页的历史统计不改写。

这不能解释所有超时。本轮另外捕获到两次约 5–6 ms 的独立超时，GPU 等待很短，
耗时主要增加在 CPU 处理阶段；目前证据不足以进一步区分 CPU 频率、缓存、内核／中断和具体运算。
历史未带时间线的单次 5.6748／6.7507／7.1111 ms 超时也不能倒推成同一个原因。

## 配置与结果

Windows x64，NVIDIA GeForce RTX 3060 Laptop GPU，驱动 610.88，8 个 Audio MMCSS worker，
普通调用线程，96 kHz、512 帧、32 源、realtime，紧凑位置控制开启，原生能力位 22。
HRIR、房间和密集事件沿用 V2 资源。32 次预热另计，测量完整 Session 调用。
所有运行保留正常桌面应用和后台服务，记录供电与后台 CPU 样本；没有设置空闲门槛或停止用户应用。

CUDA DLL 在全部运行中相同，SHA-256 为
`90e258d95901d694945b56c6eb78b632785e209a7ad861424afbe2ed8c45da48`。
粗粒度诊断使用独立 Rust EXE 和辅助 DLL，没有重编或修改 CUDA kernel。
身份及资源哈希见 [provenance.json](evidence/room-cuda-tail-diagnosis/provenance.json)，
全部原始结果、负载和供电快照见 [runs.json](evidence/room-cuda-tail-diagnosis/runs.json)。

| 运行 | 场景 | 调用数 | 均值 ms | p99 ms | 最大 ms | 超时数 |
|---|---|---:|---:|---:|---:|---:|
| 冻结 V2，无插桩 | 位置运动 | 1,024 | 2.382613 | 3.0180 | 3.2478 | 0 |
| 冻结 V2，无插桩 | 距离运动 | 1,024 | 2.001944 | 2.4710 | 4.9110 | 0 |
| flight，无 ETW | 距离运动 | 1,024 | 2.138861 | 2.6926 | 3.0801 | 0 |
| flight + CPU／GPU ETW | 距离运动 | 22,500 | 2.333630 | 9.6183 | 44.7695 | 598 |
| flight，无 ETW | 距离运动 | 22,500 | 2.104555 | 2.7088 | 5.1914 | 0 |
| flight + CPU ETW，无 GPU provider | 距离运动 | 22,500 | 2.441376 | 9.5225 | 85.4801 | 540 |

阈值为 `5.333333 ms`。后四行均有 QPC 阶段插桩，**不是正式验收**；
22,500 次无 ETW 对照的零超时也不替代六轮无插桩验收。
当前采集没有另测峰值内存，原始结果中的 `rss_peak_bytes` 为 null。
前两行只是本次定位前的无插桩短测，不增加完整数值回放或正式长测通过结论。

## 电源异常与 GPU 路径停顿

`etw-distance-1` 的 598 次超时全部集中在计时调用 4,000–5,999 内。
其他每千次分组均无超时。ETW 没有丢失事件或缓冲区。

同一 GPU adapter 上两次出现以下序列；执行这些操作的 System 线程也唤醒了阻塞中的渲染线程：

| 相对 flight 起点 | 事件 |
|---:|---|
| 9,987.162 ms | `FlushScheduler` |
| 9,995.101 ms | 暂停全部 VidPn source 的 scheduler，保留软件队列 |
| 9,995.104–9,998.661 ms | `DdiNotifyAcpiEvent`，EventType 1、Event `0xD2` |
| 9,998.665 ms | `ResumeSchedulerForVidPnSource` |
| 15,808.025 ms | 再次 `FlushScheduler` |
| 15,831.711 ms | 再次暂停 scheduler |
| 15,831.713–15,838.272 ms | `DdiNotifyAcpiEvent`，EventType 1、Event `0xD1` |
| 15,838.275 ms | 恢复 scheduler |

EventType 1 是 `DxgkAcpiEvent`，见
[Microsoft DXGK_EVENT_TYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/ne-dispmprt-_dxgk_event_type)。
NVIDIA 公共头文件将 `0xD1`／`0xD2` 命名为 power-level 通知，见
[NVIDIA acpi_common.h](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/src/nvidia/inc/kernel/platform/acpi_common.h)。
该命名只辅助解释事件；它不是对 Windows 私有驱动实现的证明，也不意味着 Windows 设备 D1／D2
状态或物理拔掉 AC 电源。

Windows Kernel-Power 521 同期记录：

- `08:19:23.3449438Z`：ValidBatteryCount 从先前的 1 变为 0。
- `08:19:29.3925701Z`：ValidBatteryCount 恢复为 1。
- 运行前后 AC 快照均为 1；未据此推断交流电断开。

同 adapter、Node 0 的核心频率样本从约 2,062 MHz 降至 1,282–1,387 MHz，之后恢复。
没有测量显存时钟；降频本身也不足以解释所有停顿幅度。
事件日志和图形事件不是同一个时钟采样点，因此只按同一异常时段关联，不声称它们完全同时发生。

关键调用的阶段分解如下，编号包含前 32 次预热：

| benchmark_call | 完整调用 ms | 直接观察 |
|---:|---:|---|
| 4814 | 16.5012 | 第二次 GPU 提交 14.0935 ms，其中 `WrResource` 阻塞 13.951 ms；就绪后只等 0.015 ms |
| 5002 | 24.8074 | GPU 完成等待合计 22.0307 ms；第一次等待 19.9473 ms 期间线程持续获得 CPU |
| 5421 | 44.7695 | GPU 提交合计 19.6036 ms，GPU 等待合计 22.3323 ms；第二次提交阻塞 19.056 ms，就绪延迟仅 0.011 ms |

4814／5421 的 `WrResource` 都由 System PID 4、TID 103176 唤醒；
正是该线程处理上述同 adapter 的 ACPI／scheduler 转换。
5421 的调用线程只使用约 0.548 ms CPU，就绪延迟合计 0.012 ms，
其余主要等待处理线程；CPU late decoder 合计约 1.159 ms。
这些证据将此类停顿定位到 GPU／Windows 图形与电源管理路径，而非调用线程长时间抢不到 CPU。
GPU 等待处的 CPU 时间可能包含驱动轮询及内核／中断，不能当作纯 DSP 运算量或 GPU kernel 执行时间。
阶段有嵌套，`voice_batch` 已包含 `gpu_submit`，不能将所有阶段简单相加。

证据：[图形事件](evidence/room-cuda-tail-diagnosis/graphics-transitions.json)、
[唤醒来源](evidence/room-cuda-tail-diagnosis/resource-wakers.json)、
[阶段与线程状态](evidence/room-cuda-tail-diagnosis/etw-distance-1-selected-cpu.json)、
[核心频率](evidence/room-cuda-tail-diagnosis/gpu-clock.json)、
[电源日志](evidence/room-cuda-tail-diagnosis/power-events.json)。

## 不采集 GPU 事件的对照

`etw-cpu-distance-1` 只开启 CPU kernel provider 和两个同步标记，不开启 DxgKrnl。
仍记录到 540 次超时，其中 538 次集中在计时调用 6,000–6,999。
电池数量再次于 `09:13:07.1039897Z` 变为 0、`09:13:13.1517471Z` 恢复为 1。
这排除了“只有详细 GPU ETW provider 才出现大幅停顿”的解释，但不能消除全部观测开销。

最慢调用 6666 为 85.4801 ms。其中第一次 GPU 等待为 80.4323 ms，
线程在 CPU 上约 80.4173 ms，就绪延迟 0.015 ms；两次提交合计仅 0.4005 ms。
该等待位于电池状态异常区间。调用线程本身的就绪延迟合计仅 0.013 ms。
这里没有 GPU 执行时间线，不能进一步将等待精确分摊为设备执行、驱动调度、内核／中断等部分。

异常区间之外还有两次超时：

| benchmark_call | 外层标记 ms | GPU 等待合计 ms | 调用线程在 CPU 上 ms | 调用线程就绪延迟 ms |
|---:|---:|---:|---:|---:|
| 18403 | 5.3636 | 0.1053 | 2.1386 | 0.073 |
| 18539 | 5.7117 | 0.4028 | 2.0157 | 0.130 |

这两次 voice batch 合计约 2.101／1.774 ms，late decoder 合计约 1.173／1.494 ms。
它们呈现 CPU 侧阶段耗时增加，未出现上述长 GPU 等待或 `WrResource` 阻塞。
CPU “在运行”时间包含内核／中断，缺少采样栈及对应 CPU 频率证据，尚不能直接归为响应合成计算瓶颈。
这两个调用不在原始 JSON 保存的最慢 32 次之内，表中耗时取自外层 flight 标记；
外层标记略包含诊断边界开销，精确总超时数仍采用 benchmark 自身的 540。

证据：[阶段](evidence/room-cuda-tail-diagnosis/etw-cpu-distance-1-phases.json)、
[线程状态](evidence/room-cuda-tail-diagnosis/etw-cpu-distance-1-selected-cpu.json)。

## 采集方式和证据限制

辅助 DLL 预分配并预触页，处理期间只记录选定阶段的 QPC、线程 ID、调用 ID 和开始／结束标记，
不写文件、不分配；渲染结束后写出。阶段只有 Session、canonical block、worker roundtrip／execution、
voice batch、late decoder、GPU submit／wait；没有启用 Rust `room-profile` 阶段计数器。
`worker_execution` 是协调线程的范围，8 个 worker 的 OS TID 另在测量前枚举。

ETW 使用 PerfCounter 时钟。起止同步事件各由两次 QPC 夹住，取两端区间交集：
完整 GPU 采集的对齐不确定度不超过约 1.95 µs；CPU-only 对照不超过约 16.05 µs。
两轮均为零丢失事件／缓冲区。处理结束后的解码和构建不与计时并行。

前期 Nsight 2022.4.2 试跑也完整保留：

- NVTX-only，1,024 次，最大 3.0180 ms、0 超时。
- CUDA／NVTX／WDDM，1,024 次，最大 5.4719 ms、1 超时；导出进程以 `0xC0000005`
  崩溃，没有可用 trace，不能归因。
- CUDA／NVTX，不采 WDDM，1,024 次，最大 5.0673 ms、0 超时。
  此次在首个计时调用开始采集，最大值受到采集启动影响；后续改为预热前采集。
- Graph 级采集，4,096 次，最大 4.7091 ms、0 超时；该版本没有导出可用的 GPU
  kernel／graph execution 表，不能用 copy 范围代替 GPU 全部执行时间。
- 首次 ETW smoke 因 provider GUID 带花括号报 `0x3ec`，尚未运行目标；修正后 32 次 smoke
  通过。所有自建 trace session 均已停止。

Nsight 对调用耗时有显著观测影响，因此转用更轻的 flight + ETW 诊断。
该版本导出的 `SCHED_EVENTS` 不能当作成对的切出／切入事件；本报告 CPU 状态只采用
原始 ETW `CSwitch`／`ReadyThread`。DxgKrnl 同名事件有多种 CSV schema，
选取完整字段数唯一匹配的表头；用于归因的选取结果没有歧义。
“切出时下一个进程”不等于“整个就绪延迟期间的竞争进程”，本报告没有使用这种错误归因。
早期错误推导没有进入本报告结论。

## 复现与恢复

使用 [V2 构建步骤](ROOM_CUDA_V2_CANDIDATE.md#构建与运行) 先冻结无插桩 EXE、原生 DLL 和依赖，
保留其 SHA-256。诊断时才应用
[diagnostic.patch](evidence/room-cuda-tail-diagnosis/diagnostic.patch)，不要开启 `room-profile`、
`room-trace` 或 `ROOM_GPU_TRACE`。复用 canonical Cargo／FetchContent 缓存。
以下变量均指操作者本机路径；`$Case` 必须是新的输出目录，资源与 V2 相同。

```powershell
git apply --check docs/architecture/evidence/room-cuda-tail-diagnosis/diagnostic.patch
git apply docs/architecture/evidence/room-cuda-tail-diagnosis/diagnostic.patch
$env:RUSTFLAGS = '-Crelocation-model=pic'
$env:CARGO_INCREMENTAL = '0'
cargo build --manifest-path rust/Cargo.toml --release --locked -p mradm-room-cuda `
    --example room_cuda_bench --target x86_64-pc-windows-msvc --target-dir $CargoCache
cl /nologo /utf-8 /O2 /EHsc /std:c++20 /DNDEBUG /W4 /LD `
    "/I$CudaRoot/include" docs/architecture/evidence/room-cuda-tail-diagnosis/timeline.cpp `
    "/Fo$Diag/timeline.obj" "/Fe$Diag/room_timeline.dll" `
    /link "/LIBPATH:$CudaRoot/lib/x64" cudart.lib advapi32.lib
Copy-Item "$CargoCache/x86_64-pc-windows-msvc/release/examples/room_cuda_bench.exe" $Diag
# 将已冻结的 mradm_room_cuda.dll 及所需运行库放入 $Diag；保持 MathDx include 路径有效。
$Benchmark = "$Diag/room_cuda_bench.exe"
$Plugin = "$Diag/mradm_room_cuda.dll"
$env:MR_ADM_ROOM_WORKERS = '8'
$env:MR_ROOM_DIAGNOSTIC_JSON = "$Case/result.json"
$env:MR_ROOM_FLIGHT_LOG = "$Case/flight.bin"
& $Benchmark $Room96 22500 512 32 --hrir $Collection --quality realtime `
    --scenario distance-ramps --audio-workers --cuda-library $Plugin --cuda-compact-control
if ($LASTEXITCODE -ne 0) { throw 'Diagnostic failed' }
```

上述为无 ETW 对照。ETW 版本在目标进程运行前启动、结束后停止。先检查 `xperf -loggers`，
已有 `NT Kernel Logger` 或同名 logger 时停止本次操作，不接管其他会话。
只停止自己成功启动的 logger，异常退出也要在 `finally` 中停止：

```powershell
$Anchor = '973c1dc0-11c4-4ae9-8f78-316c8d586f8a:0x1:5'
$Providers = "Microsoft-Windows-DxgKrnl:0xffff:5+$Anchor"
# CPU-only 对照：$Providers = $Anchor
$Existing = xperf -loggers | Out-String
if ($LASTEXITCODE -ne 0 -or $Existing -match 'NT Kernel Logger|MRADMRoomTailGpu20261020') {
    throw 'Cannot acquire independent trace sessions'
}
$OwnKernel = $false
$OwnGpu = $false
try {
    xperf -on LOADER+PROC_THREAD+CSWITCH+DISPATCHER+POWER+INTERRUPT+DPC+HARD_FAULTS `
        -ClockType PerfCounter -buffersize 1024 -minbuffers 32 -maxbuffers 128 -f "$Case/kernel.etl"
    if ($LASTEXITCODE -ne 0) { throw 'Kernel trace failed' }
    $OwnKernel = $true
    xperf -start MRADMRoomTailGpu20261020 -on $Providers -ClockType PerfCounter `
        -buffersize 1024 -minbuffers 32 -maxbuffers 128 -f "$Case/gpu.etl"
    if ($LASTEXITCODE -ne 0) { throw 'GPU/anchor trace failed' }
    $OwnGpu = $true
    & $Benchmark $Room96 22500 512 32 --hrir $Collection --quality realtime `
        --scenario distance-ramps --audio-workers --cuda-library $Plugin --cuda-compact-control
    if ($LASTEXITCODE -ne 0) { throw 'Diagnostic failed' }
} finally {
    if ($OwnGpu) { xperf -stop MRADMRoomTailGpu20261020 }
    if ($OwnKernel) { xperf -stop }
}
xperf -merge "$Case/kernel.etl" "$Case/gpu.etl" "$Case/merged.etl"
if ($LASTEXITCODE -ne 0) { throw 'Merge failed' }
xperf -i "$Case/merged.etl" -a tracestats
python docs/architecture/evidence/room-cuda-tail-diagnosis/decode-selected-cpu.py $Case `
    --xperf $Xperf --call 18403 --call 18539
```

脚本直接流式解码已保存的 ETL，无需保留多 GB CSV。完整 DxgKrnl 事件另由
`xperf -i merged.etl -a dumper` 导出；遇同名多表头事件，必须按完整字段数匹配并拒绝歧义。
系统电源日志查询使用相同 UTC 时间范围：

```powershell
$Query = "*[System[Provider[@Name='Microsoft-Windows-Kernel-Power'] and " +
    "TimeCreated[@SystemTime >= '$StartUtc' and @SystemTime <= '$EndUtc']]]"
wevtutil qe System "/q:$Query" /f:xml /e:Events
```

临时源码以修改前的 SHA-256 为保护，逐文件恢复原始字节，移除新增辅助模块；
没有整仓 reset 或覆盖其他工作。Windows 已恢复为干净的 `05b297ab`，
原 CMakeCache 不变，canonical Release CLI／C API 及无插桩 benchmark／replay 示例重新构建，
backends 烟测和四项定向 CTest 通过。
见 [restoration.json](evidence/room-cuda-tail-diagnosis/restoration.json) 与
[CTest 日志](evidence/room-cuda-tail-diagnosis/restoration-ctest.log)。
Mac 的原有未提交研究改动保留。

原始 ETL、Nsight report、flight、冻结二进制和私有环境记录保存在本地 `local/` 压缩归档，
不放入仓库。归档 691,166,796 字节，135 个成员逐一校验 SHA-256 和 ZIP CRC；
公开证据只包含必要的脱敏摘要、诊断补丁和复现工具。临时资源副本和 SDK 清理后，
canonical、Cargo、FetchContent 缓存继续保留。

## 对下一步优化的影响

继续优化响应合成或 CPU 处理，仍可能降低普通调用耗时和小幅超时概率；
它不能消除本次已经观察到的十几至几十毫秒系统／GPU 路径停顿。
在初次诊断采用的全样本口径下，这些停顿会导致超过 5.333333 ms，
P99 达标也不能替代单次截止时间要求。后续按维护者的新范围单列硬件例外，
其余小超时继续保留和调查。

若继续采用 GPU 并要求跨过此类停顿，需要单独讨论异步队列、缓冲余量或故障回退，
重新明确允许的延迟和音频行为；本轮没有实现或默认授权这些契约变更。
V2 仍是显式保留候选，完整原生数值门槛和正式六轮长测状态没有改变。
