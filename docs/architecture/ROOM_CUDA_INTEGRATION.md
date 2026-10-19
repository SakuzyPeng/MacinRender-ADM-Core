# Windows ROOM CUDA 接入候选：长测未通过

日期：2026-10-19。基于已提交的 `e0a9f28` 组合继续验证正常准备资源和公开接口接入。
**新候选未通过正式长测：135,000 次调用中有 1 次超时，最大 5.6748 ms，高于 5.333333 ms。**
两项短测和前五轮长测均为零超时，第六轮距离运动失败。没有重跑以替换结果，也没有排除正常桌面后台负载。
因此停止设备性能扩展和发布；候选归档，活动代码恢复至包含 `1a214bd` 房间坐标映射提交的原工作区，
保留已验收组合及原有未提交研究内容。

[完整验证记录](evidence/room-cuda-integration/validation.json)、
[实测接入候选补丁](evidence/room-cuda-integration/candidate.patch) 和
[未执行的打包／文件检查草稿](evidence/room-cuda-integration/packaging-draft.patch) 分开保存。
以下实现说明及构建命令针对归档候选，**不表示当前活动源码已增加正式 CUDA 入口**。
没有改动公共 C ABI 函数、结构字段或枚举。原已验收组合的历史数据不转记为新二进制结果。

## 实现与适用范围

- `MR_ADM_ENABLE_ROOM_CUDA=ON` 构建私有 `mradm_room_cuda.dll` 并启用 Rust 适配层。
  在该构建中显式设置 `MR_ADM_ROOM_CUDA=1` 才选择 GPU；`0` 或未设置选择 CPU。
  无效值会报错，exact 不读取该选项。换房继承已有会话的后端。
- 当前适用范围为 Windows x64、CUDA 12 兼容 NVIDIA 驱动、realtime、96 kHz、4096 点 HRIR FFT、
  双耳早场及最多 32 个活动源。其他配置显式拒绝，不能把本轮证据推广到任意 GPU 或房间资源。
- HRIR 来自正常认证、准备后的 `PreparedSet`，以最多 64 行的临时缓冲上传。GPU 内核源码嵌入适配层，
  不再依赖导出的频谱 fixture、运行时 `.cu` 文件或手动指定 DLL 的生产入口。
- GPU bank 使用弱引用缓存和按键单次初始化，共享同一 CPU HRIR 的活动会话共用 GPU 表。
  初始化、上传和计算不持有全局缓存锁。CPU 原始准备数据、精度、资源认证和缓存语义保持不变。
- 数学内核继续与已验证的研究适配器共用；新入口固定启用 EQ 错峰、关闭四声部实验分块。
  保留完整 FIR、距离层、事件时刻、淡化、FIFO、延迟和尾声。没有采用 GPU 最终求和或 GPU EQ 尾分区实验。
- 私有 DLL 仅从承载 Rust 的模块旁加载，依赖搜索限制在受信目录与 Windows 默认目录。
  CPU 和 exact 不加载 CUDA DLL。DLL 及 bank 生命周期由活动准备数据与会话共同持有。

## 故障与释放

普通创建失败、缺失依赖及运行时错误通过既有错误接口返回；播放期间不静默切换 CPU。
Scene 异步渲染失败通过 `ADM_SCENE_PULL_FAILED` 和 Scene diagnostics 读取，不能只查询调用级 last-error。

一次独立子进程故障注入发现：直接调用 `cudaDeviceReset` 后继续处理会使进程退出。
现为每组 CUDA 资源保存上下文唯一 ID，在处理入口及释放时验证，失效时停止处理并避免再次销毁旧句柄。
验证包含失败诊断、释放全部受影响会话、重新准备和恢复相同 PCM。没有调用系统 GPU 重置，也未影响其他进程。
这不保证应用可以与外部线程并发调用 `cudaDeviceReset`；CUDA 要求销毁上下文时没有其他线程访问其资源。
硬件断电、驱动故障或系统 TDR 不属于这次子进程注入的实测覆盖。
[CUDA 上下文 ID 与销毁语义](https://docs.nvidia.com/cuda/archive/12.0.1/cuda-driver-api/group__CUDA__CTX.html)。

正常生命周期测试记录插件持有的 bank、早场实例、晚场实例、显存分配和锁页分配；这些计数不包含驱动、
cuFFT 的内部缓存或进程全部 RSS。两个并发 Scene 会话验证共享一个 bank，全部释放后插件计数归零。

## 构建与复现

在匹配补丁基线 `1a214bd` 的工作区先检查并应用 `candidate.patch`。只有需要继续打包研究时才另行应用
`packaging-draft.patch`；它尚未经过 Windows 执行验证。然后在已建立的 MSVC 环境中复用 canonical 构建和
FetchContent/Cargo 缓存：

```powershell
cmake -S . -B build/win-canon -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DMR_ADM_ENABLE_ROOM_CUDA=ON -DMR_ADM_BUILD_CAPI_BUNDLE=ON `
  -DMR_ADM_FLAC_PROVIDER=VENDORED -DMR_ADM_OPUS_PROVIDER=VENDORED `
  -DMR_ADM_ENABLE_SOFA=OFF -DMR_ADM_ENABLE_IAMF=OFF `
  -DMR_ADM_CONSISTENCY_DIAGNOSTICS=OFF
cmake --build build/win-canon --target mradm_exe mradm_capi_bundle `
  mr_adm_room_scene_benchmark mr_adm_room_file_device_probe

# $Room 与 $Collection 为同批经过认证的 96 kHz 资源，$CargoCache 为既有 canonical Cargo 目录。
cargo build --manifest-path rust/Cargo.toml --release --locked -p mradm-room-cuda `
  --example room_cuda_bench --example room_cuda_replay --target x86_64-pc-windows-msvc `
  --target-dir $CargoCache
$env:MR_ADM_ROOM_WORKERS = '8'
$env:MR_ADM_ROOM_CUDA = '1'
python tests/manual/room_cuda_integration.py --library build/win-canon/mradm_capi.dll `
  --plugin build/win-canon/mradm_room_cuda.dll --room $Room --hrir $Collection --output $NewEvidence
# 以下脚本属于未执行的 packaging-draft.patch。
python tests/manual/room_cuda_file.py --cli build/win-canon/mradm.exe `
  --room $Room --hrir $Collection --output $NewFileEvidence

# 两项 1024 调用均零超时后，才将调用数换为 22500，各运行三轮。
& $Benchmark $Room 1024 512 32 --hrir $Collection --quality realtime `
  --scenario ramps --audio-workers --cuda-library build/win-canon/mradm_room_cuda.dll
& $Benchmark $Room 1024 512 32 --hrir $Collection --quality realtime `
  --scenario distance-ramps --audio-workers --cuda-library build/win-canon/mradm_room_cuda.dll

# 候选公开接口自动在模块旁查找 DLL。
build/win-canon/mradm.exe render -i $Input -o $Output --renderer room-binaural `
  --room $Room --room-hrir $Collection --room-quality realtime
```

`room_cuda_replay` 与原 ROOM 回放程序共用事件及分包脚本，GPU 入口附加 `--cuda-library`，
CPU 对照省略该选项。完整 PCM／HRIR／响应检查点比较使用 `scripts/room/compare-realtime.py`；
仅在严格核对实验标签后移除该标签用于比较，其余数值、时序和长度门槛保持不变。

## 可选发布包草稿（未执行）

```powershell
scripts/release/package-windows-gui-release.ps1 -RoomCuda
scripts/release/smoke-windows-gui-release.ps1 -Archive $Archive
```

打包默认复用 `build/win-canon`，`-NativeBuildDir` 可显式选择其他已有目录；`-SkipNative -RoomCuda`
要求该目录已经启用 CUDA 并完成构建。CUDA 包使用独立 `-cuda` 名称，附带可选启动器、插件、cudart、
cuFFT、NVRTC、NVRTC builtins 及工具包许可。普通启动器不主动设置 GPU，CUDA 启动器设置进程级 opt-in。
不打包 `nvcuda.dll`，驱动由系统安装。
[CUDA 12 可再分发组件许可](https://docs.nvidia.com/cuda/archive/12.0.0/eula/index.html)。

## Windows 最终运行时验收

Windows x64，RTX 3060 Laptop GPU（6 GiB）、驱动 610.88、CUDA Toolkit 12.0，接通电源。
8 个 Audio MMCSS DSP worker，调用线程普通优先级，96 kHz、512 帧、32 源、realtime。
本任务未并行运行构建或其他基准，日常应用和后台服务保持运行。每轮均使用同批资源、事件和冻结的无插桩 Release 二进制。
每轮有 32 次预热；表中不包含预热。每轮 22,500 次正式调用对应 120 秒输入音频。

| 阶段 | 场景 | 调用数 | 均值 ms | p99 ms | 最大 ms | 超时 |
|---|---|---:|---:|---:|---:|---:|
| 短测 | 位置运动 | 1024 | 2.766394 | 3.2821 | 3.8248 | 0 |
| 短测 | 距离运动 | 1024 | 2.618468 | 2.9995 | 3.3186 | 0 |
| 长测 1 | 位置运动 | 22500 | 2.750569 | 3.1770 | 3.8119 | 0 |
| 长测 1 | 距离运动 | 22500 | 2.646385 | 3.0822 | 4.8542 | 0 |
| 长测 2 | 位置运动 | 22500 | 2.770033 | 3.2469 | 4.8720 | 0 |
| 长测 2 | 距离运动 | 22500 | 2.687097 | 3.1688 | 4.0991 | 0 |
| 长测 3 | 位置运动 | 22500 | 2.797141 | 3.2943 | 5.1540 | 0 |
| 长测 3 | 距离运动 | 22500 | 2.720902 | 3.2331 | **5.6748** | **1** |

长测峰值工作集为 1,478,983,680 字节，包含准备阶段。没有本轮交替运行的 CPU 或旧 GPU 对照，
不能据此宣称新的运行时降幅或内存收益。旧组合与新候选的偶发最大值也不足以归因于上下文检查或某个后台应用。
[逐轮原始结果及主机快照](evidence/room-cuda-integration/gate.json)、
[冻结二进制哈希](evidence/room-cuda-integration/frozen-sha256.json) 和
[Windows 实际覆盖源码哈希](evidence/room-cuda-integration/windows-source-sha256.json) 保留。
驱动脚本退出提示误写为 failed short gate；JSON 中失败轮次明确为 22,500 次长测，结果未作修改。

## 已执行的正确性检查与边界

- 加入上下文保护之前，内存绑定版完成两组完整数值回放，各 231,581 帧，包含尾声和固定／碎片分包。
  默认回放最大相对 L2 为 4.870886e-7、峰值相对误差 6.516114e-7；世界坐标回放分别为
  5.146130e-7 和 7.056051e-7，低于 `2e-5`、`2e-4`。没有时移搜索、增益拟合或裁剪。
  [该阶段数值数据](evidence/room-cuda-integration/numerical-before-context-guard.json) 与最终受保护二进制分开标识。
- 公开 C API 验证 CPU 默认和 exact 不加载 GPU DLL；CUDA 三次创建／释放、每次两个 epoch、碎片分包
  PCM 逐位一致；两条并发 Scene 流共享一个 GPU bank。完整输出 138,835 帧，相对 CPU L2 为
  4.241683e-7、峰值相对误差 3.864917e-7。全部释放后插件计数归零。
  [生命周期记录](evidence/room-cuda-integration/lifecycle.json)。
- 上下文保护修复版验证 Scene 错误诊断、完整释放、重新准备后相同 PCM，以及缺失 DLL 的明确错误。
  [故障注入记录](evidence/room-cuda-integration/errors.json)。不把它等同于真实驱动 TDR 验证。
- macOS arm64 的 3 项定向 CTest、14 项 GPU 参考／质量／零分配／准备资源测试通过；48／96 kHz
  的所有 HRIR 距离层和分数延迟表上传窗口保留原始谱值位模式。双平台 clippy、C++ 格式、原生构建及
  适用的 clang-tidy／cppcheck 通过。两平台 C ABI 导出仍为 157 个。

最终长测失败后，没有继续实际音频设备欠载验收、GPU 离线渲染／文件流式渲染 seek 和静音测试、
可选 GUI 包构建、自检或性能扩展。未生成或发布 CUDA GUI 包，打包草稿不能作为经过验证的发布流程。
本轮完整输出数值门槛的证据只适用于其明确记录的构建阶段；正式接入验收尚未完成。

Windows 覆盖恢复和临时私有资源清理记录见 validation.json 中的 restoration 字段。
此前已验收组合的证据继续保留在 [ROOM_GPU_COMBINED.md](ROOM_GPU_COMBINED.md)。
