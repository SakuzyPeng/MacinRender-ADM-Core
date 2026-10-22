# ROOM CUDA V2 保留候选

日期：2026-10-19，2026-10-20 更新。状态：**完整数值回放和显式产品试用接入已验证，正式零超时验收仍未通过**。
当前入口、可移植 GUI 试用包和本轮数据见 [产品试用接入](ROOM_CUDA_PRODUCT_TRIAL.md)。

恢复的是 [关键路径实验](ROOM_CUDA_CRITICAL_PATH.md) 中的最佳 V2 组合，平均每次调用降低
0.654／0.728 ms，配对 p99 改善。1 ms 继续作为优化目标，不作为保留有实测收益候选的硬门槛。
本次整理不增加新的性能结论，也不将旧候选的通过或失败转记到 V2。

2026-10-20 补充：[超时归因](ROOM_CUDA_TAIL_DIAGNOSIS.md) 在独立诊断中确认了与已知硬件电源异常
相关的 GPU 提交／等待长停顿，同时保留另外两次 CPU 侧小幅超时。诊断插桩已撤回，
生产算法与本页的正式验收状态不变；不能用正常时段的 p99 替代单次截止时间要求。

维护者随后要求将已确认的硬件长停顿时段单列排除，继续调查普通小超时。
[小超时调查](ROOM_CUDA_SMALL_OVERRUN.md) 已确认短时 CPU 执行降速，并保留一例独立 GPU 完成等待；
正常后台负载下的小超时仍计入，历史原始记录保留。

## 保留内容与启用范围

- 正常认证和准备后的 HRIR 直接绑定 GPU，保留共享 bank、上下文有效性检查及释放处理。
  适配层为独立的 `mradm-room-cuda` crate；DSP crate 不链接 CUDA。
- 位置控制仅在实际元数据取样点和段尾求值；原点方向历史、事件 clear、音频和非位置控制保持原行为。
  有其他控制运动或显式屏幕映射时回退原路径。通过 `--cuda-compact-control` 显式启用。
- 目标 gather／512 点 R2C／滤波器安装融合、紧凑元数据上传、响应按频率块调度。
  三个原生开关分别开启后，私有 `mradm_room_cuda_optimizations()` 返回 **22**。
- 沿用 EQ 错峰 256 帧及已提交的整数环索引简化；所有距离层、完整 FIR、精度层次、事件时序、
  淡化、FIFO、256 帧延迟和尾声约束不变。

没有恢复 V3 GPU 世界方向解码扩展，也没有其 512 目标容量变更；V2 的 `max_updates` 仍为 256。
卷积 FFT 融合、收紧批次和延后等待不属于保留组合。可对照的卷积融合开关仍默认关闭。
诊断插桩没有恢复。当前打包采用构建时预编译设备程序的部署方式，历史打包草稿继续作为历史资料保留。

`MR_ADM_ENABLE_ROOM_CUDA` 默认 OFF，默认渲染继续使用 CPU，exact 不读取 GPU 选项。
公开 C ABI 的函数、结构和枚举不变。启用 CUDA 构建后，私有环境变量 `MR_ADM_ROOM_CUDA=1`
选择完整 V2，包括紧凑位置控制；未设置或 `0` 使用 CPU，无效值报错，换房继承会话后端。
生产适配器要求原生能力位 `22` 及兼容的私有 kernel contract，旧插件会在绑定前被拒绝。
研究程序 `room_cuda_bench`／`room_cuda_replay` 继续用 `--cuda-compact-control` 明确选择该组合。

候选的实测范围为 Windows x64、96 kHz、4096 点 HRIR FFT、最多 32 源、realtime 和指定 NVIDIA GPU。
离线渲染、文件流式渲染、Scene 流式渲染已有显式试用接入和正确性证据；
尚未通过正式实时截止时间验收，也没有实际声卡持续播放的验收结论。

## 已有收益与代价

数据来自同机、同批资源和事件的无插桩 Release 交替短测：8 个 Audio MMCSS worker，普通调用线程，
96 kHz、512 帧、32 源，每场景两轮各 1024 次调用，预热另计；正常桌面、AC 供电。

| 场景 | V1 均值 ms | V2 均值 ms | 减少 ms | 降幅 | V2 最大 ms | V2 超时 |
|---|---:|---:|---:|---:|---:|---:|
| 位置运动 | 2.881359 | 2.227378 | 0.653980 | 22.7% | 3.1850 | 0/2048 |
| 距离运动 | 2.781343 | 2.053821 | 0.727522 | 26.2% | 3.0726 | 0/2048 |

全部配对 p99 改善。基线距离运动的一次 5.7803 ms 超时照常保留。候选峰值工作集约增加 231 MB，
进程墙钟减去计时 DSP 的其他开销约增加 6 秒；后者不等于独立冷加载计时，显存峰值未单独测量。
这是组合收益，没有把全部收益归到某一个优化项。

完整逐轮数据见 [short-v2.json](evidence/room-cuda-critical-path/short-v2.json)，
源码和冻结二进制身份见 [source-provenance.json](evidence/room-cuda-critical-path/source-provenance.json)、
[binaries-v2.json](evidence/room-cuda-critical-path/binaries-v2.json) 和
[variants.json](evidence/room-cuda-critical-path/variants.json)。恢复时核对了全部 11 个 V2 核心文件的 SHA-256，
与该次实测源码一致；前置接入补丁及整数环改动也保留原内容。

## 构建与运行

当前源码已包含 V2，无需再次应用历史补丁。使用 canonical Rust 1.98.0，复用 Windows MSVC/Ninja 构建和既有 Cargo／
FetchContent 缓存。`$CargoCache` 为该 checkout 的既有 Cargo target 目录，`$MathDxInclude` 为
MathDx 24.08 的 `nvidia/mathdx/24.08/include`；归档与 SHA-256 见
[mathdx.json](evidence/room-cuda-critical-path/mathdx.json)。当前 V2 在构建时预编译目标 FFT，
MathDx 头文件只需在构建机保留，不加入仓库。部署时必须携带插件旁的 `room_cuda/` 和 CUDA 运行库。
当前实测组合为 CUDA 12 和 cuFFTDx 1.2.1。

```powershell
cmake -S . -B build/win-canon -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DMR_ADM_ENABLE_ROOM_CUDA=ON -DMR_ADM_CONSISTENCY_DIAGNOSTICS=OFF `
  -DMR_ADM_FLAC_PROVIDER=VENDORED -DMR_ADM_OPUS_PROVIDER=VENDORED `
  -DMR_ADM_ENABLE_SOFA=OFF -DMR_ADM_ENABLE_IAMF=OFF `
  -DMR_ADM_ROOM_CUDA_FUSE_TARGET=ON -DMR_ADM_ROOM_CUDA_FUSE_CONVOLVE=OFF `
  -DMR_ADM_ROOM_CUDA_COMPACT_UPLOAD=ON -DMR_ADM_ROOM_CUDA_FREQUENCY_MAJOR=ON `
  "-DMR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR=$MathDxInclude"
cmake --build build/win-canon --target mr_adm_room_cuda
$env:RUSTFLAGS = '-Crelocation-model=pic'
$env:CARGO_INCREMENTAL = '0'
cargo build --manifest-path rust/Cargo.toml --release --locked -p mradm-room-cuda `
  --example room_cuda_bench --example room_cuda_replay --target x86_64-pc-windows-msvc `
  --target-dir $CargoCache
$Benchmark = "$CargoCache/x86_64-pc-windows-msvc/release/examples/room_cuda_bench.exe"
$Replay = "$CargoCache/x86_64-pc-windows-msvc/release/examples/room_cuda_replay.exe"
$Plugin = (Resolve-Path build/win-canon/mradm_room_cuda.dll).Path
python scripts/research/room_gpu/resident_transport_test.py --library $Plugin `
  --kernels scripts/research/room_gpu/session_kernels.cu
$env:MR_ADM_ROOM_WORKERS = '8'
foreach ($scenario in @('ramps', 'distance-ramps')) {
    $raw = & $Benchmark $Room96 1024 512 32 --hrir $Collection --quality realtime `
        --scenario $scenario --audio-workers --cuda-library $Plugin --cuda-compact-control
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark failed' }
    $raw
    $result = $raw | ConvertFrom-Json
    if ($result.profiling -or $result.blocks -ne 1024 -or $result.sample_rate -ne 96000 -or `
        $result.call_frames -ne 512 -or $result.sources -ne 32) { throw 'Unexpected configuration' }
    if ($result.deadline_misses -ne 0) { throw 'Candidate deadline missed; stop' }
}
```

`$Room96`、`$Collection` 使用原认证资源；正式计时冻结 EXE、插件和依赖 DLL，记录哈希、私有能力位、
资源身份、负载与供电条件。计时期间不并行构建、profiler 或其他基准。不要开启 `room-profile`、
`room-trace` 或 `ROOM_GPU_TRACE`。冻结参考和候选的交替顺序见 [原短测步骤](ROOM_CUDA_CRITICAL_PATH.md#复现)。

## 验证与待办

整理提交时的本地检查与源码恢复记录见 [retention.json](evidence/room-cuda-critical-path/retention.json)。
先前的 Windows 原生解析 FIR 检查、局部参考测试及整数索引逐位测试仍有各自的适用范围，
不能替代完整原生候选会话的数值门槛。

1. 完整 Windows 数值回放已通过，详见 [本轮证据](ROOM_CUDA_PRODUCT_TRIAL.md#正确性证据)：
   同机旧 CPU realtime 对照 V2，默认和 world-only 场景均覆盖密集事件、
   屏幕、wrap 头姿、固定与碎片分包及完整尾声。严格核对实验标签后才生成供比较器读取的副本；
   原始 manifest 不改。相对 L2 ≤ `2e-5`、最大误差／参考峰值 ≤ `2e-4`，静音保持静音，输出有限；
   帧数、事件样本位置、延迟及尾长完全相同，不对齐、不拟合增益、不裁剪。
2. 正式验收仍未通过，本轮只完成接入回归短测，没有重启六轮长测。正式规则仍是位置、距离各三轮，
   每轮 22,500 次，合计 135,000 次，单次不得超过
   `5.333333 ms`。按维护者后续调整，已确认的硬件长停顿时段单列排除并保留原始记录；
   其余任一超时立即停止，不替换失败轮次，不能只按耗时大小自动排除。
   1 ms 收益目标不再阻止这一候选进入验收。
3. 本轮适用的 macOS CPU／exact 验证及接口覆盖已完成；频繁换房只记录性能，不承诺零超时。
   exact 的离线渲染、文件流式渲染和 Scene 流式渲染逐位基线不放宽。

2026-10-19 的整理只提交候选，没有启动新的 Windows 性能轮次、设备测试或打包发布。
该历史状态不改写；2026-10-20 的接入与试用打包另行记录。旧报告和原始 JSON 保留初次停止时的状态。
