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
