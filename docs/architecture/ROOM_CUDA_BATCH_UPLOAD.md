# ROOM CUDA 批次容量与上传合并实验

日期：2026-10-19。源码基线 `5926c7b`，先恢复上一轮归档的内存资源绑定候选，再隔离测试两项优化。
**本轮没有采用新的运行时配置。** 收紧 FFT 批次没有稳定的整体收益；合并上传改善了短测平均耗时，
但第三轮长测出现 **1/22,500 次超时、最大 6.7507 ms**。已完成的三轮共 67,500 次、1 次超时，
不满足 5.333333 ms 的零超时要求，余下三轮停止。失败结果没有被重跑替换。

[完整验证记录](evidence/room-cuda-batches-upload/validation.json)、
[优化补丁](evidence/room-cuda-batches-upload/optimizations.patch) 与
[尚未运行的原生回归草稿](evidence/room-cuda-batches-upload/unexecuted-regression.patch) 分开保存。
活动源码恢复到本轮之前的状态，保留既有用户研究改动；默认 realtime、exact 和公开 C ABI 不启用这些候选。

## 实现

1. **收紧批次。** 增加 16 组中间 Graph 容量，保留原五组容量作为兜底；准备阶段按响应逆 FFT、
   目标 FFT、卷积逆 FFT 的估算工作量排序，处理时选择能完整容纳实际工作量的第一组。
   不根据基准场景或调用编号选图，不改变变换尺寸、有效分区、目标时刻或 FIR 支持长度。
   所有计划与 Graph 在准备阶段生成，计划沿用原共享方式。
2. **合并上传。** 大型路径描述符数组沿用已有 pinned 缓冲，按有效长度直接上传；其余七组较小数组
   按选中 Graph 的容量打包到 64 字节对齐的缓冲，再作一次上传。每个内部 256 帧块由约八次 H2D
   提交变为最多两次。不是把整个大型最大容量缓冲都上传，也不重复打包大型路径数组。

GPU 内核算术、完整 FIR、元数据、淡化、FIFO、尾声及 256 帧延迟保持原样。保留上轮上下文失效保护。
两个编译开关均默认关闭，可分别生成基线、仅批次、仅上传、组合四个版本。私有能力位和 DLL 哈希由运行器
逐项核对；Rust 基准程序与事件脚本共用同一份冻结 Release 二进制。

## Windows 短测

i7-11800H／RTX 3060 Laptop 6 GiB，驱动 610.88、CUDA Toolkit 12.0；8 个 Audio MMCSS DSP worker，
调用线程普通优先级，96 kHz、512 帧、32 源、realtime。正常桌面、接通电源，未关闭用户应用或改动功耗设置。
复用 canonical MSVC/Ninja、FetchContent 和 Cargo 缓存，计时期间本任务没有并行构建或另一基准。

每项 32 次预热后计 1024 次调用，两轮交替先后顺序。以下“均值”是两轮均值的算术平均，
p99 列分别保留两轮自己的 p99，不能将其解释为合并后的分位值。

| 配置 | 位置均值 ms | 位置 p99，两轮 ms | 位置最大 ms | 距离均值 ms | 距离 p99，两轮 ms | 距离最大 ms | 超时 |
|---|---:|---|---:|---:|---|---:|---:|
| 基线 | 2.768475 | 3.0927 / 3.2926 | 3.6670 | 2.684760 | 3.1854 / 3.0639 | 3.8601 | 0 |
| 仅批次 | 2.795344 | 3.1268 / 3.2234 | 3.9673 | 2.605990 | 3.0147 / 2.9793 | 3.5866 | 0 |
| 仅上传 | 2.706506 | 3.2056 / 3.1576 | 3.6366 | 2.612246 | 3.0316 / 2.9890 | 3.5178 | 0 |
| 两项组合 | 2.750168 | 3.2917 / 3.1058 | 3.6885 | 2.607226 | 2.9967 / 3.1479 | 3.7711 | 0 |

仅上传在两个场景的两轮均值中均较基线低，汇总约降低 **2.2%／2.7%**。仅批次的位置均值没有稳定改善，
组合也没有比仅上传取得更好的整体结果，因此在长测前记录了选择：**只让“仅上传”进入数值扩展和长测**。
这是本轮短对照中的小幅收益，不是任意资源、GPU 或长期最大值的加速保证。
逐轮耗时、峰值工作集、主机快照、命令和哈希保存在 [short.json](evidence/room-cuda-batches-upload/short.json)。

## 数值与长测

仅上传与同机 CPU realtime 对照的默认／世界坐标完整回放，各输出 231,581 帧，包含尾声。
固定与碎片分包输出一致，全部输出有限；事件时刻、帧数及延迟一致，没有时移搜索、增益拟合或裁剪。

| 回放 | 最大相对 L2 | 最大误差／参考峰值 |
|---|---:|---:|
| 默认，含 headLocked | 4.870886e-7 | 6.516114e-7 |
| 世界坐标 | 5.146130e-7 | 7.056051e-7 |

两项均低于 `2e-5` 和 `2e-4`。数值记录与所有检查点见
[numerical.json](evidence/room-cuda-batches-upload/numerical.json)。
macOS arm64 的 14 项 GPU 参考、准备资源、质量和零分配检查，以及 clippy、C++ 格式和适用质量检查通过。
四份 Windows 原生无插桩 Release 构建与烟测通过；没有声称 CUDA 驱动内部零分配通过。

只使用事先选定并冻结的上传候选进行长测，每轮 22,500 次调用对应 120 秒音频。
期间交替插入 1024 次基线短对照；这些对照不计入候选的长测调用数，也不替代验收。

| 顺序 | 场景 | 调用数 | 均值 ms | p99 ms | 最大 ms | 超时 |
|---|---|---:|---:|---:|---:|---:|
| 1 | 位置运动 | 22500 | 2.730255 | 3.1841 | 4.1769 | 0 |
| 2 | 距离运动 | 22500 | 2.632571 | 3.0848 | 3.8553 | 0 |
| 3 | 位置运动 | 22500 | 2.743604 | 3.1913 | **6.7507** | **1** |

第三轮失败后停止余下轮次，没有改测另一份候选以替代本轮失败，也没有继续设备、GUI 打包或性能扩展。
无插桩长测未给出那次尖峰的阶段归因，不能据此认定是上传合并、上下文检查或某个后台应用导致。
三轮候选的峰值工作集为 1,478,877,184 字节，包含准备阶段；这不是独立显存测量。
[long.json](evidence/room-cuda-batches-upload/long.json) 保存完整结果、最慢调用序号和峰值内存。

## 复现与恢复

优化补丁依赖上一轮的归档接入候选；活动源码中没有应用它们。以匹配基线的工作区为起点：

```sh
git apply docs/architecture/evidence/room-cuda-integration/candidate.patch
git apply docs/architecture/evidence/room-cuda-batches-upload/optimizations.patch
```

沿用 [接入候选构建命令](ROOM_CUDA_INTEGRATION.md#构建与复现) 及同批资源。
通过以下开关分别构建并冻结四份 `mradm_room_cuda.dll`，每份与 CUDA 运行库放在同一目录；
本轮 Windows 用同卷硬链接复用运行库，未复制四份大依赖：

| 版本 | `MR_ADM_ROOM_CUDA_TIGHT_BATCHES` | `MR_ADM_ROOM_CUDA_COMPACT_UPLOAD` | 私有能力位 |
|---|---|---|---:|
| 基线 | OFF | OFF | 0 |
| 仅批次 | ON | OFF | 1 |
| 仅上传 | OFF | ON | 2 |
| 组合 | ON | ON | 3 |

```powershell
cmake -S . -B build/win-canon -DMR_ADM_ENABLE_ROOM_CUDA=ON `
  -DMR_ADM_ROOM_CUDA_TIGHT_BATCHES=OFF -DMR_ADM_ROOM_CUDA_COMPACT_UPLOAD=ON
cmake --build build/win-canon --target mr_adm_room_cuda
$env:MR_ADM_ROOM_WORKERS = '8'
& $Benchmark $Room96 1024 512 32 --hrir $Collection --quality realtime `
  --scenario ramps --audio-workers --cuda-library $FrozenDll
# 同配置执行 distance-ramps；通过短测和完整数值后才进行 22500 次长测。
& $Replay $Room96 $NewOutput --hrir $Collection --quality realtime `
  --dense --screens --head-motion wrap --cuda-library $FrozenDll
# 另建输出目录，追加 --world-only。CPU 数值参考省略 --cuda-library。
```

源码／二进制哈希、能力位、运行顺序及停止条件随证据保存。新的合成延迟 FIR 传输回归草稿未在失败后运行，
因此单独归档，不能作为已通过的检查。Windows 恢复、私有资源清理和本地改动保护记录见 validation.json。

后续 [512 点 FFT 融合实验](ROOM_CUDA_FFT_FUSION.md) 独立验证了该原生回归工具，并将其保留在研究目录。
目标生成融合取得约 6% 的短测平均耗时收益，但长测出现 1/112,500 次超时、最大 7.1111 ms，
仍未采用。该实验没有启用本页的批次缩紧或上传合并，结果分别归档。
