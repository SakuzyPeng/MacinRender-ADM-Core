# ROOM CUDA 512 点 FFT 融合实验

日期：2026-10-19。基线为 `a08245a6` 上恢复的内存资源绑定候选，保留上下文失效保护。
本轮不启用前轮的批次缩紧或上传合并，分别测试目标生成融合、卷积融合与两项组合。
**目标生成融合短测平均耗时降低约 6%，但正式长测未通过：112,500 次调用中有 1 次超时，最大 7.1111 ms。**
第五轮失败后停止最后一轮，没有重跑替换结果；运行时代码已回退，保留经过原生验证的回归工具。
所有 GPU 数学仍为 FP32；CPU 几何和原始准备数据保留原精度，exact 与默认路径不选择实验后端。

[完整验证记录](evidence/room-cuda-fft-fusion/validation.json)、
[融合增量补丁](evidence/room-cuda-fft-fusion/fft-fusion.patch) 和
[源码版本与哈希](evidence/room-cuda-fft-fusion/source-provenance.json) 一并归档。

## 实现

使用 NVIDIA MathDx 24.08 / cuFFTDx 1.2.1，沿用 CUDA 12.0 的 NVRTC 编译，不更换驱动或工具链。
库来自官方归档，未把 SDK 头文件加入仓库，也未建立新的大型项目缓存。
定义固定 512 点、`Precision<float>`、自然频谱布局、folded 实数模式、每线程 8 元素、每块 4 个 FFT，
静态检查没有额外 workspace。NVRTC 保留 FMA、关闭 FTZ、保留精确除法／平方根；没有 FP16 或 TF32。
[cuFFTDx 的融合能力与要求](https://docs.nvidia.com/cuda/cufftdx/1.2.0/requirements_func.html) 和
[NVRTC 执行接口](https://docs.nvidia.com/cuda/cufftdx/1.2.0/examples.html#nvrtc-examples) 是实现依据。

- **目标生成融合：** 在同一 kernel 中按原桶顺序收集完整 FIR 分区、补零、执行 512 点 R2C，
  然后直接写入原滤波器物理槽。省去中间时域窗口、新频谱缓冲和对应 cuFFT 计划；没有截短有效 taps。
- **卷积融合：** 保留原分区乘加顺序，在同一 kernel 中执行 C2R 并写出原有效后半块。
  原 `session_mix` 继续执行 `1/512` 缩放和有限淡化，省去中间频谱缓冲及对应 cuFFT 计划。
- 部分有效 FFT 组仍参与块内同步；只有整个 block 无工作时才提前返回。DC／Nyquist 虚部清零，
  不改变更新样本位置、完整 FIR、FIFO、尾声或 256 帧延迟。所有计划和图在准备阶段创建。
- cuFFTDx 的默认 CUTLASS 类型依赖被显式禁用；本实现只用 FP32 复数类型。原有 GPU 内核文本保持原样，
  融合代码位于独立编译开关下。

## 原生正确性与短测

新增原生延迟 FIR 回归对照独立的解析结果，覆盖 1／32 源、三个 epoch、静音、空任务／零声源批次、
历史保留、重置、双耳不同延迟与增益。基线及三个融合配置均通过，重复 epoch 逐位一致，全部输出有限，
释放后插件计数归零；最大相对 L2 与峰值相对误差均低于 4e-7。

Windows 固定八个 Audio MMCSS DSP worker，调用线程普通优先级，96 kHz、512 帧、32 源、realtime。
硬件为 i7-11800H／RTX 3060 Laptop 6 GiB，驱动 610.88；保持正常桌面后台负载、接通电源，
本任务计时期间没有并行构建或另一基准，没有关闭用户应用或更改驱动、功耗设置。
使用相同资源、事件和无插桩 Release；两轮交替顺序，每项 32 次预热后测 1024 次调用。
所有短测均零超时。表中均值为两轮均值的平均，p99 分别列出各轮结果，没有构造合并分位数。

| 配置 | 位置均值 ms | 位置 p99，两轮 ms | 位置最大 ms | 距离均值 ms | 距离 p99，两轮 ms | 距离最大 ms |
|---|---:|---|---:|---:|---|---:|
| 基线 | 2.819628 | 3.0854 / 3.3162 | 3.6024 | 2.643165 | 3.0526 / 3.0226 | 3.6458 |
| 目标生成融合 | 2.649141 | 3.0463 / 3.1434 | 4.1043 | 2.484716 | 2.8855 / 2.8216 | 3.5394 |
| 卷积融合 | 2.755944 | 3.1403 / 3.1504 | 3.5691 | 2.644290 | 3.0355 / 3.1198 | 3.9156 |
| 两项组合 | 2.691643 | 3.1031 / 3.0989 | 3.5451 | 2.482485 | 2.9115 / 2.8552 | 3.2925 |

长测前选择了**仅目标生成融合**：位置／距离均值改善约 6.0%／6.0%，两轮各自的均值及 p99 均改善。
卷积融合没有一致的额外整体收益，没有因为另一配置偶然较低的最大值改选组合。
这只是本轮短对照结果，不证明长期尖峰已消除。
逐轮结果、命令及主机快照见 [short.json](evidence/room-cuda-fft-fusion/short.json)，
事先选择记录见 [selection.json](evidence/room-cuda-fft-fusion/selection.json)。

代价也保留：目标候选短测的峰值工作集约 1.709 GB，基线约 1.478 GB；按进程墙钟减去被计时处理时间，
其余启动、准备和销毁等开销增加约 6 秒。该差值不是单独的冷加载计时，工作集也不是独立显存测量。
融合减少设备中间缓冲，同时引入了运行时 FFT 代码编译，不能把这称为加载准备的全面加速。

## 完整数值与长测

仅目标生成融合与同机 CPU realtime 的默认／世界坐标回放各输出 231,581 帧，包含完整尾声。
固定／碎片分包一致，事件时刻、帧数和延迟一致；没有时间对齐搜索、增益拟合或裁剪。

| 回放 | 最大相对 L2 | 最大误差／参考峰值 |
|---|---:|---:|
| 默认，含 headLocked | 4.011806e-7 | 5.430095e-7 |
| 世界坐标 | 4.259707e-7 | 5.292038e-7 |

两项低于 `2e-5`、`2e-4`。本地 14 项 GPU 参考、准备资源、质量及零分配测试通过，
clippy、C++ 格式、适用质量检查和四份原生构建通过。没有认证 CUDA 驱动内部零分配。

冻结仅目标生成融合的同一份 DLL 和基准程序，每轮 22,500 次调用对应 120 秒音频，不计前 32 次预热。
穿插的 1024 次基线短对照不计入候选长测调用数，也不能替代候选验收。

| 顺序 | 场景 | 调用数 | 均值 ms | p99 ms | 最大 ms | 超时 |
|---|---|---:|---:|---:|---:|---:|
| 1 | 位置运动 | 22500 | 2.621729 | 3.0797 | 4.1163 | 0 |
| 2 | 距离运动 | 22500 | 2.501796 | 2.9177 | 3.7787 | 0 |
| 3 | 位置运动 | 22500 | 2.615686 | 3.0707 | 3.9430 | 0 |
| 4 | 距离运动 | 22500 | 2.507377 | 2.9307 | 4.9833 | 0 |
| 5 | 位置运动 | 22500 | 2.666464 | 3.1386 | **7.1111** | **1** |

失败发生在第五轮 `measured_call=815`（含预热的 `benchmark_call=847`），最后一轮距离运动未执行。
累计 **112,500 次、1 次超时**，未达到每次不超过 **5.333333 ms** 的要求。
五轮候选峰值工作集为 **1,709,256,704 字节**，包含准备阶段；不是独立显存测量。
无插桩结果不能把尖峰归因于某个 FFT、驱动或后台进程。没有因正常后台波动作废失败，也没有继续设备播放、
GUI 打包或 macOS 性能扩展。

[long.json](evidence/room-cuda-fft-fusion/long.json) 保留全部长测及穿插基线对照；
[numerical.json](evidence/room-cuda-fft-fusion/numerical.json) 保留完整回放比较；
[native-correctness.json](evidence/room-cuda-fft-fusion/native-correctness.json) 保留四种配置的解析 FIR 检查。

## 复现与恢复

以下命令用于复现归档实现或形成新候选，不应对未修改的失败候选重跑长测寻找通过轮次。
在匹配 `a08245a6` 的工作区依次应用接入候选及本轮增量；不应用前轮批次／上传补丁：

```sh
git apply --check docs/architecture/evidence/room-cuda-integration/candidate.patch
git apply docs/architecture/evidence/room-cuda-integration/candidate.patch
git apply --check docs/architecture/evidence/room-cuda-fft-fusion/fft-fusion.patch
git apply docs/architecture/evidence/room-cuda-fft-fusion/fft-fusion.patch
```

从 [NVIDIA 官方归档](https://developer.nvidia.com/downloads/assets/gameworks/downloads/secure/cufftdx/nvidia-mathdx-24.08.0.zip)
获取 MathDx 24.08，解压到临时私有目录。归档长度 **24,937,758 字节**，SHA-256 为
`1af09c585066c74fe17ea0f6bfcacbdff2eea1bc5120f8701933a639896d5dac`，另存于
[mathdx.json](evidence/room-cuda-fft-fusion/mathdx.json)。`$MathDxInclude` 指向包含 `cufftdx.hpp` 的
`nvidia/mathdx/24.08/include`，版本头为 `CUFFTDX_VERSION=1201`。
该绝对路径会编入研究 DLL，NVRTC 在运行时读取这些头文件，因此必须保留到测试结束。
本轮没有制作可分发 CUDA 包，不能把这样的研究 DLL 当作独立部署产物。

沿用已建立的 Windows canonical MSVC/Ninja 环境、CUDA 12.0、vendored FLAC/Opus、SOFA/IAMF 关闭配置，
复用 `build/win-canon` 和既有 Cargo／FetchContent 缓存。按下表逐项配置、构建并冻结 DLL，
共用一份 Rust Release 基准／回放程序；禁止测量期间重新构建。

| 版本 | `MR_ADM_ROOM_CUDA_FUSE_TARGET` | `MR_ADM_ROOM_CUDA_FUSE_CONVOLVE` | 私有能力位 |
|---|---|---|---:|
| 基线 | OFF | OFF | 0 |
| 目标生成 | ON | OFF | 4 |
| 卷积 | OFF | ON | 8 |
| 组合 | ON | ON | 12 |

以下以目标生成为例，`$FrozenDir` 是同卷新建的小型输出目录；其余版本替换两个开关和目录。
运行库使用硬链接复用 canonical 产物，不复制四份大型依赖：

```powershell
cmake -S . -B build/win-canon -DCMAKE_BUILD_TYPE=Release `
  -DMR_ADM_ENABLE_ROOM_CUDA=ON -DMR_ADM_CONSISTENCY_DIAGNOSTICS=OFF `
  -DMR_ADM_ROOM_CUDA_FUSE_TARGET=ON -DMR_ADM_ROOM_CUDA_FUSE_CONVOLVE=OFF `
  "-DMR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR=$MathDxInclude"
cmake --build build/win-canon --target mr_adm_room_cuda
New-Item -ItemType Directory $FrozenDir -ErrorAction Stop | Out-Null
Copy-Item build/win-canon/mradm_room_cuda.dll $FrozenDir
foreach ($pattern in @('cudart64*.dll', 'cufft64*.dll', 'nvrtc64*.dll', 'nvrtc-builtins64*.dll')) {
    Get-ChildItem build/win-canon -Filter $pattern | ForEach-Object {
        New-Item -ItemType HardLink -Path (Join-Path $FrozenDir $_.Name) -Target $_.FullName | Out-Null
    }
}
$env:RUSTFLAGS = '-Crelocation-model=pic'
$env:CARGO_INCREMENTAL = '0'
cargo build --manifest-path rust/Cargo.toml --release --locked -p mradm-room-cuda `
  --example room_cuda_bench --example room_cuda_replay --target x86_64-pc-windows-msvc `
  --target-dir $CargoCache
# 将两个 example exe 冻结一次；$Benchmark、$Replay 指向它们，所有版本共用。
$FrozenDll = Join-Path $FrozenDir 'mradm_room_cuda.dll'
python scripts/research/room_gpu/resident_transport_test.py --library $FrozenDll `
  --kernels scripts/research/room_gpu/session_kernels.cu
$env:MR_ADM_ROOM_WORKERS = '8'
& $Benchmark $Room96 1024 512 32 --hrir $Collection --quality realtime `
  --scenario ramps --audio-workers --cuda-library $FrozenDll
& $Benchmark $Room96 1024 512 32 --hrir $Collection --quality realtime `
  --scenario distance-ramps --audio-workers --cuda-library $FrozenDll
```

检查 DLL 的私有 `mradm_room_cuda_optimizations()` 返回位与表一致，冻结 SHA-256 后再计时。
两轮交替短对照的实际命令／顺序、四份 DLL 哈希及两个 exe 哈希分别见 short.json、
[variants.json](evidence/room-cuda-fft-fusion/variants.json) 和
[binaries.json](evidence/room-cuda-fft-fusion/binaries.json)。

完整数值回放使用同机 CPU realtime 参考；`$CpuOutput`、`$GpuOutput` 必须为新的输出目录。
分别运行默认和追加 `--world-only` 的世界坐标模式，每种模式使用独立目录：

```powershell
& $Replay $Room96 $CpuOutput --hrir $Collection --quality realtime --dense --screens --head-motion wrap
& $Replay $Room96 $GpuOutput --hrir $Collection --quality realtime --dense --screens --head-motion wrap `
  --cuda-library $FrozenDll
```

对 GPU manifest 严格核对 `experiment` 为下列字典后，将仅移除该字段的副本写在同一输出目录，
再调用 `scripts/room/compare-realtime.py`；不改原 manifest 或其余元数据／PCM。
以下 Python 片段的两个参数依次为 CPU 和 GPU 输出目录：

```python
import json, subprocess, sys
from pathlib import Path
reference, candidate = map(Path, sys.argv[1:])
manifest = json.loads((candidate / 'manifest.json').read_text())
assert manifest.pop('experiment') == {
    'kind': 'room-cuda-session', 'cpu_reference': False, 'control': 'production Session',
    'finite_fade': True, 'binding': 'prepared-memory', 'eq_stagger_frames': 256,
}
comparison = candidate / 'comparison-manifest.json'
comparison.write_text(json.dumps(manifest))
subprocess.run([sys.executable, 'scripts/room/compare-realtime.py',
                str(reference / 'manifest.json'), str(comparison), '--output',
                str(candidate / 'numerical.json')], check=True)
```

只有新候选的两项短测和完整数值都通过，才把调用数改为 `22500`，位置／距离各三轮。
核对输出中的 `profiling=false`、96 kHz／512／32、八个 Audio worker、普通调用线程及
`experiment.binding=prepared-memory`；每轮有任一 `deadline_misses` 就停止剩余轮次。
本轮在选择 target 后没有切换到其他融合配置，也没有拼接历史成功轮次。

本地已恢复所有实验运行时代码，保留原有未提交研究内容；仅新增经过验证的
`scripts/research/room_gpu/resident_transport_test.py`。Windows 源码、HEAD 和原始改动状态逐项恢复，
清除本轮 CUDA CMake 选项并重建 canonical CPU CLI／C API。临时私有资源、输出和 MathDx SDK 已删除，
既有共享构建缓存保留；恢复记录见 validation.json 的 `restoration`。
融合补丁可供后续研究，但本轮不纳入正式运行路径。

后续独立测试了 [延后 GPU 等待至 FIFO 消费](ROOM_CUDA_DEFERRED_WAIT.md)，保持 256 帧延迟，
没有叠加本页 FFT 融合。该候选距离运动短测 1/1024 次超时、最大 5.6600 ms，未进入长测。
