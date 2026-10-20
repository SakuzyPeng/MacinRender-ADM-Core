# ROOM CUDA：1 ms 目标与关键路径实验

日期：2026-10-19（用户本地日期）。基线提交 `28f3dc0b`，恢复归档的内存资源绑定后端进行研究。
**本轮取得了较明显的短测改善，但没有达到两个场景平均各减少 1 ms 的目标。**
最佳组合的位置／距离均值分别降低 **0.654／0.728 ms（22.7%／26.2%）**，两轮配对 p99 均改善，
共 **4096 次调用零超时**。因此未进入完整 Windows 数值回放和六轮长测，也没有正式采用 GPU 组合。

进一步的 GPU 晚场双耳解码原型只在首组位置对照中减少 **0.336 ms**，继续扩测停止。
它没有达到收益门槛；该次 1024 调用没有超时。源码、原始结果和未达到目标的结论均保留。

活动源码仅保留不改变浮点结果的整数环索引简化，以及新增的逐位回归测试。
[完整验证记录](evidence/room-cuda-critical-path/validation.json)、
[组合 V2 补丁](evidence/room-cuda-critical-path/combined-v2.patch)、
[晚场 V3 增量](evidence/room-cuda-critical-path/decoder-v3.patch) 和
[保留的整数改动](evidence/room-cuda-critical-path/integer-rings.patch) 分开保存。

## 时间线改变了实施顺序

使用测试机已有的 Nsight Systems 2022.4.2.18，采集 CUDA API、GPU kernel／传输、NVTX 阶段和
CPU 上下文切换。没有安装工具、更新驱动或改动功耗配置。诊断从 32 次预热后开始，正式性能二进制
没有这些钩子，也没有开启 `room-profile`／`room-trace`／`ROOM_GPU_TRACE`。

早期在进程内结束 capture 的路径出现处理／退出错误；一个任务自己的残留进程已终止。
随后改成正常进程退出时结束采集，距离运动 512 调用完整执行，退出码为 0，独立 JSON 与 SQLite
同时保存。失败尝试单独留作工具诊断，未作验收证据，也没有停止用户应用。

该成功诊断的每次 512 帧观察值如下，**不能当成无插桩耗时，也不能把嵌套／并行阶段相加**：

| 观察项 | 每次调用 ms | 含义 |
|---|---:|---|
| 声源批次 | 1.4163 | 包含 GPU 提交及等待声源 worker |
| GPU 提交 | 0.4947 | 声源批次内的嵌套区间 |
| CPU 晚场及双耳解码 | 1.1342 | 与主 GPU 批次重叠 |
| GPU 响应合成 kernel | 0.6332 | GPU 活动区间，仍可能受调度影响 |
| CPU 晚场结束晚于 GPU 输出回传结束的部分 | 0.1153 | 各批次正时间差的均值，折算到每次调用 |

最后一行通过同一批次的 CPU 解码结束时间与 GPU D2H 结束时间比较得到，**不是移植后的预计收益**。
这说明不能把约 1.13 ms 的 CPU 晚场时间全部计为可移除的串行工作。
据此先优化输入准备和响应访存，再实际验证扩大 GPU 常驻范围。
[时间线汇总](evidence/room-cuda-critical-path/timeline-analysis.json) 与
[诊断程序结果](evidence/room-cuda-critical-path/distance-exit-result.json) 保留；原始 profiler 文件留在私有目录。
Nsight 区分 API、排队和 GPU 执行区间的依据见 [官方分析说明](https://docs.nvidia.com/nsight-systems/AnalysisGuide/)。

## V2：输入准备与响应计算组合

1. **位置求值只在实际需要时执行。** 仅用于 GPU 流水中只有位置在变化、其他控制稳定且没有显式
   屏幕映射的输入段。元数据取样点和段尾仍使用原绝对样本位置的 f64 公式；其余样本保留原音频、
   gain、diffuse 和 headLocked 历史操作。遇到原点时反向检查被跳过的样本，保留最后非零方向。
   不改变更新频率、事件位置、有效位 clear、FIFO 或尾声，其他情况走原路径。
2. **整数环索引简化。** diffuse 的私有环长只可能为 32／64，用位掩码代替取模；headLocked 的
   有界历史索引用加一和边界复位代替取模。浮点表达式、抽头顺序和所有样本位置保持原样。
3. **目标 FFT 融合和紧凑上传。** 复用此前经过组件数值验证的目标打包／512 点 R2C／安装融合，
   以及七组小数组打包、路径描述符单独上传的方案。没有启用批次缩紧、卷积 FFT 融合或延后等待。
4. **响应按频率块调度。** 调换响应 kernel 的二维 grid 映射，让相邻 block 在同一频率块处理不同
   响应行，尝试复用 HRIR 数据。每个输出频点内部的运算和归约顺序保持原样，未减少任何路径或距离层。

所有变换尺寸、完整 FIR、时序、淡化、精度层次和 256 帧延迟保持原约束。GPU 仍为 FP32，
CPU 几何保持 f64。MathDx 24.08／cuFFTDx 1.2.1 只在临时目录使用，没有加入仓库或全局安装。

三个配置分别为：基线（私有能力位 0）、输入与整数优化＋融合／上传（6）、再加频率块调度（22）。
V1 EXE 作为冻结的原整数索引基线，V2 EXE 包含整数简化；两个候选共用同一 V2 EXE。
基线关闭 `--cuda-compact-control`，候选开启。DLL 与 EXE 的身份独立核对。

## Windows 短测

i7-11800H／RTX 3060 Laptop 6 GiB，驱动 610.88、CUDA 12.0；8 个 Audio MMCSS DSP worker，
普通调用线程，96 kHz、512 帧、32 源、realtime、无插桩 Release。正常桌面、AC 供电。
计时期间没有本任务的并行构建、profiler 或另一基准。每项 32 次预热后测 1024 次，两轮交替顺序。

“均值”是两轮均值的平均；p99 保留两轮各自的结果，没有构造合并分位数。

| 配置 | 位置均值 ms | 位置 p99，两轮 ms | 位置最大 ms | 距离均值 ms | 距离 p99，两轮 ms | 距离最大 ms |
|---|---:|---|---:|---:|---|---:|
| V1 基线 | 2.881359 | 3.2724 / 3.5212 | 3.8838 | 2.781343 | 3.5077 / 3.1217 | **5.7803** |
| V2 输入／整数＋融合／上传 | 2.457847 | 2.8161 / 3.1391 | 3.5775 | 2.343133 | 2.6374 / 2.9270 | 3.2543 |
| V2 再加频率块调度 | **2.227378** | **2.8247 / 2.6962** | **3.1850** | **2.053821** | **2.6190 / 2.7130** | **3.0726** |

两个候选各 4096 次、零超时。基线第一轮距离出现一次 5.7803 ms 超时，照常保留，未作废该轮。
最佳候选分别减少 0.653980／0.727522 ms，仍低于预先设定的 1 ms 门槛。
没有将较小的平均收益或短测零超时写成长测通过。
[逐轮结果、命令、供电／负载快照](evidence/room-cuda-critical-path/short-v2.json)。

最佳候选峰值工作集 **1,709,850,624 字节**，基线最高 **1,479,102,464 字节**，均包含准备阶段。
进程墙钟减去被计时 DSP 的其余开销约增加 6 秒，不能当成独立冷加载计时；未单独测进程显存峰值。
没有把该组合的全部收益归给某个单项，尤其没有独立量出保留的整数改动的收益。

## V3：GPU 晚场双耳解码扩展

在 V2 最快组合上增加独立的 GPU 世界方向解码批次，共享现有认证 HRIR bank。
CPU 保留方向／头姿几何与原世界场合并，GPU 执行 HRIR 响应、目标准备和完整动态卷积，
最后按原方向顺序求和。用单独控制历史、有限淡化和 reset 标记保持时序；不用额外音频延迟。
未迁移全部 EQ 或整个音频链，不能把这个原型称为完成了全链 GPU 常驻。

该实验显式要求共享的 4096 点 HRIR bank 和最多 32 个世界方向；默认入口不选择它。
私有目标更新容量扩至 512，容纳各方向的合法活动目标；未放宽控制更新率或截短支持长度。

先执行一组位置配对，以 1 ms 作为继续条件：

| 配置 | 调用数 | 均值 ms | p99 ms | 最大 ms | 超时 |
|---|---:|---:|---:|---:|---:|
| 冻结 V1 基线 | 1024 | 2.789881 | 3.2765 | 3.7942 | 0 |
| V3 GPU 晚场解码 | 1024 | 2.454068 | 3.0057 | 3.3719 | 0 |

只减少 **0.335813 ms**，因此距离运动及后续重复未执行。
V3 的工作集峰值为 **1,848,963,072 字节**。原始 collector 的 `deadline_passed=false` 同时要求
四项短测全部完成，在这里表示未完成完整短矩阵，不能解释为出现了超时。
[V3 结果](evidence/room-cuda-critical-path/short-v3.json)。
本轮没有给 V3 再做内层插桩，不能把收益不足归因于某个具体同步或 GPU 调度事件。

## 正确性、保留与停止范围

- V2／V3 各通过本地 15 项 GPU 参考、准备资源、零分配和质量测试；覆盖 48／96 kHz、运动、
  原点历史、clear、屏幕、headLocked、分包、reset、换房和完整尾声。新增控制优化用例与原 GPU
  参考路径逐位比较，包括接近原点的长区间和不同分包。
- 四份 DLL 的 Windows 原生解析 FIR 回归通过：1／32 源、三个 epoch、静音／空任务、历史、
  reset 和带未完成批次的释放；插件资源计数归零。这是组件验证，不替代完整原生会话比较。
- 整数优化通过 macOS 和 Windows 的 48／96 kHz 直接抽头逐位对照；回退实验代码后，本地再通过
  19 项相关测试。浮点抽头顺序和表达式未改变，公开 C ABI、布局和质量枚举未改。
- clippy warnings denied、C++ 格式、适用质量检查、原生 MSVC／Rust Release 构建通过。
  不声称 CUDA 驱动内部零分配或 macOS CUDA 后端通过。

**没有执行完整 Windows 候选数值回放、六轮长测、设备播放、GUI 打包或 macOS 性能矩阵。**
停止由 1 ms 收益门槛触发。没有新增 exact 全流程相对 `718a804` 的认证声明。

最终只保留 `diffuse.rs` 与 `session.rs` 的整数索引简化，以及 `binaural_convolution.rs` 的逐位测试。
位置快照优化、频率块调度、融合／上传组合和 GPU 晚场解码全部归档。
原有及其他会话产生的研究文件保持不变；GPU 的旧通过记录不转记到本次产物。

## 复现

在匹配 `28f3dc0b` 的工作区，按下列顺序构建冻结参考和候选。复用 canonical MSVC/Ninja、
Cargo／FetchContent 缓存；不要新建大型构建树。以下补丁针对研究入口，当前活动源码没有应用 GPU 补丁。

```sh
git apply docs/architecture/evidence/room-cuda-integration/candidate.patch
git apply docs/architecture/evidence/room-cuda-critical-path/combined-v2.patch
# 此时尚未应用 integer-rings.patch，构建 V1 参考。
```

沿用 [接入构建配置](ROOM_CUDA_INTEGRATION.md#构建与复现) 的 vendored FLAC/Opus、SOFA/IAMF 关闭设置。
从 [NVIDIA 官方归档](https://developer.nvidia.com/downloads/assets/gameworks/downloads/secure/cufftdx/nvidia-mathdx-24.08.0.zip)
获取 MathDx 24.08；校验值见 [mathdx.json](evidence/room-cuda-critical-path/mathdx.json)。
`$MathDxInclude` 指向 `nvidia/mathdx/24.08/include`，测试期间保留该路径供 NVRTC 读取。

```powershell
function Save-RoomBuild($Directory) {
    New-Item -ItemType Directory $Directory -ErrorAction Stop | Out-Null
    Copy-Item "$CargoCache/x86_64-pc-windows-msvc/release/examples/room_cuda_bench.exe" $Directory
    Copy-Item "$CargoCache/x86_64-pc-windows-msvc/release/examples/room_cuda_replay.exe" $Directory
    Copy-Item build/win-canon/mradm_room_cuda.dll $Directory
    foreach ($pattern in @('cudart64*.dll', 'cufft64*.dll', 'nvrtc64*.dll', 'nvrtc-builtins64*.dll')) {
        Get-ChildItem build/win-canon -Filter $pattern | ForEach-Object {
            New-Item -ItemType HardLink -Path (Join-Path $Directory $_.Name) -Target $_.FullName | Out-Null
        }
    }
    Get-FileHash "$Directory/room_cuda_bench.exe", "$Directory/room_cuda_replay.exe", `
        "$Directory/mradm_room_cuda.dll" | ConvertTo-Json | Set-Content "$Directory/hashes.json"
}
cmake -S . -B build/win-canon -DCMAKE_BUILD_TYPE=Release `
  -DMR_ADM_ENABLE_ROOM_CUDA=ON -DMR_ADM_CONSISTENCY_DIAGNOSTICS=OFF `
  -DMR_ADM_ROOM_CUDA_FUSE_TARGET=OFF -DMR_ADM_ROOM_CUDA_FUSE_CONVOLVE=OFF `
  -DMR_ADM_ROOM_CUDA_COMPACT_UPLOAD=OFF -DMR_ADM_ROOM_CUDA_FREQUENCY_MAJOR=OFF `
  "-DMR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR=$MathDxInclude"
cmake --build build/win-canon --target mr_adm_room_cuda
$env:RUSTFLAGS = '-Crelocation-model=pic'
$env:CARGO_INCREMENTAL = '0'
cargo build --manifest-path rust/Cargo.toml --release --locked -p mradm-room-cuda `
  --example room_cuda_bench --example room_cuda_replay --target x86_64-pc-windows-msvc `
  --target-dir $CargoCache
Save-RoomBuild $BaselineDir
git apply docs/architecture/evidence/room-cuda-critical-path/integer-rings.patch
cargo build --manifest-path rust/Cargo.toml --release --locked -p mradm-room-cuda `
  --example room_cuda_bench --example room_cuda_replay --target x86_64-pc-windows-msvc `
  --target-dir $CargoCache
cmake -S . -B build/win-canon -DMR_ADM_ROOM_CUDA_FUSE_TARGET=ON `
  -DMR_ADM_ROOM_CUDA_COMPACT_UPLOAD=ON -DMR_ADM_ROOM_CUDA_FREQUENCY_MAJOR=ON
cmake --build build/win-canon --target mr_adm_room_cuda
Save-RoomBuild $CandidateDir
# 关闭 FREQUENCY_MAJOR 并单独冻结 DLL，则得到能力位 6 的对照。
python scripts/research/room_gpu/resident_transport_test.py --library "$CandidateDir/mradm_room_cuda.dll" `
  --kernels scripts/research/room_gpu/session_kernels.cu
$env:MR_ADM_ROOM_WORKERS = '8'
& "$BaselineDir/room_cuda_bench.exe" $Room96 1024 512 32 --hrir $Collection --quality realtime `
  --scenario ramps --audio-workers --cuda-library "$BaselineDir/mradm_room_cuda.dll"
& "$CandidateDir/room_cuda_bench.exe" $Room96 1024 512 32 --hrir $Collection --quality realtime `
  --scenario ramps --audio-workers --cuda-library "$CandidateDir/mradm_room_cuda.dll" --cuda-compact-control
# 上面为单项调用示例。正式配对使用下面的循环，输出目录必须是新目录。
New-Item -ItemType Directory $NewEvidence -ErrorAction Stop | Out-Null
foreach ($round in 1..2) {
    foreach ($scenario in @('ramps', 'distance-ramps')) {
        $order = if (($round -eq 1) -eq ($scenario -eq 'ramps')) {
            @('baseline', 'candidate')
        } else { @('candidate', 'baseline') }
        foreach ($name in $order) {
            $directory = if ($name -eq 'baseline') { $BaselineDir } else { $CandidateDir }
            $extra = if ($name -eq 'candidate') { @('--cuda-compact-control') } else { @() }
            $raw = & "$directory/room_cuda_bench.exe" $Room96 1024 512 32 --hrir $Collection `
                --quality realtime --scenario $scenario --audio-workers `
                --cuda-library "$directory/mradm_room_cuda.dll" @extra
            if ($LASTEXITCODE -ne 0) { throw 'Benchmark failed' }
            $raw | Set-Content -Encoding utf8 (Join-Path $NewEvidence "$round-$scenario-$name.json")
            $result = $raw | ConvertFrom-Json
            if ($result.profiling -or $result.blocks -ne 1024 -or $result.sample_rate -ne 96000 -or `
                $result.call_frames -ne 512 -or $result.sources -ne 32) { throw 'Unexpected configuration' }
            if ($name -eq 'candidate' -and $result.deadline_misses -ne 0) {
                throw 'Candidate deadline missed; stop'
            }
        }
    }
}
```

只有形成新的候选时才继续试验；任何超时立即停止对应候选。两场景均值各降低至少 1 ms、
配对 p99 改善和零超时短测是本轮进入数值扩展／长测的条件，不以阶段累计时间代替整体收益。
研究 V3 时再应用 `decoder-v3.patch`，重建并独立冻结 EXE／DLL，追加 `--cuda-resident-decoder`。
V3 首组位置对照未达到 1 ms，按该条件停止，没有继续拼接其他候选的成功调用。

诊断补丁 `diagnostic.patch` 只在接入 prerequisite 上应用，并单独编译 `room_cuda_bench` 的
`room-trace` 版本；`timeline.cpp` 使用已有 CUDA／NVTX3 头文件生成临时 helper DLL。
设置 `MR_ROOM_DIAGNOSTIC_JSON`，使用保存的
[采集命令](evidence/room-cuda-critical-path/distance-exit-command.json)。不把诊断构建用于验收。

源码／二进制哈希、复原顺序见 [source-provenance.json](evidence/room-cuda-critical-path/source-provenance.json)。
Windows 已恢复任务前的源码、HEAD 和改动状态，并重新构建 canonical CPU CLI／C API；
本轮私有资源、临时 SDK 和追踪工作目录清理，保留共享缓存。逐项记录见 validation.json 的 `restoration`。
