# 七期 7.2：编码器迁入 Rust

> 状态：A 已完成（2026-11-19，本机 Linux x64）；B（Opus MKA）、C（IAMF 与 MP4）、D（APAC 编排）待做。
> 切片与用户决定见[审计七期一节](PCM_CONSISTENCY_AUDIT.md#七期平台岛收缩)，清单与对照方法见 [7.1 记录](RUST_ISLAND_BASELINE.md)。

7.2 把编码器的 C++ 封装迁入 Rust：
- C 库（libFLAC、libopus、IAMF bridge）由 Rust 经手写 `extern` 直接调用（用户决定 2026-11-19）；
- 库仍由岛的 CMake 工程构建，vendored 只有一份；
- 每批与迁移前的构建做逐字节对照，用 CLI 记录的 `encode-*` 矩阵与其他命令。

| 批 | 内容 | 状态 |
| --- | --- | --- |
| A | FLAC 写出与 Vorbis comment；删除 dr_flac 与没有生产调用者的 FLAC 读取链 | 已完成 |
| B | Opus MKA：libopus multistream 编码与 Rust 写出的 Matroska 封装 | 待做 |
| C | IAMF bridge 调用、MP4 打包器的探测与运行、MP4 元数据 atom | 待做 |
| D | APAC：worker 子进程与心跳协议迁入 Rust，AudioToolbox 编码移入 Apple 岛；删除 `mradm_audio_island` | 待做 |

## A：FLAC

### 迁入 Rust 的部分

**`mradm-ffi` `flac`** 是新的 FLAC 写出模块，取代原来只供测试探针使用的解码校验。它有两个入口。

`encode`（原 `convert_to_flac`）：
- 用 `mradm-wav` 读最终的 float32 WAVE，每块 4096 帧；
- 每块：先 `clamp(±1)`，再乘 8,388,607，在 f32 上四舍五入（远离零），与 `lroundf` 相同；
- 交给 libFLAC 流编码器：24 位，compression level 5，write / seek / tell 回调；
- 析构时 `finish` 改写 STREAMINFO，再关闭文件，与 C++ 写出端的析构顺序相同；
- 取消在开始前和每块之前检查；
- 进度事件（post_processing / encode_flac）的帧数与文字与原岛相同。

`write_metadata`（原 `write_flac_metadata`）照原调用顺序使用 libFLAC 的 metadata chain：
- 读 chain，新建 Vorbis comment；
- 依次追加 ENCODER、DATE、COMMENT 与已知布局的 `WAVEFORMATEXTENSIBLE_CHANNEL_MASK`；
- 替换已有的 VC 块，没有时插在 STREAMINFO 之后；
- `sort_padding`，再经 `<path>.metadata_tmp` 用 tempfile 写回并改名；
- IO 回调用 Rust 文件句柄，`eof` 照 `feof` 的语义：短读置位，seek 清除；
- COMMENT 的文本与 CAF 共用 `mradm_engine::render::metadata_comment`（原私有的 `comment`，与 C++ `compact_metadata_comment` 相同）。

**派发**：`mradm-ffi` `engine_service.rs` 的 `Runner::run` 先由 `own_island` 接下 `Island::EncodeFlac` 与扩展名为 `.flac` 的 `Island::WriteMetadata`，其余岛仍交给 C++ `mradm_audio_island`。
- 进度与取消经同一个 `Bridge`，映射到渲染的进度区间，与 C++ 岛相同；
- C++ 岛对岛 4 返回 2（不接），岛 9 对 `.flac` 不再写入。

### 删除

| 删除的内容 | 说明 |
| --- | --- |
| C++ `flac_io.cpp`（583 行） | `FloatFlacWriter`、`FloatFlacReader`、`write_flac_metadata`、`convert_to_flac` |
| `audio_io.h` 中的对应声明，`WriterHandle` 的 FLAC 分支 | `.flac` 终稿时 Apple 岛写的是临时 `.wav`，这条分支已经走不到 |
| `island.cpp` 的岛 4 | — |
| dr_flac（dr_libs） | `MRDependencies.cmake` 的获取、`mr_adm_audio` 的链接、许可证清单、SBOM、`third_party/licenses/dr_libs` 与 `THIRD_PARTY_LICENSES.md` |
| FLAC 读取链 | 函数表的 `flac_validate`（Rust 与 `rust_services_ffi.h`，函数表 36 → 35 项）、`mradm-ffi` 的 libFLAC 解码校验、`mradm-wav` 的 `flac` 模块（Ogg / 原生 FLAC 输入）。生产没有调用者，只有测试探针在用（7.1 盘点） |
| 测试与工具 | `flac_io_smoke_test.cpp`、`flac_reader_probe.cpp` 与 `.manifest`、`flac_reader_test.py`、ctest `mr_adm_flac_io_smoke_tests` / `mr_adm_flac_reader_tests`，以及为替换 dr_flac 做的解码器选型工具 `scripts/flac-eval` |

`audio_island_test.cpp` 只保留 Opus 的预取消与"不接的岛交还调用方"两项检查，并新增一项：岛 4 交还调用方。

### 测试的去向

C++ 的 FLAC 检查移植为 Rust 测试。

`mradm-ffi` `flac`：
- **往返**：3 声道、10,000 帧，样本含越界值与恰好的舍入平局；libFLAC 解码器逐样本比对量化结果，MD5 一并校验；
- **进度**：事件序列（0、每块、`FLAC encoded`）；
- **Vorbis comment**：写两次后仍只有一个 VC 块，标签与立体声掩码正确，临时文件不留，音频不变；
- **取消**：开始前取消时不留输出、源文件保留；编码中取消在下一块停止；
- **失败**：源缺失、9 声道、元数据目标缺失或不是 FLAC 时的错误码与文字；
- **规则**：标签顺序与掩码、量化规则、扩展名判断。

`mradm-capi` `tests/render.rs` `flac_outputs_are_encoded_and_tagged`（原 C ABI 终稿流程检查）：
- 5.1 的 `.flac` 经 `adm_render_file` 写出；
- 临时文件已清除，旧固定临时路径上的文件保持不变；
- 输出为 6 声道，带 COMMENT 与掩码 `0x0000003F`。

### 对照与故障注入

**参考**：迁移前的 Release `mradm`（consistency-a，main `c754495`）。

**对照**：同一构建树上用 `cli-transcript.py` 录参考（`--tool mradm=<参考>`）与候选两份记录，134 条命令 0 差异。其中逐字节相同的输出包括：
- 9 个 FLAC 输出，DATE 由既有的时钟归一化处理；
- FLAC 的错误路径与 `-v` 日志。

**故障注入**：两处植入的缺陷都被对照抓到。
- 压缩级别改为 6：7 个 FLAC 输出不同；另两个输出在级别 6 下恰好与级别 5 字节相同。
- 标签顺序把 DATE 移到最后：9 个 FLAC 输出全部不同，第一个差异在 VC 块内。

**其他检查**：
- ctest 24/24；
- `mr_adm_rust_quality`（fmt 与 clippy）通过；
- `x86_64-pc-windows-msvc` 与 `aarch64-apple-darwin` 的交叉 clippy 通过；
- FFI 头校验通过：0 个函数，58 个结构体；
- 许可证检查与 `check-changed.sh` 通过。

### 规模

- 生产 C/C++ 8,684 → 8,024 行；
- 岛接口 1,057 → 1,054 行；
- C++ 测试 9,927 → 9,201 行。

### 发布说明

- FLAC 输出的字节、错误与进度不变。
- 删除的 FLAC 读取链不在任何公开路径上：CLI 与 C ABI 一直只接受 WAVE 输入。
- 构建不再获取 dr_libs。

### 待维护者复核

- macOS 与 Windows 上 Rust 直接链接 libFLAC 的符号，包括状态字符串表 `FLAC__StreamEncoderInitStatusString` / `FLAC__Metadata_ChainStatusString`。vendored 静态库在 Windows 上使用 `FLAC__NO_DLL`。
- 在两平台上用同一命令表录参考与候选，对照 `encode-flac-*`。
