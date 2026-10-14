# 房间包工具

现成预设通过仓库的 [Git LFS 房间资源目录](../../resources/rooms/README.md) 分发，
可直接配合个人 SOFA 使用；获取和加载无需运行制作脚本。

房间运行时不需要 Python。个人 SOFA 由原生代码自动准备；房间预设制作脚本使用 NumPy、SciPy，冻结参考包转换另外
需要 h5py、pyroomacoustics 及已有的连续 HRIR 代码。生成环境版本写入清单，跨平台验证
必须分发同一份资源字节，而不是在三个平台分别随机生成资源。

- `pack-reference.py` 将已有冻结组件转为 `room.json` / `hrir.json`；不生成全方向 BRIR 库。
- `generate-room.py` 用 `room-template.json` 生成同类新房间。模板包提供频带 FIR、传播核、
  EQ 和世界方向；定义文件提供几何、早反射吸收和八频带晚场 RT60 / 能量。新晚场采用
  固定种子的有限噪声衰减 FIR，不含运行时反馈，不继承冻结房间的声学通过结论。
- `mradm room-check room.json --hrir /path/to/hrir.json` 检查版本、形状、有限值、哈希、滤波支持
  并准备 FFT，输出资源身份、尾长和缓冲延迟。缺省 `--hrir` 读取包内默认绑定。
- `validate-reference.py` 调用 **Release** `room_impulse`，在验证端显式恢复参考测量的距离
  校准，检查 22.2 的 22 个全频参考位和 6 个补点；临时响应用完即删。
- `compare-replays.py` 比较 Release `room_replay` 的准备系数、HRIR / 分支、PCM 及帧数。
  输入为 `macos-arm64=.../manifest.json windows-x64=.../manifest.json linux-x64=.../manifest.json`，
  使用 `--output report.json` 保存报告。`--allow-partial` 可记录已完成的平台，但报告不会把缺少平台当作通过。

新增预设示例：

```sh
python3 scripts/room/generate-room.py \
  --template-package /path/to/reference/room.json \
  --definition /path/to/my-room-definition.json --output /path/to/new-room
build/release/mradm room-check /path/to/new-room/room.json --hrir /path/to/reference/hrir.json
build/release/mradm render -i input.wav -o output.wav --renderer room-binaural \
  --room /path/to/new-room/room.json --room-hrir /path/to/reference/hrir.json --room-metres-per-unit 1.5
```

默认不复制个人 HRIR；播放时独立选择，切换时固定当前个人 HRIR。需要独立分发的包时才
加 `--include-hrir`。所有输出目录必须不存在，生成器不会覆盖另一房间。
v1 动态切换保留相同的晚场世界方向基与算法保护延迟；生成器从模板继承这些组件。
不兼容的包可创建独立会话，不能在现有会话内交叉淡化。

## 直接使用个人 SOFA

`room-binaural` 会在创建会话时原生读取 SOFA、准备 48 kHz FIR、处理 `Data.Delay` 并构建
插值网格。用户不需要运行 Python，也不需要生成 `hrir.json`。准备过程在播放前执行，
活动数据完全保存在内存中；同一 SOFA 的活动会话共享准备数据，结束后可回收，不建立磁盘缓存。

```sh
build/release/mradm room-check /path/room.json --sofa /path/personal.sofa
build/release/mradm render -i programme.wav -o binaural.wav --renderer room-binaural \
  --room /path/room.json --sofa /path/personal.sofa
```

离线渲染、文件流式渲染和 Scene 流式渲染都支持该入口。C API 沿用
`adm_render_options_set_sofa_path` 或 Scene 配置中的 `rendering.sofa_path`；既有公开结构和
函数签名不变。房间专用原生导入在 `MR_ADM_ENABLE_SOFA=OFF` 的构建也可用，该选项继续控制
其它双耳后端的可选 SOFA 支持。`--room-hrir`／`adm_room_config_t.hrir_manifest` 也接受 SOFA，
并继续兼容原有 HRIR JSON 包。SOFA 单独指定时优先于房间默认绑定；如果同时显式指定 HRIR 包
和 SOFA，两者仍须匹配来源哈希。换房保留当前 HRIR，换个人 SOFA 须创建新会话。

原生入口支持双耳、固定监听点、单半径、完整方向覆盖的 SimpleFreeFieldHRIR／GeneralFIR。
保留幅度和完整响应，支持不同采样率及以样本或秒声明的 `Data.Delay`；保护延迟单独报告。
不完整几何、无法识别的左右耳、异常数据或不支持的 HDF5 编码会报错，不静默回退到默认 HRIR。

### 多距离 HRTF：房间直达声

`--room-hrir`、`room-check --hrir` 和 `adm_room_config_t.hrir_manifest` 也接受
`mradm.hrtf-collection` v1 的 `distance_index.json`。当前支持 48 kHz、双耳
`HRIR_DFEQ100`，各层必须具有相同的方向顺序、监听耳序和一个明确的参考层。
加载时验证相对路径、文件长度、SHA-256、实际 SOFA 半径和层间几何；任何一层失败都会拒绝准备。

```sh
build/release/mradm room-check /path/room.json --hrir /path/collection/distance_index.json
build/release/mradm render -i programme.wav -o binaural.wav --renderer room-binaural \
  --room /path/room.json --room-hrir /path/collection/distance_index.json --room-metres-per-unit 1.5
```

直达声使用房间约束后的实际米数：相邻距离层线性加权，与球面三角形的方向权重相乘，
逐耳对齐时延后合成最多六个 HRIR。head-locked 直达声及 extent/divergence 采样点使用同一查询。
早期反射、晚场解码固定使用合集参考层。离线渲染、文件流式渲染和 Scene 流式渲染共用此实现。

HRTF 超出集合范围时保持端点层；传播增益和延时仍只按实际距离计算一次。
例如 SkinRLC 九层覆盖 0.2–1.6 m，但房间若限制最小距离为 0.25 m，实际最近位置仍是 0.25 m。
合集必须已经去除公共自由场传播增益和延时，使用跨层一致的 EQ；加载器不重新均衡或归一化。
公共保护帧按全部层计算，非直达分支使用相同时间基准，完整 FIR 不按能量截短。

所有层在准备阶段驻留内存，共享方向网格、分数延时和相位表；整组准备预算为 1 GiB，
不建立磁盘缓存，处理阶段无文件访问或分配。换房固定当前集合；改变集合须创建新会话。
同时传入 `--sofa` 时，其文件必须匹配集合参考层。既有单 SOFA 和预处理包继续走原数值路径。

检查 JSON 的 `hrtf_collection` 报告半径、参考层、逐层来源摘要，以及可选的
`provenance/source_nearfield_quality_report.json`。这些质量记录只用于报告，不触发滤波修改。
`mr_adm_room_hrir_check distance_index.json` 扫描全部节点、距离中点和层边界，并保留来源标记。
节点相对 L2 门槛为 `2e-5`，边界两侧 ±1e-7 m 的响应差门槛为 `2e-4`；通过数值检查不等于声学精度或试听认证。

### 可选的离线打包工具（开发／兼容用途）

以下脚本用于复现或分发旧格式资源包，不是播放或导入 SOFA 的必要步骤。

`prepare-hrir.py` 独立读取 SimpleFreeFieldHRIR / 适用的 GeneralFIR，无需个人研究脚本。
使用现有的 NumPy、SciPy、h5py 环境；只支持双耳、固定监听点、单半径且包围监听点的方向网格。
多半径、变化的监听者、重复方向、网格孔洞、非有限值及超过 v1 完整 FIR 预算的输入会明确拒绝。
不进行逐耳/逐方向归一化，也不按能量截短响应。非 48 kHz 数据离线重采样，FIR 系数按采样间隔
补偿以保留传递增益；`Data.Delay` 的整数、分数及负值都保留。

```sh
cmake --build --preset release --target mr_adm_room_hrir_check
python3 scripts/room/prepare-hrir.py /path/personal.sofa --output /path/new-hrir \
  --check-executable build/release/mr_adm_room_hrir_check
build/release/mradm room-check /path/room.json --hrir /path/new-hrir/hrir.json
```

耳序默认根据监听者坐标下的 `ReceiverPosition` 识别；含糊的数据须显式传 `--ear-order lr|rl`。
`--faces` 可保留原始三角网格（N×3 的 JSON、NPY 或文本；`--faces-index-base 0|1`），否则生成
确定顺序的凸包网格。输出目录必须不存在。工具记录源 SOFA 哈希、依赖版本、耳序和各保护延迟。
重采样及 `Data.Delay` 带来的共同因果保护延迟为 `preprocessing_guard_samples`，运行时查询报告为
`hrir_preprocessing_guard_frames`；它与 HRIR 插值、传播、材料和处理 FIFO 的延迟分别记账。

原生 `room_hrir_check` 对全部测量节点执行实际 Rust HRIR 查询，验证幅度和时序重建，相对 L2
门槛为 `2e-5`。传入 `--check-executable` 才会生成通过的 `node-validation.json`；仅成功打包
不代表节点回归、声学或实时容量已通过。固定依赖版本生成一份资源后，将同一字节分发给各平台。

Python 回归：`python3 -B tests/unit/room_hrir_prepare_test.py build/release/mr_adm_room_hrir_check`。
CMake 可用 `MR_ADM_ROOM_PREP_PYTHON` 指定已有环境，避免另外创建依赖缓存。

新增算法类型需要新的 `kind` 及对应 Rust 实现；不要只修改版本字符串伪装兼容。当前仅
接受 `hybrid-shoebox-v1` 和 `delay-aligned-v1`。新算法必须实现不可变准备、会话处理、
确定尾长、reset 与双实例切换契约，并提供各平台相同资源/事件的逐位回归。

## Linux 性能复测

复用现有 Cargo / CMake 构建缓存，使用 Release `room_bench`。可选的 `room-profile` Cargo
feature 启用固定大小的跨线程阶段计数器；普通构建不读计时器、不更新计数器。阶段耗时是
各调用的墙钟时间之和，包含嵌套与并行重叠，不能相加作为 CPU 时间。`worker_roundtrip`
与 `worker_execution` 的差包含排队、唤醒与交接；`session_process` 减 `canonical_block`
包含输入控制、校验和状态查询。只在没有正在运行的 DSP 调用时 reset / snapshot。

定位偶发慢调用时，使用 `room-trace` feature 构建 Release `room_bench` 并追加 `--trace`。
它包含 `room-profile`，但关闭 HRIR / 响应 / 滤波器等内层算术计时，保留声源任务组、单个
声源任务、晚场和线程交接阶段。计数按工作线程分开存放，以减少计数器自身的跨核争用。
Linux 64 位和 macOS 同时记录执行线程 CPU 时间；其它平台的原生 CPU 时钟当前不可用，
以 `thread_cpu_available=false` 明确标注。原生时钟封装只在诊断示例中，DSP crate 仍禁止 unsafe。

沿用已有的 `--target-dir`、`--target` 和 Rust 构建参数，只增加 `--features room-trace`，然后运行：

```sh
"$ROOM_TRACE_BIN" "$ROOM_JSON" 11250 512 32 469 --trace > room-call-trace.jsonl
```

每轮预分配并触碰调用记录内存，运行后保留最慢 32 次调用的 block、切换阶段及墙钟 / CPU
阶段计数。`--trace` 必须显式启用，普通 Release 构建会拒绝该参数。CPU 时间同样存在嵌套
重叠；任务组驱动线程的 CPU 时间不包含其它线程，因此墙钟与驱动线程 CPU 的差不能直接
等同于系统抢占。应结合 `voice_render` 等任务计数分析。诊断会改变时序与 RSS，结果只能用于
定位；容量和设备通过结论必须来自无插桩运行。需要内层墙钟细分时，单独使用 `room-profile`。

`room_stages ROOM_JSON [ITERATIONS=2000]` 输出单声源各阶段的平均耗时 JSON。
版本 2 新增 `supported_direct_target_us` / `supported_early_target_us`：预先生成八个不同头姿的
完整响应，按实际结构支持长度构建目标；每轮推进卷积时钟，计时只包含目标更新，排除响应生成
和卷积处理，先预热 32 轮。支持长度另列于 `direct_support_taps` / `early_support_taps`。
原 `stereo_target_us` 仍保留整条 FIR、同一采样点替换目标的微基准，二者不可混为同一负载。
`room_bench ROOM_JSON [CALLS=384] [FRAMES=256|512] [SOURCES=1|16|32] [SWITCH_PERIOD_CALLS=0]`
可指定声源数，缺省遍历三种数量。切换周期为 0 时只切换一次，非零时交替切换两个已准备的房间，
周期必须容纳预热、过渡和排尾。会话实例的分配与排队耗时单列为
`preparation_outside_dsp_seconds`，不计入单次 DSP 期限；进程 CPU 仍包含这部分工作。
这仍是内核压力测试，不模拟设备回调或异步准备线程。

关闭 `room-profile` / `room-trace` 并重新构建后才运行正式容量测试。例如，`ROOM_BIN` 指向现有 Release
可执行文件，`ROOM_JSON` 指向固定资源：

```sh
python3 scripts/room/benchmark-linux.py "$ROOM_BIN" "$ROOM_JSON" /path/to/new-sweep \
  --workers 8 1 4 16 32 --repeats 3 --blocks 384
python3 scripts/room/benchmark-linux.py "$ROOM_BIN" "$ROOM_JSON" /path/to/new-long-run \
  --workers 8 --repeats 3 --blocks 11250 --frames 512 --sources 32 --switch-period 469
```

每种状态分别输出静态、连续头姿、连续头姿加房间切换，以及连续头姿加位置斜坡四个场景。
位置斜坡场景（`source_ramps`）每次调用让每个声源在各自的偏移处开始一段覆盖整次调用的斜坡，
`distinct_event_offsets` 为每次调用的不同偏移数；用 `room-profile` 构建时另输出
`voice_segments`（声源分段数）及每声源每块的平均值，普通构建为 `null`。11250 次 512 帧调用对应
每个场景 120 秒音频；469 次调用约为 5 秒。脚本串行运行各组，保存可执行文件与清单哈希、
CPU 配额、允许的核心、限流/压力快照及子进程 CPU、缺页、上下文切换增量。
输出目录必须不存在；不要同时运行构建或其他基准。系统/cgroup 计数也可能包含其他进程，
不能直接把系统总量归因于 DSP，更不能把内核期限超时报告为设备欠载。

## 全向声学检查与校正包

`room_omni` 用理想全向单位脉冲接收器驱动同一房间内核，导出直达、早反射和晚场三个
独立通道。它不使用个人 HRIR；全向接收器的 40 帧因果保护延迟写入探针元数据。完整响应
参与分析，临时 PCM 每测完一个位置就删除。

```sh
cmake --build --preset release --target mr_adm_room_omni
python3 scripts/room/check-acoustics.py --probe build/release/mr_adm_room_omni \
  --room /path/reference/room.json --output /path/acoustic-report.json
```

缺省使用 `reference-positions.json` 的 22 个参考位置和 6 个补点。T20 按 EBU Tech 3276
§2.3 的 −5 至 −25 dB 区间计算；T30、拟合 R² 和分析滤波器的衰减时间另行保留。
采用十进制中心频率 `1000·10^(k/10)`，同时检查体积标称 Tm 与各点测得 Tm 的容差；
160/200 Hz 边界保留既有的 0.05 s 更严限制。相邻频带差、15 ms 内 1–8 kHz 连续频带
反射（含重合路径相干叠加）及 50 Hz–16 kHz 频响采用
BS.1116-3 与 EBU 3276 规定的交集。报告同时给出资源完整性和独立的声学通过状态。
测量限定在清单给定的几何/监听点，并预先采用固定的 `r / 2.1 m` 声源电平校准；原始未校准
频响、分支能量也输出。频响使用全部参考位置共同的参考电平，不按各方向拟合或归一化增益。
同时保留既有标准的逐曲线参考电平检查；这只是分析量，没有修改实际响应或声源增益。
这些检查不包含真实房间、背景噪声、耳机或完整标准认证。

`reference-corrected-v1.json` 是新资源 `reference-7x6x3.2-corrected-v1` 的制作定义。
它要求冻结模板的精确晚场组件哈希；原冻结包保持原样。`template-orthogonal-mix-v1`
对全部世界方向 FIR 使用一个共同的 Householder 正交变换，变换前后每个时刻的方向总能量
相同（浮点误差单列），再施加共同的指数包络及全局晚场发送量。它保留所有采样点，既不
针对声源方向/耳朵作增益归一化，也不削除反射路径。所有修改均在离线制作阶段，运行时
仍使用现有 `hybrid-shoebox-v1`。

```sh
python3 scripts/room/generate-room.py --template-package /path/reference/room.json \
  --definition scripts/room/reference-corrected-v1.json --output /path/reference-corrected-v1
build/release/mradm room-check /path/reference-corrected-v1/room.json --hrir /path/reference/hrir.json
python3 scripts/room/check-acoustics.py --probe build/release/mr_adm_room_omni \
  --room /path/reference-corrected-v1/room.json --output /path/corrected-acoustics.json
```

完整分发需要绑定的 HRIR 时加 `--include-hrir`。生成一次后分发同一份资源字节；清单记录
定义、工具、模板哈希以及 NumPy/SciPy 版本。既定条件下的结果见
[校正包声学报告](../../docs/architecture/ROOM_CORRECTED_ACOUSTIC_VALIDATION.json)，其中包括
所有 T20/T30、R²、频响和反射数据；不能外推到任意位置、距离或实际耳机。

## 无 GUI 设备与 OSC 验收

```sh
cmake --build --preset release --target mr_adm_room_scene_benchmark
build/release/mr_adm_room_scene_benchmark --list-devices
python3 scripts/room/device-probe.py --probe build/release/mr_adm_room_scene_benchmark \
  --room /path/reference/room.json --other-room /path/variant/room.json \
  --device 'exact output device name or ID' --seconds 1800 --sources 32 --output /path/new-device-evidence
```

驱动器以 100 Hz 发送 PoseBridge v3 OSC 数据，探针使用实际接收器应用头姿，约每 5 秒切换
一次房间。测试信号为确定性宽带噪声，会按会话音量实际播放；缺省 `--volume 0.1`，不改
系统音量。名称/ID 必须唯一匹配当前设备，不会在选择失败时静默改用另一设备。
`--device null` 可先检查调用链；它不构成真实设备验收。
`--quality exact|realtime` 显式选择档位，缺省仍为精确档。`--source-ramps` 让每个声源
每 512 帧提交一次持续 512 帧的位置斜坡，各源事件偏移不同；它会与头追和换房同时运行。
两项配置都写入证据，避免把静态声源或精确档结果误作实时档的密集运动验收。
`--single-room-switch` 只在开头请求一次换房，之后保持房间固定，用于隔离周期换房的影响。
Windows 发送线程使用独立高精度等待定时器，采样时间按 10 ms 逻辑时钟递增，主机时间另行记录。
接收包数须达到素材时长 × 100 Hz 的 99%，应用比例也须达到 99%，并且没有拒包、丢包或发送错误；
仅把低频率收到的包全部应用不能通过。原生探针在素材结束后继续更新头姿、完成排尾，停止请求新房间。

记录包括有效/预期输出帧、EOS、错误、欠载事件、每秒 RSS、控制调用及接收至应用时间分布、
输出回调间隔和队列深度。录音、计数和时序缓冲在准备时分配并预触碰；音频回调只写入固定
缓冲。后半段 RSS 斜率和完整时间序列均保留，需结合有界状态与零分配回归判断资源增长。
欠载计数表示应用可观察到的输出回调供数不足；它不覆盖驱动未上报的硬件故障或模拟输出失真。

可添加 `--capture 'BlackHole 16ch'` 指定虚拟输入，或在 Windows 添加 `--capture-loopback`
使用所选 WASAPI 输出端点回环。每分钟保存一个一秒测量窗口，运行后执行：

```sh
python3 scripts/room/analyze-loopback.py /path/new-device-evidence/recordings \
  --output /path/new-device-evidence/loopback-latency.json
```

分析要求足够的相关度和唯一相关峰。输出测量起点为输出回调 PCM，终点为输入回调 PCM；
不包含传感器、OSC 接收、渲染计算及预渲染队列。时间戳来自回调入口，不是硬件采样/呈现
时间；报告的不确定度下限包含输入/输出最大回调周期与一个样本，驱动时间戳约定、时钟漂移、
重采样及调度误差仍未校准。虚拟回环、软件队列或内核 257 帧结果都不能作为传感器到耳机
的物理延迟。回环设备若静音/电平为零，结果会缺少有效相关窗，不能算测量通过。

文件流式渲染可通过独立的 Release 探针验证正式 `MonitorSession` 路径，包括 ADM 导入、
PCM 文件解码、后台渲染、监控处理和真实音频设备：

```sh
cmake --build --preset release --target mr_adm_room_file_device_probe
build/release/mr_adm_room_file_device_probe programme.wav \
  /path/reference/room.json /path/variant/room.json 'exact current output device ID'
```

该探针固定为实时档，直接以 100 Hz 调用头姿 API，每 5 秒请求换房，并播放至文件及房间
尾声排空。它报告实际帧数、欠载、控制请求、峰值电平、RSS 和诊断，不计算传感器延迟。
文件监控的 `ended` 表示生产端 EOS；验收还需等待输出环形缓冲排空并核对完整帧数。
