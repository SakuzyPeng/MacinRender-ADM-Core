# 七期 7.1：平台岛收缩的方向与基线

> 状态：已完成（2026-11-19，本机 Linux x64）。七期的切片与用户决定见
> [PCM 一致性审计的七期一节](PCM_CONSISTENCY_AUDIT.md#七期平台岛收缩)，六期移交的规模与开放问题见[六期结项](RUST_BOUNDARY_CLOSEOUT.md)。

七期按 [ADR 0021](../adr/0021-rust-primary-project.md) 收缩平台岛，退出条件是生产 C/C++ 只剩岛：
- 编码器的 C++ 封装迁入 Rust；
- 第三方 C 库与平台框架只经窄接口调用；
- Apple / Windows 岛评估 objc2 / windows crate。

7.1 不改生产代码，交付三样：逐岛清单、规模基线、编码器的新旧对照工具。

## 用户决定（2026-11-19）

1. **顺序**：编码器（7.2）→ 输出设备（7.3）→ Apple 岛（7.4）→ Windows 岛（7.5）。编码器在 Linux 上可以完整对照，所以排在最前。
2. **C 库绑定**：libFLAC、libopus 与 IAMF bridge 在 Rust 中手写 `extern` 声明，与 `mradm-ffi/src/flac.rs` 现有的 libFLAC 解码器声明相同。
   - 库仍由岛的 CMake 工程（`src/CMakeLists.txt`）构建，vendored 只有一份；
   - 不引入 `audiopus_sys`、`libflac-sys` 等会再构建一份 C 库的 crate。
3. **dr_flac 与 FLAC 读取链在 7.2 删除**，取代此前「dr_flac 暂缓」的决定。盘点发现，生产中没有读取 FLAC 输入的路径。下面这条链只被测试探针 `mr_adm_flac_reader_probe`（`flac_reader_test.py`）使用：
   - `FloatFlacReader`（dr_flac 解出 PCM）；
   - 它经函数表调用的 Rust `flac_validate`（`mradm-ffi/src/flac.rs` 的 libFLAC 严格校验）；
   - `mradm-wav` 的 `flac` 模块。

   7.2 把它们与 dr_flac 依赖一起删除。

   > 更正：征求这项决定时，我把这条链描述为「生产已改由 Rust 解码」。实际情况是生产完全不读 FLAC 输入，删除的结论不变。
4. **miniaudio 暂不替换**（同日早些时候的决定）。cpal 是以后的候选，见六期结项的七期初拟。

## 逐岛清单

行数取自 `language-surface.py` 的当前测量（main `2beee89` 之后）。

| 岛 | 生产 / 岛 / 接口（行） | 第三方与平台 API | Rust 调用的接口 | Rust 调用方 | C++ 测试 |
| --- | --- | --- | --- | --- | --- |
| 编码器与容器元数据 `src/adm_audio` | 生产 3,550，接口 285 | libFLAC（编码器与 metadata 接口），libopus（multistream），dr_flac，IAMF AOM bridge（`iamf_aom_bridge.h`），AudioToolbox / CoreFoundation（APAC，仅 macOS），外部进程 mp4box / ffmpeg 与 APAC worker | `mradm_audio_island`（事件 1 打包器探测，4 FLAC，5 Opus MKA，6 APAC，7 IAMF，8 MP4 打包，9 文件元数据），`mradm_audio_apac_child` | `mradm-capi` `render.rs` 的岛运行器，`cli.rs` 的 `__apac-encode` | `audio_island_test`、`flac_io_smoke_test`、`opus_mka_smoke_test`、`apac_smoke_test`、`iamf_smoke_test`、`wav_io_test`、`flac_reader_probe` |
| 输出设备 `src/adm_realtime` | 生产 405，接口 183 | miniaudio（硬件与 null 后端） | `mradm_device_islands`（`MradmDeviceTable` 记录） | `mradm-capi` `scene.rs`、`monitor.rs`，`mradm-ffi` 的设备探针 | 只有手动探针（`scene_stream_stress`、`system_spatial_monitor_check`、房间设备探针）；C 消费者测试枚举设备 |
| 平台事实 `src/adm_engine` | 接口 191 | 无，汇总其他岛 | `mradm_platform_islands` | `render.rs`、`cli.rs`、`monitor.rs` | C 消费者测试的能力表 |
| Apple 岛 `src/adm_apple` | 岛 2,312，接口 398 | AudioToolbox（AUSpatialMixer），AVFoundation / CoreMedia（AVSampleBufferAudioRenderer sink） | `mradm_apple_islands`，总线计划经 `apple_plan_ffi.h` 由 Rust 给出 | `render.rs`、`monitor.rs` | `apple_island_test`、`apple_renderer_smoke_test`（1,911 行）、`lfe_routing_test` 的 Apple 部分 |
| Windows 岛 `src/adm_windows` | 岛 709 | ISpatialAudioClient、MMDevice、WRL | 经 `mradm_device_islands` 的 sink 记录 | 同输出设备 | 只有手动探针 |
| Rust 服务函数表 | 生产 94（`rust_services_ffi.h` 72，`rust_services.cpp` 22） | — | `mradm_islands_bind`：Rust 交给岛的 33 + 3 个入口 | `mradm-capi` `services.rs` | 岛测试经 `rust_services_test.h` 绑定 |
| 进度回调跳板 `src/adm_c_api` | 生产 27 | — | `mradm_capi_progress_v1` / `_v2` | `render.rs` | C 消费者测试 |
| CUDA 边界 `src/adm_render_room/cuda` | CUDA 512 | CUDA runtime | Windows 上 Rust 在运行时加载的独立 DLL（`mradm-room-cuda`） | `mradm-ffi` `room.rs` | 无自动测试 |

**编码器的结构**：每个编码都是文件到文件的转换。
- Rust 先写出 float32 临时 WAVE，岛读取它并编码到最终容器；
- 元数据（事件 9）在编码完成后写入。

因此编码器迁入 Rust 后，PCM 的读取（`mradm-wav`）与容器写出都在 Rust，C 库只负责压缩：
- libFLAC：编码器与 Vorbis comment 块；
- libopus：multistream 编码。

各编码器的现有 C++ 部分：

| 编码器 | 行数 | 内容 |
| --- | ---: | --- |
| `opus_mka_io.cpp` | 835 | 手写的 EBML / Matroska 写出（每 5 秒一个 cluster，SimpleBlock）与 libopus 调用 |
| `flac_io.cpp` | 583 | libFLAC 编码（compression level 5、24 位）、经 libFLAC metadata 接口插入或替换 Vorbis comment，以及要删除的 dr_flac 读取 |
| `iamf_io.cpp` | 525 | AOM bridge 调用（整数 PCM staging），在 PATH 中探测 mp4box / ffmpeg 并经 `fork` + `execvp` / `CreateProcessW` 运行 |
| `metadata_io.cpp` | 318 | 注释文本格式化与 MP4 iTunes 元数据 atom 写出 |
| `apac_io.cpp` | 738 | AudioToolbox 编码，以及 worker 子进程与心跳协议；AudioToolbox 部分属于 Apple 岛 |
| `wav_io.cpp`、`audio_handles.cpp`、`wav_backend.h`、`wav_ffi.h`、`audio_io_internal.h` | 494 | Rust `mradm-wav` 的 C++ 薄包装：编码器读临时 WAVE，Apple 岛读输入、写输出 |

## 岛共用的头

`src/adm_core`、`src/adm_render_common`、`src/adm_metadata`、`src/adm_dsp` 共 2,693 行，是编码器与 Apple / Windows 岛共用的 C++ 头与私有 FFI 记录。按目前的使用者，它们的去向如下：

| 使用者 | 头 |
| --- | --- |
| 编码器与 Apple 岛 | `audio_io.h`（333）、`errors.h`、`logging.h`、`progress.h` |
| 只有 Apple 岛（含 Windows sink） | `render.h`、`render_apple.h`、`live_override.h`、`engine_records.h`、`routing_ffi.h`、`scene_handle.h`、`scene_ffi.h`、`scene_math.h`、`meter.h`、`direct_matrix_records.h`、`render_common.h`、`speaker_layouts.h`、`buffered_media_timeline.h` |
| 引擎与岛表 | `engine_ffi.h`（277）、`audio_output_device.h`、`capability.h`、`options.h` |
| 只被同目录或已无调用者 | `room.h`、`screen.h`、`config_ffi.h`、`layout_ffi.h`、`adm_ffi.h`、`gain.h`、`room_ffi.h` |

最后一类在 7.2 前后逐个核实，没有调用者的随相应切片删除。

7.2 之后，`audio_io.h` 只剩 Apple 岛使用的 WAVE 读写包装。7.4 的结果决定其余头留在 Apple 岛内，还是随 Apple 岛迁入 Rust 后删除。

## Apple / Windows 岛的评估标准（7.4、7.5）

每个岛先做一条路径的原型，再按下列标准决定迁入 Rust，或者保留 C++ / Objective-C++ 并只收窄接口：
- Apple 原型：APAC 编码或 AVSampleBufferAudioRenderer sink；
- Windows 原型：ISpatialAudioClient 的静态床。

标准：
1. **API 覆盖**：objc2-* / windows crate 是否覆盖所需接口。
   - Apple：AUSpatialMixer 属性与渲染回调、AudioConverter / ExtAudioFile 的 APAC 编码、AVSampleBufferAudioRenderer 与 CMSampleBuffer；
   - Windows：ISpatialAudioClient、ISpatialAudioObjectRenderStream、MMDevice、MMCSS。
2. **实时约束**：设备与渲染回调中不分配、不加锁，沿用现有的分配计数测试。
3. **unsafe 面**：迁入后 `unsafe` 行数与被删除的 C++ 行数之比，以及是否需要新的 `unsafe` 构造规则。
4. **构建**：不需要额外工具链（例如 bindgen / libclang）；新 crate 锁精确版本并登记许可证。
5. **行为**：Apple 冒烟测试与 `apple_island_test` 经岛表不改断言通过；系统空间监听由维护者实机确认。

## 对照方法

**编码器（7.2）**：用 6.1 的 CLI 记录做新旧对照。
- `cli-transcript.py`、`boundary.py` 与 `cli-commands.json` 已从 6.7 删除前的历史恢复，ctest 恢复 `mr_adm_boundary_tool_tests`，七期结项时再删除。C ABI 采集工具不恢复，七期不改 C ABI。
- 命令表新增 20 条 `encode-*` 命令：
  - FLAC：binaural、5.1、7.1、96 kHz、DirectSpeakers、裁剪、响度归一化、峰值限制、`-v`，以及高度层与 HOA3 的拒绝路径；
  - Opus MKA：2 / 6 / 8 / 12 / 16 / 24 声道（mapping family 0 / 1 / 255）、HOA3、6 与 320 kbps 每声道、裁剪、响度、`-v`，以及 96 kHz 的拒绝路径。
  - 命令表共 133 条命令。
- `mradm-testkit` 新增 12 秒的 `objects-long` fixture：Matroska 每 5 秒开一个 cluster，覆盖 cluster 翻转与较长的 FLAC。它不是 PCM 门禁输入，字节由 `fixtures_golden` 锁定。

**本机自我对照**：同一 Release（consistency-a）构建相隔两秒的两份记录，133 条命令 0 差异，MKA 的 track UID 由既有归一化处理。

APAC、MP4 元数据与 IAMF 在 Linux 上不可用：
- APAC 与 MP4 元数据：只能在 macOS 运行；
- IAMF：需要 AOM bridge SDK。

7.2 会为 MP4 atom 写出补 Rust 单元测试，并请维护者在 macOS 与带 IAMF 的构建上用同一命令表对照。

**输出设备与 Apple / Windows 岛**：
- 依靠岛测试、C 消费者测试与 Apple 冒烟测试；
- 维护者实机检查系统空间监听与设备切换；
- 一致性门禁的 PCM 不经这些岛。

## 规模基线

7.1 不改生产代码。

| 区域 | 行数 |
| --- | ---: |
| 生产 C/C++ | 8,684（其中 `c_api.h` 2,009 是契约） |
| Apple / Windows 岛 | 3,021 |
| 岛接口 | 1,057 |
| CUDA 边界 | 512 |
| C++ 测试 | 9,927 |

七期结束时的预期：
- 生产区域只剩 `c_api.h`，可能另有进度回调跳板（27 行，待 7.6 决定）；
- 编码器只剩 C 库本身，岛接口随编码器与设备迁入 Rust 而减少；
- Apple / Windows 岛的行数取决于 7.4 / 7.5 的评估结果。
