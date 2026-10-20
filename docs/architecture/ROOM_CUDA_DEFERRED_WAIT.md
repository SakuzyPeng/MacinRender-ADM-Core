# ROOM CUDA：延后等待至 FIFO 消费

日期：2026-10-19，源码基线 `e14fe43c` 加归档的内存资源绑定候选。
**第 4 项未通过 Windows 短测：距离运动 1024 次调用出现 1 次超时，最大 5.6600 ms。**
位置运动短测零超时；两项候选累计 2048 次、1 次超时。剩余对照及六轮长测停止，没有重跑替换结果。
运行时改动回退，保留经过原生验证的回归工具扩展。

[验证记录](evidence/room-cuda-deferred-wait/validation.json)、
[增量补丁](evidence/room-cuda-deferred-wait/deferred-wait.patch)、
[源码哈希](evidence/room-cuda-deferred-wait/source-provenance.json) 和
[逐次对照记录](evidence/room-cuda-deferred-wait/short.json) 一并归档。
本轮没有叠加批次缩紧、上传合并或 FFT 融合。

## 实现范围

利用原有 256 帧 FIFO，把当前调用尚不需要的最后一批 GPU 输出留到第一次读取它时完成。
例如 512 帧调用返回前，CPU worker 已完成两块的提交和独立的晚场／解码工作，但第二块的 GPU 结果
可以到下一次调用消费 FIFO 时再等待。下一次输入准备可以与这批 GPU 工作重叠。
同一调用需要的输出仍先等待完成，再写入调用者缓冲；没有额外队列或延迟。

- 仅在稳定房间、没有 headLocked 音频及其尾声、没有切换、尚未 EOS 的既有流水路径启用。
  其他情形沿用立即完成路径；退出流水路径时先完成此前留存的输出。
- GPU 只有一批未完成工作，锁页输入／输出、频谱和滤波器继续由后端拥有；没有长期双份大缓冲。
  CPU worker 在 `process()` 返回前全部收齐，不跨调用持有借用的输入、事件或输出切片。
- 完成前保留原声源顺序和块元数据；configure、reset、queue_room 在修改其依赖状态前先完成旧批次。
  EOS 后消费原 FIFO，完整排尾。释放会话时后端同步未完成工作再释放资源。
- 保持 FFT、完整 FIR、距离层、浮点精度、归约顺序、元数据时钟、淡化及 **256 帧延迟**。
  CPU／exact／默认路径不启用该实验。
- 完成可以发生在调用线程，私有 native `finish()` 因此显式绑定 device 0；上下文失效检查保留。
  这是调用线程的 CUDA 上下文绑定，没有调整系统 GPU、驱动或功耗设置。
  [CUDA 12.0 的线程上下文规则](https://docs.nvidia.com/cuda/archive/12.0.0/cuda-runtime-api/group__CUDART__DRIVER.html)。

通过研究参数 `--cuda-deferred-wait` 选中这条路径。基线和候选使用**相同的冻结 EXE、DLL、内核及资源**，
只有该参数不同；输出 JSON 明确记录 `experiment.deferred_wait`。因此本轮基线也包含跨线程完成的设备绑定。
没有重新分配音频 worker 的数量或优先级，调用线程保持普通调度。

## Windows 短测

i7-11800H／RTX 3060 Laptop 6 GiB，驱动 610.88、CUDA 12.0；8 个 Audio MMCSS DSP worker，
96 kHz、512 帧、32 源、realtime、无插桩 Release。每项先预热 32 次，再计 1024 次。
正常桌面环境、接通电源，计时期间本任务没有在测试机上并行构建或运行另一基准。

原计划两轮交替对照：第一轮位置先基线后候选，距离先候选后基线；第二轮反转。
第一轮距离候选失败后停止，所以只完成下列三项：

| 配置 | 场景 | 调用数 | 均值 ms | p99 ms | 最大 ms | 超时 |
|---|---|---:|---:|---:|---:|---:|
| 立即等待基线 | 位置运动 | 1024 | 2.748289 | 3.1660 | 3.4375 | 0 |
| 延后等待 | 位置运动 | 1024 | 2.608942 | 3.1617 | 3.3463 | 0 |
| 延后等待 | 距离运动 | 1024 | 2.712787 | 3.4987 | **5.6600** | **1** |

单组位置对照的均值约降低 5.1%，但距离基线与第二轮尚未执行，不能据此认定有稳定的整体收益。
距离超时发生在零基 `measured_call=11`，含预热的 `benchmark_call=43`；预热已经排除，失败照常计入。
无插桩结果没有给出尖峰的阶段归因，不能据此归因于 CUDA 等待、调用线程或某个后台应用。

候选峰值工作集 **1,477,881,856 字节**，位置基线为 **1,477,885,952 字节**，包含准备阶段。
这不是独立显存测量，也没有证据证明加载准备加速。命令、进程墙钟、供电／负载快照和最慢调用均保留在 short.json。

计时仍包含每次调用必须消费的输出等待；最后一块不属于该次 FIFO 输出，下一次调用消费时才计入等待。
没有提前返回本次应交付的 PCM。该调度变化的正确性由下列本地回归验证；软件 Session 计时不代表设备欠载次数。

## 已验证与未执行的检查

- 本地 **15 项**测试通过：4 项 GPU 参考／生命周期、1 项准备资源、1 项零分配、9 项质量测试。
  既有 48／96 kHz 回归开启延后模式，覆盖碎片分包、屏幕、方向跨原点、headLocked 回退、
  位置控制、EQ、reset、换房及完整尾声；处理阶段零分配检查通过。
- 新的审计后端确认 512 帧调用后恰有一批未消费 GPU 工作，0 帧调用不消费它，1 帧 FIFO 读取会消费它；
  后续提交不会覆盖未完成存储。验证了 configure、clear_generation、reset、EOS、释放及完成错误传播。
  注入完成失败时，不把失败输出写入调用者缓冲；reset 在成功完成旧批次后丢弃旧 epoch 的 FIFO。
- Windows 原生解析延迟 FIR 检查通过：1／32 源、三个 epoch、静音／空任务、历史保留和重置；
  完成在独立主机线程执行，重复 epoch 逐位一致。最大相对 L2 **3.263328e-7**，
  最大误差／参考峰值 **2.980232e-7**。销毁尚有提交批次的实例后，插件资源计数全部归零。
  [原生记录](evidence/room-cuda-deferred-wait/native-correctness.json)。
- clippy（warnings denied）、C++ 格式、适用的 clang-tidy／cppcheck 和 Windows 原生构建通过。
  Windows 专属 CUDA 源码由 MSVC 构建验证，没有声称通过 macOS 的 CUDA 编译或驱动内部零分配认证。

**没有执行完整 Windows 数值回放、六轮长测、macOS 性能扩展、音频设备播放或 GUI 打包。**
短测失败后未继续这些阶段，原生组件／本地参考结果不能代替完整同机 PCM 验收。
也没有把本轮结果写成 exact 的新逐位基线认证。

## 复现

这些步骤用于检查归档实现或形成新候选，不应重跑未修改的失败候选以挑选通过轮次。
在匹配 `e14fe43c` 的源码上依次应用两个补丁；无需 MathDx／cuFFTDx：

```sh
git apply --check docs/architecture/evidence/room-cuda-integration/candidate.patch
git apply docs/architecture/evidence/room-cuda-integration/candidate.patch
git apply --check docs/architecture/evidence/room-cuda-deferred-wait/deferred-wait.patch
git apply docs/architecture/evidence/room-cuda-deferred-wait/deferred-wait.patch
```

使用当前保留的 `resident_transport_test.py`。沿用既有 MSVC／Ninja、vendored FLAC/Opus、SOFA/IAMF
关闭的 canonical 环境和 CUDA 12.0；`$CargoCache` 为既有 Cargo 缓存，`$FrozenDir` 为新的同卷小目录：

```powershell
cmake -S . -B build/win-canon -DCMAKE_BUILD_TYPE=Release `
  -DMR_ADM_ENABLE_ROOM_CUDA=ON -DMR_ADM_CONSISTENCY_DIAGNOSTICS=OFF
cmake --build build/win-canon --target mr_adm_room_cuda
$env:RUSTFLAGS = '-Crelocation-model=pic'
$env:CARGO_INCREMENTAL = '0'
cargo build --manifest-path rust/Cargo.toml --release --locked -p mradm-room-cuda `
  --example room_cuda_bench --example room_cuda_replay --target x86_64-pc-windows-msvc `
  --target-dir $CargoCache
New-Item -ItemType Directory $FrozenDir -ErrorAction Stop | Out-Null
Copy-Item build/win-canon/mradm_room_cuda.dll $FrozenDir
Copy-Item "$CargoCache/x86_64-pc-windows-msvc/release/examples/room_cuda_bench.exe" $FrozenDir
Copy-Item "$CargoCache/x86_64-pc-windows-msvc/release/examples/room_cuda_replay.exe" $FrozenDir
foreach ($pattern in @('cudart64*.dll', 'cufft64*.dll', 'nvrtc64*.dll', 'nvrtc-builtins64*.dll')) {
    Get-ChildItem build/win-canon -Filter $pattern | ForEach-Object {
        New-Item -ItemType HardLink -Path (Join-Path $FrozenDir $_.Name) -Target $_.FullName | Out-Null
    }
}
$FrozenDll = Join-Path $FrozenDir 'mradm_room_cuda.dll'
$Benchmark = Join-Path $FrozenDir 'room_cuda_bench.exe'
python scripts/research/room_gpu/resident_transport_test.py --library $FrozenDll --cross-thread
$env:MR_ADM_ROOM_WORKERS = '8'
# 基线省略 --cuda-deferred-wait；保存各次独立 JSON，不混合分位数。
& $Benchmark $Room96 1024 512 32 --hrir $Collection --quality realtime `
  --scenario ramps --audio-workers --cuda-library $FrozenDll
foreach ($scenario in @('ramps', 'distance-ramps')) {
    $raw = & $Benchmark $Room96 1024 512 32 --hrir $Collection --quality realtime `
        --scenario $scenario --audio-workers --cuda-library $FrozenDll --cuda-deferred-wait
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark failed' }
    $raw | Set-Content -Encoding utf8 (Join-Path $NewEvidence "$scenario.json")
    $result = $raw | ConvertFrom-Json
    if ($result.profiling -or $result.blocks -ne 1024 -or $result.sample_rate -ne 96000 -or `
        $result.call_frames -ne 512 -or $result.sources -ne 32 -or `
        -not $result.experiment.deferred_wait) { throw 'Unexpected configuration' }
    if ($result.deadline_misses -ne 0) { throw 'Short gate failed; stop this candidate' }
}
```

`$Room96` 与 `$Collection` 使用同批经过认证的资源；先建立 `$NewEvidence` 新目录。
冻结二进制哈希见 [binaries.json](evidence/room-cuda-deferred-wait/binaries.json)，实际执行顺序见 short.json。
任何新候选仍须先过两个短测及完整同机数值门槛，再进入 6 × 22,500 次长测；本候选没有进入该阶段。

## 恢复

本地实验运行时及 Rust 测试随补丁归档，原有未提交研究改动保持原样。
仅保留原生回归工具的跨线程完成及未完成批次释放检查。Windows 恢复原源码、HEAD 和改动状态，
关闭本轮 CMake CUDA 入口并重建 canonical CPU CLI／C API；删除本轮私有资源与临时输出，保留共享缓存。
逐项恢复与清理结果记录于 validation.json 的 `restoration`。

后续 [1 ms 目标与关键路径实验](ROOM_CUDA_CRITICAL_PATH.md) 根据 CPU／GPU 时间线重新组合优化，
最佳短测平均改善 0.654／0.728 ms，但仍未达到 1 ms 目标；GPU 候选继续归档。
