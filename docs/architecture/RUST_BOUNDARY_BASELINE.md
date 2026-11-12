# 方向与基线（六期 6.1）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)六期 6.1，路线见
> [Rust 主导逆转路线](RUST_INVERSION_ROADMAP.md)，五期结项见 [RUST_ORCHESTRATION_CLOSEOUT.md](RUST_ORCHESTRATION_CLOSEOUT.md)。
> 本切片只改一致性脚本、测试工具、CMake 测试目标与文档；生产代码、PCM 与内核输出都不变。

## 方向

六期把边界迁入 Rust：岛改为由 Rust 直接调用（6.2），C ABI 由 Rust cdylib 实现（6.3），CLI 改为 Rust（6.4），
删除 C++ 外壳与 SDK（6.5），构建以 Cargo 为主（6.6）。切片表与用户决定（2026-11-12）见审计六期一节。
6.1 给出三份清单（C ABI 家族、CLI 契约、岛入口）、规模基线，以及六期切换前用来证明新旧实现逐字节相同的对照工具。

## C ABI 清单

`include/adm/c_api.h` 声明 164 个函数，按 6.3 的迁移家族划分如下；机器可读的清单是
[`scripts/consistency/capi-families.json`](../../scripts/consistency/capi-families.json)，
`tests/unit/boundary_tools_test.py` 校验它与头文件、GUI P/Invoke 列表和采集工具保持一致。

| 家族 | 切片 | 内容 | 导出 | GUI 使用 |
| --- | --- | --- | ---: | ---: |
| B | 6.3 B | 版本、上下文与内存 | 7 | 7 |
| C | 6.3 C | render / inspect / export / room / 能力表（含 render options、cancel token、scene info） | 80 | 55 |
| D | 6.3 D | preview | 4 | 3 |
| E | 6.3 E | Scene 流与 Scene 输出 | 38 | 0 |
| F | 6.3 F | Monitor、HpTF 文本解析与 OSC 头追踪 | 35 | 17 |
| 合计 | | | 164 | 82 |

`adm_render_options_set_monitor_system_spatial` 虽带 monitor 字样，属于 render options，归 C 家族；`adm_destroy_scene_info`
归 C 家族。GUI 使用数按 `gui/MacinRender.Gui/Interop/NativeMethods.cs` 中出现的函数名统计。

C ABI 采集工具（见“对照工具”）引用了其中 163 个导出：
- 完整采集 125 个：返回码、公开结果字段、错误消息、JSON、PCM 与输出文件都记录；
- 部分采集 38 个，清单逐个写明原因：Monitor 的 25 个只采集参数校验（空句柄、空输入路径），因为 monitor 会打开音频输出设备；
  Scene 输出的 8 个只采集参数校验，NULL 类型的输出以暂停状态采集；Scene 流的房间状态与切换只采集错误路径，因为房间准备是异步的；
  OSC 的快照与姿态只采集存在性与收到数据报之前的状态，因为它们带端口与接收时间；
- 不采集 1 个：`adm_monitor_output_devices_json`，它列出宿主机的音频设备。

部分采集与不采集的行为由现有 C ABI 测试（`mr_adm_c_api_tests`、`mr_adm_scene_output_c_api_tests`、
`mr_adm_hptf_parameters_tests`、`mr_adm_osc_head_tracking_tests` 等）和维护者的主机检查覆盖，
6.3 E、F 迁移这些家族时如需逐字节对照，再按家族补充可确定的调用序列。

## CLI 契约

`mradm` 有 8 个公开子命令和 1 个内部子命令，定义在 `src/adm_cli/`：

| 子命令 | 参数与选项 | 输出 |
| --- | --- | --- |
| `render` | `-i/--input`（必填）、`-o/--output`、`-v/--verbose`，以及 40 个选项：输入映射（`--input-layout` 与 `--input-channels` 互斥）、输出选择（`--output-layout`、`--renderer`、`--output-bit-depth`）、计量与增益（`--no-peak-limit`、`--peak-limit-dbtp`、`--peak-normalize-to-limit`、`--loudness-target`、`--final-gain-db`）、裁剪（`--start`、`--end`）、平滑（`--interp-ms`、`--object-smoothing-frames`）、编码（Opus / APAC / IAMF）、双耳与房间（`--sofa`、`--room`、`--room-quality` 需 `--room`、`--room-hrir`、`--room-metres-per-unit`、`--screen-config`）、语义策略（`--semantic-policy`、`--write-semantic-report`）、扬声器与 DirectSpeakers（`--speaker-geometry`、`--speaker-spread-mode`、`--direct-speakers-routing`、`--direct-speakers-matrix`、`--lfe-routing`、`--triple-balance-mode`、`--binaural-spread-mode`）、Apple（`--apple-spatial-preset`、`--apple-speaker-rendering-flags`）与听者朝向 | 输出文件；日志 |
| `inspect` | 位置参数 `file`、`--xml`、`--write-semantic-policy-template` | 场景文本或 AXML；模板文件 |
| `backends` | 无 | 后端、能力与布局表 |
| `layouts` | `--format`（必填）、`--layout`、`--renderer` | 声道顺序表 |
| `input-layouts` | `--format text/json`、`--layout` | 输入布局目录 |
| `formats` | 无 | 容器格式表 |
| `export` | `-i`、`-o`（必填）、`--semantic-policy` | 新的 ADM BWF |
| `room-check` | 位置参数 `manifest`、`--hrir/--sofa`、`--quality exact/realtime` | 检查与准备结果 JSON |
| `__apac-encode`（内部） | `--in`、`--out`、`--layout`（必填）、`--bitrate`、`--drc`、`--container` | APAC 编码子进程，带心跳协议 |

另有全局的 `--help` 与 `--version`（`mradm 1.9.1`）。

输出通道：
- 子命令的正文与日志都写到 stdout。日志由 spdlog 默认 logger 写出，格式为 `[level] message`，`-v` 打开 debug 级（含 `progress N%: …`）；
  颜色只在终端上出现，重定向时没有转义码。
- CLI11 的参数解析错误写到 stderr，`--help` 写到 stdout。

退出码：

| 退出码 | 含义 | 来源 |
| ---: | --- | --- |
| 0 | 成功，含 `--help` 与 `--version` | |
| 1 | 领域错误（输入无效、组合不支持、IO 失败等），消息经日志写到 stdout | `EXIT_FAILURE` |
| 105 | 取值校验失败（枚举不在列表内、十进制越界或非有限、整数越界或无法转换） | CLI11 `ValidationError` |
| 106 | 缺少必填项（必填选项、位置参数或子命令） | CLI11 `RequiredError` |
| 107 | 缺少依赖的选项（`--room-quality` 需要 `--room`） | CLI11 `RequiresError` |
| 108 | 互斥选项同时出现（`--input-layout` 与 `--input-channels`） | CLI11 `ExcludesError` |
| 109 | 多余的参数或未知选项 | CLI11 `ExtrasError` |

按用户决定 1，6.4 的 Rust CLI 必须保持选项名、取值与默认值、stdout、输出文件、退出码与领域错误的中文消息；`--help` 的排版与
参数解析错误的措辞可以变化，写进发布说明，退出码要按上表映射。

固定命令表 [`scripts/consistency/cli-commands.json`](../../scripts/consistency/cli-commands.json) 有 102 条命令，
覆盖全部 8 个公开子命令与 `__apac-encode`、`render` 的全部 43 个选项，以及上表的每个退出码。每条命令有一个类别：
所有类别都比较退出码与工作目录内的文件变化；`contract` 还比较 stdout、stderr，`help` 与 `parse-error` 只豁免文字差异并单独列出。
本机记录中 77 条是 contract（53 条成功、24 条领域错误），3 条 help，22 条 parse-error。

## 岛入口清单

五期中，Rust 状态机在需要岛时暂停，把岛请求交回 C++ 执行。六期入口改为 Rust 之后，这些点改为由 Rust 调用岛提供的窄 C 接口（6.2）：

| 岛 | 现在的 C++ 入口 | 现在的执行方 | 6.2 去向 |
| --- | --- | --- | --- |
| FLAC / Opus MKA / IAMF / APAC 编码 | `convert_to_flac` / `convert_to_opus_mka` / `convert_to_iamf` / `convert_to_apac`（`src/adm_audio`，读已写完的 float WAVE） | `RenderService` 执行岛请求 4–7 | 窄 C 接口，保持“读 float WAVE、写目标文件”的形状 |
| 容器元数据 | `write_file_metadata`（FLAC Vorbis comment、Matroska tags、MP4 udta；WAV / CAF 已在 Rust） | 岛请求 9 | 同上 |
| IAMF 的 MP4 打包 | 打包器探测（mp4box 优先于 ffmpeg ≥ 7，`fork` + `execvp` / `CreateProcessW`）与打包 | 岛请求 1、8 | 同上 |
| APAC 编码子进程 | `run_apac_encode_child`（`mradm __apac-encode`，心跳协议；库侧经 `MRADM_APAC_HELPER` 或同目录的 `mradm` 找到子进程） | CLI 子命令 | 6.4 的 Rust CLI 提供 `__apac-encode`，编码本身留在 Apple 岛 |
| Apple AUSpatialMixer 后端 | `apple_renderer()` → `IRenderer`（`src/adm_apple`，总线计划已在 Rust `mradm-render` `apple`） | 离线：岛请求 2、3；监听：`monitor_session` 打开岛流 | 窄 C 渲染接口（6.2 B 完成：`mradm_apple_islands`）；Apple 内部停用 `IRenderer` / `IRenderStream` 按用户决定推到 6.5 |
| 岛流表 | `island_stream.cpp`：把 C++ `IRenderStream` 交给 Rust Monitor | `MonitorSession` | 随 Apple 渲染接口改为窄 C 接口（6.2 B 完成：生产代码移入 Apple 岛，`island_stream` 移到 `tests/support`） |
| 系统空间 sink | `make_avsamplebuffer_device`（macOS）、`make_spatialaudioclient_device`（Windows） | `monitor_session` / `scene_output_session` 的宿主表 | 设备接口窄 C 化（6.2 C 完成：`mradm_device_islands`，设备由 Rust 打开与驱动） |
| miniaudio 设备 | `make_miniaudio_device`（含 NULL 后端与设备枚举） | 同上 | 留作 C 岛，经窄 C 接口（6.2 C 完成，同上） |
| CUDA、room 浮点环境、Windows MMCSS 与定时器 | 已在 `mradm-ffi`，经 `Host` trait | Rust | 不变 |

## 发现

以下是 6.1 对照中看到的现有行为，采集照现状记录。是否修正由对应切片决定，改动时作为有意的行为变化单独成片：
- **Monitor 创建的旧错误消息。** `adm_create_monitor(ctx, NULL, …)` / `adm_create_monitor_ex` 返回 INVALID_ARGUMENT，但不更新上下文的
  错误消息，`adm_context_last_error_message` 读到的是上一次失败调用（采集中是 HpTF 解析）留下的消息。归 6.3 F。
- **未知扩展名。** `mradm render -o out.xyz` 退出 0，写出 float32 RF64 WAVE。归 6.4。
- **dr_flac。** 生产代码不再解码 FLAC：`FloatFlacReader`（dr_flac）只剩 `tests/support/audio_reader` 与 FLAC 冒烟测试在用。
  dr_flac 的迁移按用户决定暂缓，6.5 删除 C++ SDK 时再决定它的去处。
- **头文件冒烟测试不加载库。** `mr_adm_c_api_header_smoke` 不调用任何函数，链接时没有依赖库，换库运行时库损坏它也通过，
  因此不列入换库测试，只保留原 ctest 目标。

## 对照工具

三种对照都在同一构建、同一平台上比较两份记录，不作为跨平台证据，CI 不跑。比较只做下列事先列明的归一化
（`scripts/consistency/boundary.py` `NORMALIZATIONS`），其余每个字节都比较：
1. **root**：采集时工作目录的绝对路径换成 `<root>`；
2. **clock**：落在本记录自身运行时间窗内（开始前 1 s 到结束后 1 s）的 ISO 8601 UTC 时间换成等长的 `0000-00-00T00:00:00Z`
   （FLAC、CAF、Matroska、MP4 的日期标签）；
3. **bext clock**：RIFF / RF64 / BW64 中 bext 的 OriginationDate 与 OriginationTime（载荷 320–338 字节），同样只在时间窗内替换；
4. **sidecar**：文本里 `.render_tmp` / `.gain_tmp` / `.trim_tmp` 临时文件名中的 16 位随机十六进制换成 `<token>`；
5. **matroska uid**：Matroska 文件 Segment/Tracks/TrackEntry/TrackUID 的载荷（`opus_mka_io.cpp` 每个文件随机抽取）清零，
   沿 EBML 结构定位，到第一个 Cluster 为止，不扫描音频数据。
6. **MP4 clock**：`moov/mvhd`、`moov/trak/tkhd`、`moov/trak/mdia/mdhd` 的创建与修改时间，在上述运行时间窗内时清零；
   按 1904 年纪元解释 version 0 的 32 位或 version 1 的 64 位字段，保留其余头字段及 `mdat` 内容。

时间、bext、Matroska 与 MP4 都按结构或时间窗定位；单元测试约束外形像 bext 时间的样本、Cluster 里像 TrackUID 的字节、
`mdat` 中像完整 MP4 时间字段的字节保持原样，时间窗之外的时间和未知版本的 MP4 头不归一化。

### CLI 记录：`cli-transcript.py`

```bash
python3 scripts/consistency/cli-transcript.py record build/consistency-a /tmp/cli-old
python3 scripts/consistency/cli-transcript.py record build/consistency-a /tmp/cli-new --tool mradm=/path/to/new/mradm
python3 scripts/consistency/cli-transcript.py compare /tmp/cli-old /tmp/cli-new --output report.json
```

`record` 在记录目录里生成夹具（`mr_adm_make_fixture`、JSON 夹具、合成房间包、SOFA、非 WAVE 文件），复制到临时工作目录后，
以相对路径运行命令表中的每条命令。逐条保存退出码、stdout、stderr 和整个工作目录内的文件变化，包括预定输出目录外的残留文件、
对夹具或先前输出的覆盖与删除。新增或修改的内容保存为 `files/<命令 id>/<工作目录相对路径>`，删除记录在 `removed_files`；
后续命令覆盖同一路径不会改变前一条的快照。采集器自身的日志和快照始终放在工作目录外。
清除 `MR_ADM_*` / `MRADM_*` 环境变量。manifest 记录源码指纹、命令表摘要、平台、构建身份（`CMakeCache.txt` 与 `mradm`、
`mr_adm_make_fixture` 的摘要）、`--tool mradm` 的路径与摘要、临时工作目录和运行时间窗。文件快照格式使用 `cli-transcript.v2`，
旧版记录需重新采集；其路径和归一化声明不足以用于新版比较。

`compare` 先核验两份记录各自的完整性（夹具、stdout / stderr、输出清单与摘要）和前提（源码、命令表、平台、夹具、构建身份相同，
CLI 摘要只能与声明的 `--tool mradm` 相符），不符时退出 2；再逐条比较，相同退出 0，不同退出 1。

### C ABI 采集：`capi-capture.py`

```bash
cmake --build build/consistency-a --target mr_adm_capi_consumers
python3 scripts/consistency/capi-capture.py record build/consistency-a /tmp/capi-old
python3 scripts/consistency/capi-capture.py record build/consistency-a /tmp/capi-new --library /path/to/new/libmradm_capi.so.1
python3 scripts/consistency/capi-capture.py compare /tmp/capi-old /tmp/capi-new --output report.json
python3 scripts/consistency/capi-capture.py tests build/consistency-a --library /path/to/new/libmradm_capi.so.1
```

采集工具 `mr_adm_capi_capture`（`tests/tools/capi_capture.cpp`）只包含 `c_api.h` 与标准库，链接共享的 `libmradm_capi`，
按 B / C / D / E / F 家族运行固定的调用序列，把每个值写进 `capture.txt`（`[段名]` 与 `键=值`，double 记位模式，字符串按 JSON 转义），
渲染输出、预览窗口与 Scene 拉取的 PCM 写进 `files/<段名>/`。不记录依赖时序的量（输出环填充、欠载、队列深度、年龄、绑定端口）。
Scene 流提交固定的整数信号与元数据更新，`signal_end` 后拉到 EOS，记录拉取总帧数、连续性与结束状态。
`--family scene` 同时覆盖 Scene 输出及房间状态／切换的错误路径，对应 6.3 E；`monitor` 对应 6.3 F。
render 家族另采集非空 options 上关闭峰值限制、关闭后恢复限制的渲染，验证 setter 对公开结果和 PCM 的影响。

`record` 把采集工具和要测的库（默认是构建目录里的库，`--library` 指定候选）复制进同一个临时目录，库按加载名命名
（Linux `libmradm_capi.so.1`、macOS `libmradm_capi.1.dylib`、Windows `mradm_capi.dll`）。可执行文件带 `$ORIGIN` / `@loader_path`
rpath（Windows 先搜索可执行文件所在目录），并清除 `LD_LIBRARY_PATH`、`LD_PRELOAD` 与 `DYLD_*`，所以加载的就是复制进去的那份。
`tests` 用同样的方式把 `mr_adm_c_api_consumer_tests`、`mr_adm_scene_stream_c_api_consumer_tests`（与原 ctest 目标同源，只链接共享库）
放到候选库旁边运行。`compare` 的前提与退出码同 CLI 记录；差异按行报告，带所在段名。

### 替换 CLI 的完整采集：`collect.py --tool mradm`

`collect.py --tool` 现在也接受 `mradm`（`collection.py` `SUBSTITUTABLE_TOOLS`），`compare-collections.py` 照旧要求二进制摘要只与
声明的替换相符。6.4 用它把新 CLI 的完整一致性采集（294 个 PCM、日志与检查点）与旧 CLI 逐字节比较。

## 规模基线

6.1 不改生产代码。`db0182e` 的生产 C/C++ 为 21,772 行，岛 3,045 行、CUDA 边界 512 行另计：

| 模块 | 行数 | 六期去向 |
| --- | ---: | --- |
| `src/adm_c_api` | 3,545 | 6.3 改为 Rust cdylib |
| `include/adm` | 3,983 | `c_api.h`（2,008）保留；内部 C++ 接口 6.5 删除或移到 `tests/support` |
| `src/adm_audio` | 4,169 | 编码器与元数据（约 3,000）作为岛保留，薄包装随调用方删除 |
| `src/adm_render_common` | 2,629 | 6.5 删除或移到 `tests/support` |
| `src/adm_engine` | 1,857 | 6.3 / 6.5 删除 |
| `src/adm_dsp` | 1,535 | 私有 FFI 头随 C++ 调用点删除 |
| `src/adm_cli` | 1,410 | 6.4 改为 Rust |
| `src/adm_realtime` | 1,366 | miniaudio 设备（215）留作岛，其余 6.3 / 6.5 删除 |
| `src/adm_metadata` / `src/adm_io` / `src/adm_render_room` / `src/adm_core` / `src/adm_renderer_factory` | 386 / 363 / 283 / 200 / 46 | 6.5 删除 |

预计六期结束时生产 C/C++ 约 5,500–6,500 行，以各切片实测为准。本切片新增的采集工具计入 tests 区域，只报告；
它在 6.3 结束、C ABI 的新旧对照完成后随七期的 C++ 测试清退一起处理。

## 验证

初始提交 `a6326614` 的 Linux x64 记录（`build/consistency-a`，Release、vendored FLAC / Opus、C ABI bundle；
该记录早于下述维护者修正，使用当时的五条归一化和 v1 CLI 记录）：
- **CLI 记录**：两份记录相隔数秒，102 条命令 0 差异；归一化前原始字节不同的 26 项全部由当时的五条规则解释。
  `--tool mradm` 换成直通脚本 0 差异；换成植入缺陷的脚本（`formats` 多一行、`room-check` 退出码改为 3、`render-final-gain` 输出翻转一个字节），
  报出 11 处差异（8 条 room-check 退出码、1 处 stdout、1 处输出文件、1 处 parse-error 类的退出码），另有 1 处 parse-error 文字差异单独列出。
- **C ABI 采集**：两份采集 3,020 行、34 个文件 0 差异；另 3 份与之并行运行（制造 CPU 争用）的采集也 0 差异。
  `--library` 指向拷贝到别处的同一份库 0 差异；指向损坏的文件时采集工具加载失败，说明加载的是复制进去的库。
  以 `LD_PRELOAD` 植入改写 `adm_api_version_patch` 与 `adm_scene_info_channels` 的 shim，报出 6 处差异；
  这项检查在 `capi-capture.py` 清除 `LD_PRELOAD` 之前完成，之后记录不再受它影响。
- **换库运行**：两个 consumer 测试在构建目录的库上通过；换成损坏的库时两者都失败。
- **单元测试**：`mr_adm_boundary_tool_tests`（14 项：归一化的五条规则及其不误伤、CLI 记录与比较、命令表覆盖、C ABI 比较、
  家族清单与头文件 / GUI / 采集工具一致）与 `mr_adm_collection_tool_tests`（`--tool mradm` 的接受与比较）通过。
- `language-surface.py --check` 通过；`format.sh --check` 只报出已知的 `binaural_render_fixture_test.cpp` 版本差异。

维护者复查发现并修正五项问题：MP4 二进制时间遗漏、峰值限制 setter 只有空句柄调用、Scene 输出误归 monitor、CLI 漏采预定输出目录外的文件、
以及 help / parse-error 错误地豁免文件差异。新版归一化为六条，CLI 记录升级为 v2。

修正代码 `1d1770b0` 的原生验证如下；后续提交只补充本节记录。两平台复用原有构建与依赖缓存，FLAC / Opus 均为 vendored、IAMF 关闭。

| 平台与配置 | CLI 两次完整采集 | C ABI 两次完整采集 | 换库 consumer 测试 |
| --- | --- | --- | --- |
| macOS arm64，Release，SOFA 开启 | 102 条命令，0 差异 | 3,399 条值、37 个文件，0 差异 | 两个均通过 |
| Windows x64，MSVC / Ninja canonical Release，SOFA 关闭 | 102 条命令，0 差异 | 3,143 条值、35 个文件，0 差异 | 两个均通过 |

- 两平台分别运行 `capi-capture.py record --family scene` 和 `--family monitor`，确认 Scene 输出及房间状态／切换错误路径归入 scene，
  monitor 采集中没有 Scene 输出段。macOS 的 C ABI consumer 目标另在 Debug 下构建通过；所有音频文件的采集与比较使用 Release。
- macOS 上以替代库把 `adm_render_options_set_peak_limit` 改为空操作，新采集明确检出关闭限制用例的公开结果及 WAV 差异。
  原始版本的采集对此返回零差异；同一缺陷导致实际禁用限制的输出从约 4.50 dBTP 变成 -1 dBTP，PCM 不同。
- macOS 上包装真实 CLI，分别在 `formats` 留下工作目录文件、在缺少输入的 `render` 命令写出 WAV；保持 stdout 与退出码不变，
  新比较分别报告两个文件差异。单元测试还覆盖嵌套路径、覆盖前的独立快照、删除、快照篡改及 help / parse-error 的文件变化。
- `boundary_tools_test.py`：19 项通过（Windows 跳过 7 项 POSIX 假程序用例）；`collection_tools_test.py`：73 项通过，既有条件子测例按原规则跳过。
  新增 MP4 测试覆盖 version 0 / 1、普通／扩展／至父级末尾的 box 长度、运行时间窗、畸形头以及音频载荷中的相似字节。
- `scripts/quality/check-changed.sh --build-dir build/debug` 与 `language-surface.py --check` 通过；clang-tidy 保留 warning-only 提示，
  cppcheck 通过。未改变生产代码、公开 ABI 或锁定矩阵的音频算法。
