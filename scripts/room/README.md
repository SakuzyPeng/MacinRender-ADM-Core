# 房间包工具

运行时只使用二进制组件，不需要 Python。生成阶段使用 NumPy、SciPy；冻结参考包转换另外
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

新增算法类型需要新的 `kind` 及对应 Rust 实现；不要只修改版本字符串伪装兼容。当前仅
接受 `hybrid-shoebox-v1` 和 `delay-aligned-v1`。新算法必须实现不可变准备、会话处理、
确定尾长、reset 与双实例切换契约，并提供各平台相同资源/事件的逐位回归。

## Linux 性能复测

复用现有 Cargo / CMake 构建缓存，使用 Release `room_bench`。可选的 `room-profile` Cargo
feature 启用固定大小的跨线程阶段计数器；普通构建不读计时器、不更新计数器。阶段耗时是
各调用的墙钟时间之和，包含嵌套与并行重叠，不能相加作为 CPU 时间。`worker_roundtrip`
与 `worker_execution` 的差包含排队、唤醒与交接；`session_process` 减 `canonical_block`
包含输入控制、校验和状态查询。只在没有正在运行的 DSP 调用时 reset / snapshot。

`room_stages ROOM_JSON [ITERATIONS=2000]` 输出单声源各阶段的平均耗时 JSON。
`room_bench ROOM_JSON [CALLS=384] [FRAMES=256|512] [SOURCES=1|16|32] [SWITCH_PERIOD_CALLS=0]`
可指定声源数，缺省遍历三种数量。切换周期为 0 时只切换一次，非零时交替切换两个已准备的房间，
周期必须容纳预热、过渡和排尾。会话实例的分配与排队耗时单列为
`preparation_outside_dsp_seconds`，不计入单次 DSP 期限；进程 CPU 仍包含这部分工作。
这仍是内核压力测试，不模拟设备回调或异步准备线程。

关闭 `room-profile` 并重新构建后才运行正式容量测试。例如，`ROOM_BIN` 指向现有 Release
可执行文件，`ROOM_JSON` 指向固定资源：

```sh
python3 scripts/room/benchmark-linux.py "$ROOM_BIN" "$ROOM_JSON" /path/to/new-sweep \
  --workers 8 1 4 16 32 --repeats 3 --blocks 384
python3 scripts/room/benchmark-linux.py "$ROOM_BIN" "$ROOM_JSON" /path/to/new-long-run \
  --workers 8 --repeats 3 --blocks 11250 --frames 512 --sources 32 --switch-period 469
```

每种状态分别输出静态、连续头姿、连续头姿加房间切换的结果。11250 次 512 帧调用对应
每个场景 120 秒音频；469 次调用约为 5 秒。脚本串行运行各组，保存可执行文件与清单哈希、
CPU 配额、允许的核心、限流/压力快照及子进程 CPU、缺页、上下文切换增量。
输出目录必须不存在；不要同时运行构建或其他基准。系统/cgroup 计数也可能包含其他进程，
不能直接把系统总量归因于 DSP，更不能把内核期限超时报告为设备欠载。
