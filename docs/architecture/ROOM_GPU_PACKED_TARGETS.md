# ROOM GPU 有效分区 FFT 实验

日期：2026-10-18（本地时区）。基于 [批次缩紧与 GPU 晚场原型](ROOM_GPU_SESSION_LATE.md)。

**结论：两项 Windows 短测与完整数值对照通过，首轮长测失败。候选已回退，保留补丁和失败证据。**
该轮结束时实验入口恢复到上一版保留原型，默认 realtime 与 exact 路径没有新增变更。
后续与输入流水形成的新组合见 [组合报告](ROOM_GPU_COMBINED.md)；本页记录仍属于原单项候选。

## 改动

将每个目标的两个耳道、有效 256 帧分区连续打包，目标 FFT 仍为 512 点 R2C。
使用原有结构性支持长度决定分区数量，保留完整有效 taps、末分区补零、响应桶累计顺序、
全部距离层和动态目标的原样本时序。变换结果按映射写回原有物理槽，卷积只读目标声明的有效分区。

Graph 容量同时考虑响应行数、更新数、卷积任务数和目标 FFT 行数。计划和 Graph 均在准备阶段
建立；同一实例中串行执行的图共用相同尺寸／批量数的 cuFFT 计划及其工作区，没有新增大型滤波器副本。
该资源族生成 33 个 Graph 容量组合；处理阶段不创建计划或分配该布局的存储。
[cuFFT 12.0 的计划重用及 Graph 支持](https://docs.nvidia.com/cuda/archive/12.0.0/cufft/index.html#cuda-graphs-support)
是实现依据；计划不跨实例的并发 stream 共享。

## Windows 验收

固定八个 worker、96 kHz、512 帧、32 源、realtime，RTX 3060 Laptop／i7-11800H。
沿用 CUDA 12.0、驱动 610.88、canonical MSVC 和原 Cargo 缓存；无插桩 Release，前 32 次调用预热。
使用正常桌面环境并接通电源，没有并行构建或另一项本任务基准。背景负载不作为排除失败的理由。

| 阶段 | 场景 | 调用数 | 均值 ms | p99 ms | 最大 ms | 超时 |
|---|---|---:|---:|---:|---:|---:|
| 短测 | 位置运动 | 1024 | 3.446776 | 4.4057 | 4.8267 | 0 |
| 短测 | 距离运动 | 1024 | 3.313605 | 4.1462 | 4.7742 | 0 |
| 长测第 1 轮 | 位置运动 | 22500 | 3.647923 | 16.5439 | 93.1708 | **362** |

长测失败后其余五轮未执行，没有继续 macOS 性能矩阵。长测前后的背景 CPU 观察范围为
约 3.56%–8.94%，供电状态为 AC；这些是前后快照，不能说明每次超时发生时的负载。
观察到的进程峰值工作集为 1,707,323,392 bytes，包含准备阶段；没有独立测量进程显存峰值。
没有用短测结果或后续诊断的较好结果替换长测失败，也不据此断言尖峰由打包算法或后台负载造成。

## 正确性

Windows 同机 CPU 与候选完整回放均为 231,581 帧，延迟 256 帧；输入 96,013 帧后实际输出 135,568 帧。
manifest 的 tail_frames=135,502 是初始房间静态值，排尾终点来自换房后的 Session 状态。
固定／碎片分包一致，全部输出有限。包含运动、头姿、屏幕、headLocked、extent／diffuse、换房和尾声。
相对 CPU 的最大 L2 为 `4.898492078127715e-7`，最大误差／参考峰值为 `6.516114024176086e-7`，
分别低于 `2e-5` 与 `2e-4`。没有搜索时移、拟合增益或裁剪输出。

这一回放的全部 HRIR、分支和 PCM 检查点，与上一版 GPU 回放的文件哈希也完全相同；
该结论限于本次同资源、同事件回放，不能替代其他资源族的验收。

本地 CPU 模拟实际 pack／install 内核，对照先合成完整 FIR 的独立时间域参考，覆盖零更新、
1／3／17／256 次更新，支持长度 1／256／257／777／2048／4096／7071 帧，检查布局、补零和保护区。
普通 Debug 与 UBSan／bounds 检查通过。ASan 检查未完成：一次采样显示运行时在 main 前初始化停滞，另一运行时尝试也超时；
均已停止，不能计为 ASan 通过。C++ 格式与 cppcheck 通过；本轮未修改 Rust 处理代码。

## 补充诊断

正式长测结束后尝试 Nsight Systems 2022.4.2 的 CUDA 节点、WDDM 和线程调度采集，采集器在
事件整理阶段异常退出，没有可读时间线。它遗留的本任务采集进程已核实并终止；这发生在正式
长测之后，不影响长测失败证据。第一次分阶段短诊断受到该残留进程的资源占用影响，单独留存；
清理后的复采未形成有效结果，后续排查按用户要求停止。这些诊断均不作为无插桩验收。

诊断中的布局计数显示，目标 FFT 平均执行行数由原布局的 4480 降到 3072，减少约 31.4%。
计时受插桩和残留进程占用影响，不能用于验收或给长期尖峰定因。用户随后提及 FFmpeg 编码，
主机和重叠时间未确认，并明确要求停止占用排查；本轮仍按正常桌面环境下不合格处理。

诊断结果及完整原始数值、主机条件、源码／二进制哈希、清理证明见
[validation.json](evidence/room-gpu-packed/validation.json)。长期尖峰未得到可靠归因。

## 复现

候选改动保存在 [candidate.patch](evidence/room-gpu-packed/candidate.patch)，针对当前保留的 GPU
原型；正常入口已经回退。需要检查或形成新候选时，从仓库根目录应用：

```sh
git apply --check docs/architecture/evidence/room-gpu-packed/candidate.patch
git apply docs/architecture/evidence/room-gpu-packed/candidate.patch
clang++ -std=c++20 -O0 -g -Wall -Wextra -Werror -fsanitize=undefined,bounds \
  docs/architecture/evidence/room-gpu-packed/layout_test.cpp -o "$LAYOUT_TEST_BINARY"
UBSAN_OPTIONS=halt_on_error=1 "$LAYOUT_TEST_BINARY"
```

Windows 构建、fixture 认证、数值比较与短测参数沿用
[GPU 工具说明](../../scripts/research/room_gpu/README.md#完整-session-研究入口)。
两个场景各 1024 次，只有短测及数值均通过的新候选才进入 22500 次的六轮长测；任一轮超时即停止。
不要继续运行这个未修改的失败候选以寻找较好的长测轮次。

Windows 覆盖状态已逐项核对并恢复：355 项，87 个原文件恢复、268 个任务文件移除，原 HEAD 与 Git 状态一致。
本地与 Windows 临时私有资源已清理，既有缓存和原有研究改动保留。后续占用排查及测试已停止。
