# ROOM GPU 可行性探针

独立 Windows／CUDA 研究程序，生产 v2 和 exact 不选择此路径。它回答的是：真实 ROOM 数据的
频谱响应构造、目标准备和多目标卷积，连同 CPU／GPU 往返，能否装进 512／96 kHz 的时间预算。
**它不运行完整 Session，因此通过本探针不等于完整 ROOM 零超时通过。**

## 范围与数值检查

`room_gpu_fixture` 通过现有公开 Rust 模块读取原生九层集合，并导出完整频谱、材料、传播与
分数延时表。CPU 的 `ResponseWork` 为 16 个运动快照生成参考响应；不截 FIR、不删距离层，
只跳过原模型中增益已经为零的路径。每个快照有 32 源，按源／分支／耳逐一比较完整响应。
GPU 使用 f32 复数算术和 cuFFT；几何、插值权重及延时分解沿用 CPU f64 计算，提前写入描述符。
NVRTC 启用 FMA，关闭 FTZ，未启用 fast-math、FP16 或 TF32。

原型每 256 帧重建所有源的直达和早反射响应；一次 512 帧调用执行两次。反变换保持原尺寸和
桶累计顺序，完整 FIR 再切成 256 帧分区并做 512 点 FFT。输入历史、滤波器和 FFT 计划常驻 GPU。
可测试 1／4／8／16 个活动目标。**目标使用等权混合来测占用成本，没有实现产品的 10 ms 有限淡化。**
卷积算术另用完整历史和已知输入检查最初 512 帧输出，所有初始目标绑定相同响应；这不验证时变
淡化、seek、epoch reset 或完整 PCM 尾声。

数值阈值为相对 L2 `2e-5`、最大误差／参考峰值 `2e-4`，不做对齐、增益拟合或裁剪；静音严格保持
静音，结果必须有限。参考数据由本地 Release CPU 实现导出，再把同一份输入送至 Windows GPU；
这是组件算术检查，不是同机旧 realtime 与完整候选 Session 的最终验收。

## 三个边界

- `transfer`：音频输入上传、简单计算、双耳输出回传，观察基本往返成本。
- `response`：HRIR 插值、相位、路径传递、响应桶累加、逆 FFT 和完整 FIR 拼接，然后把 FIR 回传 CPU。
- `resident`：响应、目标 FFT、共享输入历史、多个活动目标卷积及双耳合成均在 GPU，只回传 PCM。

`graph` 将两次 256 帧处理捕获为 CUDA Graph；`launch` 保留逐个提交的对照入口。正式计时没有
CUDA 事件或内层剖析，使用主机墙钟，包含描述符复制到固定页内存、输入生成、全部提交、传输、
同步和消费输出的复制。前 32 次调用预热，文件读取、CUDA 初始化、JIT、分配和计划构建不计时。
主机代码的稳态缓冲预先分配；未认证驱动内部的零分配行为。

`--paced` 在每次调用后等待到该次开始后的 5.333333 ms，等待本身不计入调用耗时，用来观察
周期唤醒后的处理耗时；没有统计唤醒本身晚于计划时刻的延迟，因此不代表设备欠载次数。
`ramps` 和 `distance-ramps` 各重复八个几何快照，**不等于完整基准的连续轨迹、32 个
事件偏移和每样本控制语义**。预计算几何、Scene 事件展开、EQ、晚场、LFE、headLocked、extent、
换房和设备回调均不在计时范围；只有一个主机提交线程，没有计入八个音频 worker 的准备工作。

## 复现

开发与常规检查在本地完成，性能先在 Windows 测。复用现有 Cargo target 构建导出器，不创建
第二份依赖缓存。以下 Cargo 命令在 `rust/` 下执行，工具与探针命令在仓库根目录执行：

```sh
RUSTFLAGS=-Crelocation-model=pic CARGO_INCREMENTAL=0 cargo build --release --locked \
  --no-default-features -p mradm-dsp --example room_gpu_fixture \
  --target "$NATIVE_TARGET" --target-dir "$EXISTING_CARGO_TARGET"
"$FIXTURE_EXPORTER" "$ROOM96" "$FULL_COLLECTION" "$NEW_PRIVATE_FIXTURE"
python3 scripts/research/room_gpu/seal_fixture.py "$NEW_PRIVATE_FIXTURE"
```

将同一份私有 fixture 传到 Windows，沿用 canonical MSVC／SDK 环境及已安装的 CUDA。探针使用
普通 MSVC 编译主机程序，NVRTC 编译 GPU 内核，避免让旧 nvcc 驱动新版 MSVC；不修改驱动设置。
没有新的 FetchContent／Cargo 缓存，也没有把 GPU 库接入生产构建。

```powershell
python scripts/research/room_gpu/run.py --fixture PRIVATE_FIXTURE --output NEW_SCREEN `
  --env-file PRIVATE_CANONICAL_ENV_JSON --route resident --targets 16 --calls 64
```

先确认该配置的数值检查及短筛通过。失败就停止，不进入后续调用数或长测。通过后可分别运行
两个 1024 次的组件短测，复用已构建的探针：

```powershell
python scripts/research/room_gpu/run.py --fixture PRIVATE_FIXTURE --output NEW_RAMPS `
  --binary NEW_SCREEN/probe.exe --route resident --targets 16 --calls 1024 --scenario ramps --paced
python scripts/research/room_gpu/run.py --fixture PRIVATE_FIXTURE --output NEW_DISTANCE `
  --binary NEW_SCREEN/probe.exe --route resident --targets 16 --calls 1024 --scenario distance-ramps --paced
```

收集每次运行的 JSON、GPU 状态和源码／二进制哈希；不同时运行构建或另一基准。显存观察值
包含该 GPU 上其他占用，不是本进程独立峰值。用完删除传输的私人频谱和响应，保留汇总及哈希。
CUDA 只验证 NVIDIA／Windows 路线；没有据此宣称 macOS GPU 或完整实时档已经支持。

API 依据：[NVRTC 12.0](https://docs.nvidia.com/cuda/archive/12.0.0/nvrtc/index.html)、
[cuFFT 12.0 批量变换与 Graph 支持](https://docs.nvidia.com/cuda/archive/12.0.0/cufft/index.html)。

## 完整 Session 研究入口

后续 `room-gpu-experiment` 在现有 Session 中增加可选的批量边界；`gpu_experiment.rs` 收集实时
几何描述符、完整目标和原样本位置的有限淡化任务，`reference.rs` 提供本地正确性执行器。
`session_backend.cpp`／`session_kernels.cu` 执行 CUDA 批次，CPU 晚场与 GPU 运算重叠。
示例侧的 `room_support/gpu_native.rs` 是私有 DLL 边界；DSP crate 仍不包含 unsafe 或 CUDA 依赖。

首个完整 Session 候选的 Windows 位置运动短测已失败，结果见 [完整 Session 报告](../../../docs/architecture/ROOM_GPU_SESSION.md)。
以下命令用于复现或修改后的新候选，不应对未修改的失败候选继续长测。

本地正确性检查（`rust/` 下，追加既有 native target／target-dir）：

```sh
cargo test --locked --no-default-features -p mradm-dsp --features room-gpu-experiment \
  --test room_gpu --test room_quality --test room_no_alloc
cargo clippy --locked --no-default-features -p mradm-dsp --features room-gpu-experiment \
  --lib --example room_bench --example room_replay --test room_gpu -- -D warnings
```

在 Windows canonical MSVC 环境下，将 `CUDA_INCLUDE`、`CUDA_LIB`、`NEW_NATIVE_OUT` 替换为实际路径，
先创建新的小型输出目录；不另建大项目缓存：

```powershell
cl /nologo /utf-8 /O2 /EHsc /std:c++20 /DNDEBUG /W4 /LD /I CUDA_INCLUDE `
  scripts/research/room_gpu/session_backend.cpp `
  /FoNEW_NATIVE_OUT/session_backend.obj /FeNEW_NATIVE_OUT/room_gpu_session.dll `
  /link /LIBPATH:CUDA_LIB cuda.lib cudart.lib nvrtc.lib cufft.lib
```

在 `rust/` 下复用 canonical Cargo target 构建原生示例：

```powershell
cargo build --release --locked --no-default-features -p mradm-dsp `
  --features room-gpu-experiment --example room_bench --example room_replay `
  --target x86_64-pc-windows-msvc --target-dir EXISTING_CANONICAL_CARGO_TARGET
```

回放和计时均使用同一份原生房间、完整集合及已认证的 GPU fixture。GPU fixture 的生成和认证
沿用上文；实际房间材料和传播表由当前实例重新准备上传，支持本次回放的换房参数变化。
运行前让 CUDA `bin` 可由当前进程找到。基线使用相同 `room_replay` 命令，省略三个 `--gpu-*` 参数。

```powershell
$env:MR_ADM_ROOM_WORKERS = "8"
ROOM_REPLAY_EXE ROOM96 NEW_GPU_REPLAY --hrir COLLECTION --quality realtime --dense --screens --head-motion wrap `
  --gpu-dll NEW_NATIVE_OUT/room_gpu_session.dll --gpu-fixture PRIVATE_FIXTURE `
  --gpu-kernels scripts/research/room_gpu/session_kernels.cu
ROOM_BENCH_EXE ROOM96 1024 512 32 --hrir COLLECTION --quality realtime --scenario ramps `
  --gpu-dll NEW_NATIVE_OUT/room_gpu_session.dll --gpu-fixture PRIVATE_FIXTURE `
  --gpu-kernels scripts/research/room_gpu/session_kernels.cu
```

只有位置短测零超时才执行同配置的 `--scenario distance-ramps`。首个候选在位置短测失败后停止。后续结果见下一节。
没有设备回调或长测通过结论；生产 C ABI 没有添加 GPU 选档入口。

## 批次缩紧与 GPU 晚场

这一阶段保留较紧的 Graph 容量和异步晚场大分区卷积。完整 FIR、时序、尾声和 256 帧 Session
延迟不变。Native 后端还需要 `session_late.h`／`session_trace.h`；上述普通构建命令不变。
`ROOM_GPU_TRACE` 和 Cargo `room-trace` 只用于独立诊断构建，不能用于验收。

[本轮报告](../../../docs/architecture/ROOM_GPU_SESSION_LATE.md) 记录全部三个候选：当前保留版本
数值与两项 1024 次短测通过，但第一次 22,500 次位置长测出现一次超时，其余轮次停止。
单次传输和四组归约未证明稳定收益，已撤回。不得对这个未修改的失败候选继续长测以寻找较好的轮次。

以后形成新候选，仍先按上文依次执行两项短测和完整数值验证。只有都通过，才能用以下命令进行
六轮长测；变量使用与短测相同的资源／二进制路径，输出目录应新建。保留正常桌面后台负载，
记录供电和负载；不并行构建或运行另一基准。任一超时立即退出，不拼接不同候选的结果。

```powershell
$env:MR_ADM_ROOM_WORKERS = "8"
New-Item -ItemType Directory -Path $NEW_LONG_OUT -ErrorAction Stop | Out-Null
foreach ($round in 1..3) {
    foreach ($scenario in @("ramps", "distance-ramps")) {
        $raw = & $ROOM_BENCH_EXE $ROOM96 22500 512 32 --hrir $COLLECTION --quality realtime `
            --scenario $scenario --gpu-dll $GPU_DLL --gpu-fixture $PRIVATE_FIXTURE `
            --gpu-kernels $GPU_KERNELS
        if ($LASTEXITCODE -ne 0) { throw "Native benchmark failed" }
        $raw | Set-Content -Encoding utf8 (Join-Path $NEW_LONG_OUT "$scenario-$round.json")
        $result = $raw | ConvertFrom-Json
        if ($result.profiling -or $result.blocks -ne 22500 -or $result.sample_rate -ne 96000 -or `
            $result.call_frames -ne 512 -or $result.sources -ne 32 -or `
            $result.experiment.kind -ne "room-gpu-session") { throw "Unexpected benchmark configuration" }
        if ($result.deadline_misses -ne 0) { throw "Deadline missed; stop this candidate" }
    }
}
```

每轮只计 22,500 次处理调用（120 秒音频），前 32 次为预热。以上墙钟是 ROOM Session 软件计时，
不代表音频设备欠载测试。完整原始结果和主机观察值随报告保存；当前仍没有生产 GPU 选档。

后续的 [有效分区 FFT 实验](../../../docs/architecture/ROOM_GPU_PACKED_TARGETS.md) 缩减了目标变换行数，
数值与短测通过，但长测失败；改动已经回退，补丁和完整失败记录单独保留。

后续 [CPU 位置控制与输入流水组合](../../../docs/architecture/ROOM_GPU_PIPELINE.md) 的两项短测及数值
检查通过，首轮长测 1/22,500 超时；运行时改动已回退，补丁单独保存。
基准保留 `slowest_calls` 的调用序号，回放保留独立 `--world-only` 场景，用于验证没有 headLocked
源时的完整世界坐标路径。该模式的固定输入为 512 帧，仍与碎片输入逐位比较，并写入不同的
scenario 与 world_only 标记。CPU 和 GPU 比较必须使用相同标记，默认回放仍覆盖 headLocked。

当前实验代码采用 [输入流水与有效分区 FFT 组合](../../../docs/architecture/ROOM_GPU_COMBINED.md)：
两个单项候选虽曾各自长测失败，本次组合重新通过完整数值及两项短测，随后同一套无插桩 Release
二进制完成六轮、135,000 次调用，零超时，最大 5.1795 ms。保留各单项失败记录，不拼接历史成功轮次。
随后补充矩阵中的 32 源换房出现 1/512 次超时、最大 5.6083 ms，该记录保留。
当前验收范围不承诺频繁换房零超时：该项仅记录性能，不阻断固定位置／距离运动验收。
该结果限固定的 Windows CUDA／96 kHz／512 帧／32 源位置与距离运动门槛；生产 realtime、exact
和公共 C ABI 仍不选择 GPU，正式设备回调及其他 GPU 型号尚未认证。构建与运行命令不变。

[后续换房调查](../../../docs/architecture/ROOM_GPU_SWITCH.md) 记录了无更新／无任务图的失败候选，
该运行时改动已回退。进一步的独立阶段剖析将主要热点缩小到淡化期间的 CPU 声源 EQ 长分区刷新；
保留 `empty_graph_test.py` 原生状态回归工具；后续 EQ 批处理结果见下文。

[声源 EQ 批处理实验](../../../docs/architecture/ROOM_GPU_EQ_BATCH.md) 的原生回归、完整数值和
单次／重复换房短测通过，但 44 次换房的 120 秒音频长测出现 457/22500 次超时，候选已回退。
保留 `eq_batch_test.py` 和完整候选补丁；新测试工具需要补丁版本的私有 EQ DLL 接口。
首次调查按当时的换房门槛停止，未执行固定六轮运动长测。范围调整后恢复同一候选补验：
两项 1024 次短测及第一轮位置长测通过，但第一轮固定房间距离长测出现 **1/22500 次超时、
最大 5.6484 ms**，后续四轮停止，EQ 运行时再次回退到 `e7ace2a`。
这次失败不涉及换房；[补验记录](../../../docs/architecture/evidence/room-gpu-eq-batch/motion-validation.json)
与首次换房记录分别保留，尚未进入设备播放接入。

## 后续余量配置

[分项与组合验收](../../../docs/architecture/ROOM_GPU_COMBINED.md#后续余量实验) 保留 CPU EQ
长分区错峰与八个 worker 的 `Audio` MMCSS。基准追加 `--audio-workers --gpu-eq-stagger`，
回放只追加 `--gpu-eq-stagger`；`--audio-workers` 是基准对齐开关，不改变默认或全局线程设置。
该组合已独立通过数值、短测及六轮 **135,000 次零超时**，最大 **4.7578 ms**，最小实测余量
约 **0.575533 ms**。软件 Session 延迟仍为 256 帧，默认 realtime／exact／公共 C ABI 不选择它。

`--gpu-voice-chunks` 的四声源分组未证明一致整体收益，不纳入通过组合。GPU 声源求和虽然减少
回传量，但加入后的组合在固定房间距离长测出现 **1/22500 次超时、最大 5.6088 ms**，运行时
已撤回，补丁与数据单独保留。不得合并不同组合的成功轮次，亦不重跑失败轮次挑选通过结果。
MMCSS 记录只覆盖八个 DSP workers，基准调用者保持普通调度；实际设备播放仍需单独验证。

## 内存绑定与 FFT 融合回归

后续 [512 点 FFT 融合实验](../../../docs/architecture/ROOM_CUDA_FFT_FUSION.md) 基于归档的内存资源绑定
适配层，对目标生成融合、卷积融合及组合分别验证。目标生成融合改善约 6% 的短测平均耗时，但长测
1/112,500 次超时、最大 7.1111 ms；运行时代码已回退，源码补丁与结果保留在报告中。

`resident_transport_test.py` 已在 Windows 对四种配置执行通过。它合成双耳不同延迟／增益的 FIR，
以独立解析结果检查 1／32 源、重复 epoch、静音、空任务、历史保留及重置，并检查释放后资源计数。
无需私人 HRIR 或音频资源；它是正确性回归，不提供性能证据。该工具需要内存绑定版 DLL 的私有 bank 接口，
当前 `room_gpu_session.dll` 研究接口不能直接替代它。

```powershell
python scripts/research/room_gpu/resident_transport_test.py --library $ResidentDll --kernels $MatchingKernelSource
```

`$MatchingKernelSource` 必须与 DLL 配套：融合 DLL 需要报告中的融合补丁版本，未融合的内存绑定 DLL
可以使用当前 `session_kernels.cu`。CUDA 运行库与 DLL 放在同一目录；融合配置还需保留 DLL 编译时指定的
MathDx 头文件路径以供 NVRTC 使用。完整准备、构建和冻结步骤见报告，不应重跑未修改的失败候选长测。

[延后等待实验](../../../docs/architecture/ROOM_CUDA_DEFERRED_WAIT.md) 在 Windows 距离运动短测出现
1/1024 次超时、最大 5.6600 ms，运行时已归档。原生工具保留 `--cross-thread` 选项，验证在另一主机线程
完成输出，并在最后一次提交后直接销毁实例、检查全部资源计数归零。该选项需要支持跨线程完成的
内存绑定 DLL，完整构建步骤见报告；正确性通过不代表延后等待候选通过性能验收。

[1 ms 目标实验](../../../docs/architecture/ROOM_CUDA_CRITICAL_PATH.md) 记录了新的时间线和输入／响应组合：
最佳配置两场景均值改善 0.654／0.728 ms、4096 次短测零超时，未达到本轮收益目标，未进入长测。
GPU 晚场双耳解码扩展仅在首组位置配对中减少 0.336 ms，随后停止。GPU 实现均以补丁归档；
活动代码只保留通过双平台逐位检查的整数环索引简化，不转记旧的零超时结果。
