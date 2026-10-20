# ROOM CUDA V2 显式试用接入

日期：2026-10-20。基于 `05b297ab84f8`，默认构建和默认渲染继续使用 CPU。
CUDA V2 已接入现有显式选项并提供 Windows GUI 试用包；**正式六轮零超时验收仍未通过**。
本次完成产品入口、完整数值回放和部署验证，没有重启已经结束的小超时归因调查。

## 使用与适用范围

解压 Windows CUDA trial 包，运行根目录的 `MacinRender ADM CUDA Trial.cmd`，
在现有界面选择 ROOM、96 kHz 资源及 realtime 质量。普通启动入口不设置 CUDA 选项。
exact 始终使用 CPU；其他渲染器保持原行为。

CLI 和 C API 使用同一个进程级显式选项：

```powershell
$env:MR_ADM_ROOM_CUDA = '1'
build/win-canon/mradm.exe render -i $Input96 -o $Output `
  --renderer room-binaural --room $Room96 --room-hrir $Collection --room-quality realtime
```

未设置或 `0` 使用 CPU；无效值报错。缺失依赖、不支持的资源及运行时错误通过现有错误接口报告，
不会在处理中静默切换后端。换房继承会话后端。公开 C ABI 的 157 个 `adm_*` 函数、结构和枚举未改变。

本轮实测机器为 Windows x64、RTX 3060 Laptop GPU、驱动 610.88、CUDA 12.0。
GPU 路径支持 realtime、96 kHz、4096 点 HRIR FFT、双耳早场、最多 32 个活动源。
试用包生成 SM 75／80／86／89／90 的设备程序；**除 SM 86 外仅完成编译，没有这些显卡的实机验收**。
保留全部距离层、完整 FIR、元数据时序、淡化、FIFO、256 帧算法延迟和完整尾声。

## 接入与部署改动

- 生产适配器 `bind_realtime` 固定选择完整 V2：紧凑位置控制、EQ 错峰 256 帧、
  紧凑上传、频率块调度及目标 FFT 融合。原生能力位必须为 `22`。
  检查报告增加 `implementation=room-cuda-v2`、`compact_control=true` 和能力位。
  研究入口仍可单独选择配置。
- 目标 gather／512 点 FFT／安装在构建时使用 MathDx 24.08 编译为 PTX，
  运行时从插件旁的 `room_cuda/` 加载。按支持的 1–64 个桶分别编译入口，在准备阶段选择。
  基础频谱内核继续使用不需要 SDK 头文件的 NVRTC 路径。
- 对应的私有设备参数增加布局描述；新增私有 kernel contract 检查，在准备阶段拒绝旧插件。
  PTX 还检查 FFT 尺寸、目标容量和描述尺寸。所有读取和 CUDA 模块加载均在准备阶段完成。
- GUI 打包复用 `build/win-canon`。`-SkipNative` 只跳过编译，仍从指定构建树取 C API，
  并检查 CPU／CUDA 配置，避免误用旧 GUI runtime。CUDA 包另带显式启动器、依赖清单、
  PTX 哈希和 NVIDIA／MathDx 许可；不携带 SDK 头文件、Windows AVRT 或 NVIDIA 驱动 DLL。
- 默认 `MR_ADM_ENABLE_ROOM_CUDA=OFF`。启用构建中的三个 V2 原生开关默认 ON，
  未保留的卷积 FFT 融合仍为 OFF。

初版部署使用动态桶循环。两轮交替短测显示位置运动均值回退，因此没有交付该版本；
改为专用桶入口后，同轮两场景合计均值恢复到冻结 V2 的范围。原始试验结果继续保留。

## 正确性证据

完整回放使用冻结 V2 EXE、同机 CPU realtime、相同认证资源和事件。
原始实验 manifest 保留；严格检查 GPU 标签后才生成比较副本。
没有时间对齐、增益拟合或裁尾。每组输出 **231,581 帧**，17 个 HRIR／分支／PCM 检查点全部通过。

| 完整会话 | 相对 L2（对 CPU） | 最大误差／参考峰值 | 对冻结 V2 GPU |
|---|---:|---:|---|
| 密集事件、headLocked、wrap 头姿 | 3.56564×10⁻⁷ | 6.27999×10⁻⁷ | 逐位一致 |
| 密集事件、world-only、wrap 头姿 | 3.69584×10⁻⁷ | 6.92335×10⁻⁷ | 逐位一致 |
| 密集事件、屏幕映射、wrap 头姿 | 4.25971×10⁻⁷ | 5.29204×10⁻⁷ | 逐位一致 |

三组均包含固定／碎片分包、房间变化、完整尾声；事件、帧数、延迟及尾长元数据完全一致。
数值门槛仍为相对 L2 ≤ `2e-5`、最大误差／参考峰值 ≤ `2e-4`。

产品入口检查包括：

- Scene 流式渲染的三次创建／释放、每次两个 epoch、不同分包、静音、并发 bank 共享、
  上下文失效诊断和重新创建。单场景输出 138,835 帧，相对 L2 为 `3.98778e-7`；
  持有的 bank、早／晚场实例、设备及锁页分配计数在释放后均为零。
- 离线渲染及其文件流式渲染 process／seek 路径：完整 144,719 帧，
  CUDA 相对 L2 为 `1.94896e-7`，最大误差／参考峰值为 `3.43028e-7`。
  静音保持静音；固定 seek 窗口逐位匹配连续输出。复用预览会话的完整、窗口及再次完整渲染均通过。
- exact 和 CPU realtime 文件 PCM 与本轮接入前的 `05b297ab` CPU 构建逐位一致；
  exact 在 CUDA 选项无效或插件缺失时仍运行 CPU。既有 `718a804` 历史基线要求不变，
  本轮没有重新生成或替换该历史基线。
- 包移到中文及空格路径，移除开发 PATH，并临时隐藏构建用 MathDx：
  GPU 输出仍逐位一致。缺失插件／NVRTC 返回 Unsupported，缺失 PTX 返回 RenderFailed 并释放资源，
  旧插件在绑定前被拒绝；恢复依赖后输出不变。没有卸载主机的 CUDA Toolkit。
- Windows 和 macOS 的公开 96 kHz 回归覆盖文件／Scene、碎片分包、seek、epoch reset 和 48 kHz 输出。
  macOS 另通过 3 项 CTest、16 项 Rust 定向测试、格式检查与 C ABI 导出检查。
  GPU 控制适配器的零分配检查使用现有参考工厂；不将其表述为 NVIDIA 驱动内部零分配证明。

单场景测试的插件持有分配为约 1.002 GB 设备内存、70.24 MB 锁页内存；
两个并发会话共享一个 bank，约 1.441 GB／140.47 MB。这些计数不包含驱动或 cuFFT 内部开销。
GUI 自检及启动烟测不等同于实际声卡持续播放或设备 underrun 验收。

## Windows 性能筛选

无插桩 Release，8 个 Audio MMCSS worker、普通调用线程、96 kHz、512 帧、32 源，
每场景 1,024 次计时调用，32 次预热另计。保持正常桌面负载并记录供电和进程快照；
计时期间没有并行构建或其他基准，没有关闭应用、改变供电策略或排除普通超时。

最终代码构建的独立短测：

| 场景 | 均值 ms | p99 ms | 最大 ms | 超时 | 峰值工作集 MB |
|---|---:|---:|---:|---:|---:|
| 位置运动 | 2.152317 | 2.5956 | 2.8037 | 0/1024 | 1501.8 |
| 距离运动 | 2.394600 | 3.2514 | 4.2122 | 0/1024 | 1493.9 |

专用编译版的同轮对照（最终格式整理前）：

| 场景 | 冻结 V2 均值 ms | 专用编译版均值 ms | 冻结 V2／候选超时 |
|---|---:|---:|---:|
| 位置运动 | 2.433643 | 2.489895 | 1／0 |
| 距离运动 | 2.206789 | 2.146798 | 0／0 |

冻结参考的位置超时为 5.9111 ms，照常计入。专用编译版两项合计均值几乎相同，
不能用这个小样本对照声称新增运行时收益。峰值工作集减少约 207–216 MB，
这是包含准备阶段的基准进程峰值，并非音频处理阶段的单独内存测量；它不能替代截止时间验收。

**本轮短测不是正式长测通过。** 历史普通小超时继续保留，没有再次运行六轮 135,000 次验收。
已确认硬件异常时段仍按维护者要求单列；频繁换房继续只记录，不承诺零超时。

## 复现

在既有 Windows canonical MSVC／Ninja 环境中运行，保留 Cargo 和 FetchContent 缓存。
`$MathDxInclude` 指向已验证的 MathDx 24.08 `include`；部署机器只需包内运行库及兼容 NVIDIA 驱动。

```powershell
cmake -S . -B build/win-canon -DCMAKE_BUILD_TYPE=Release `
  -DMR_ADM_ENABLE_ROOM_CUDA=ON -DMR_ADM_BUILD_CAPI_BUNDLE=ON `
  -DMR_ADM_CONSISTENCY_DIAGNOSTICS=OFF `
  -DMR_ADM_ROOM_CUDA_FUSE_TARGET=ON -DMR_ADM_ROOM_CUDA_FUSE_CONVOLVE=OFF `
  -DMR_ADM_ROOM_CUDA_COMPACT_UPLOAD=ON -DMR_ADM_ROOM_CUDA_FREQUENCY_MAJOR=ON `
  "-DMR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR=$MathDxInclude"
cmake --build build/win-canon --target mradm_exe mradm_capi_bundle --parallel 8
python tests/manual/room_cuda_integration.py --library build/win-canon/mradm_capi.dll `
  --plugin build/win-canon/mradm_room_cuda.dll --room $Room96 --hrir $Collection --output $ApiEvidence
python tests/manual/room_cuda_file.py --cli build/win-canon/mradm.exe `
  --library build/win-canon/mradm_capi.dll --baseline-cli $FrozenCpuCli `
  --room $Room96 --hrir $Collection --output $FileEvidence
scripts/release/package-windows-gui-release.ps1 -RoomCuda -SkipNative -NativeBuildDir build/win-canon
scripts/release/smoke-windows-gui-release.ps1 -Archive $Archive
python tests/manual/room_cuda_portable.py --app $PackagedApp `
  --room $Room96 --hrir $Collection --output $PortableEvidence `
  --reference $ApiEvidence --stale-plugin $FrozenV2Plugin
```

回放 EXE 的构建和短测参数沿用 [V2 构建步骤](ROOM_CUDA_V2_CANDIDATE.md#构建与运行)。
下面的 `$ReferenceReplay` 为同机冻结参考 EXE，`$Replay` 为待验 EXE，`$Plugin` 为待验插件，
`$ReplayRoot` 必须是尚不存在的输出目录：

```powershell
if (Test-Path $ReplayRoot) { throw 'Choose a new replay output directory' }
New-Item -ItemType Directory -Path $ReplayRoot | Out-Null
$env:MR_ADM_ROOM_WORKERS = '8'
$cases = @(
    @{ name = 'dense-headlock'; options = @('--dense', '--head-motion', 'wrap') },
    @{ name = 'dense-world'; options = @('--dense', '--world-only', '--head-motion', 'wrap') },
    @{ name = 'dense-screen'; options = @('--dense', '--world-only', '--screens', '--head-motion', 'wrap') }
)
foreach ($case in $cases) {
    $cpu = Join-Path $ReplayRoot ($case.name + '-cpu')
    $gpu = Join-Path $ReplayRoot ($case.name + '-cuda')
    $options = $case.options
    & $ReferenceReplay $Room96 $cpu --hrir $Collection --quality realtime @options
    if ($LASTEXITCODE) { throw 'CPU replay failed' }
    & $Replay $Room96 $gpu --hrir $Collection --quality realtime @options `
        --cuda-library $Plugin --cuda-compact-control
    if ($LASTEXITCODE) { throw 'GPU replay failed' }
    $reference = Get-Content (Join-Path $cpu 'manifest.json') -Raw | ConvertFrom-Json
    $manifest = Get-Content (Join-Path $gpu 'manifest.json') -Raw | ConvertFrom-Json
    if ($reference.PSObject.Properties.Name -contains 'experiment') { throw 'Unexpected CPU experiment' }
    $e = $manifest.experiment
    if (@($e.PSObject.Properties).Count -ne 7 -or $e.kind -ne 'room-cuda-session' -or `
        $e.control -ne 'production Session' -or $e.finite_fade -ne $true -or `
        $e.cpu_reference -ne $false -or $e.binding -ne 'prepared-memory' -or `
        $e.eq_stagger_frames -ne 256 -or $e.compact_control -ne $true) {
        throw 'GPU experiment is outside the retained V2 contract'
    }
    $manifest.PSObject.Properties.Remove('experiment')
    $copy = Join-Path $gpu 'manifest-rounding.json'
    [IO.File]::WriteAllText($copy, ($manifest | ConvertTo-Json -Depth 100), [Text.UTF8Encoding]::new($false))
    python scripts/room/compare-realtime.py (Join-Path $cpu 'manifest.json') $copy `
        --output (Join-Path $ReplayRoot ($case.name + '-comparison.json'))
    if ($LASTEXITCODE) { throw 'Numerical gate failed' }
}
```

所有测试输出目录必须为新目录。移位检查只重命名其自建副本；
MathDx 隐藏试验由私有编排负责并在 finally 中恢复。

分项数据、源码／二进制身份、最终包身份与原始证据归档哈希见
[validation.json](evidence/room-cuda-product/validation.json)。
本次依赖安装仅增加共享 MathDx 构建缓存；测试用私有资源副本及兼容路径在归档后清理。
