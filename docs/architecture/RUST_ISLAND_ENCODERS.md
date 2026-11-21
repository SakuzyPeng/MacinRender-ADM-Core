# 七期 7.2：编码器迁入 Rust

> A、B、C 已合入。D 已在 macOS arm64 验证真实 APAC 编码，并在 Windows x64 验证不可用路径。D 的逐字节对照及 AudioToolbox 的重复性见本文末节。
> 切片与用户决定见[审计七期一节](PCM_CONSISTENCY_AUDIT.md#七期平台岛收缩)，清单与对照方法见 [7.1 记录](RUST_ISLAND_BASELINE.md)。

7.2 把编码器的 C++ 封装迁入 Rust：
- C 库（libFLAC、libopus、IAMF bridge）由 Rust 经手写 `extern` 直接调用（用户决定 2026-11-19）；
- 库仍由岛的 CMake 工程构建，vendored 只有一份。`mradm_native` 先构建链接清单中的编码库，再由 Cargo 链接；
- 每批与迁移前的构建做逐字节对照，用 CLI 记录的 `encode-*` 矩阵与其他命令。

| 批 | 内容 | 状态 |
| --- | --- | --- |
| A | FLAC 写出与 Vorbis comment；删除 dr_flac 与没有生产调用者的 FLAC 读取链 | 已完成 |
| B | Opus MKA：libopus multistream 编码与 Rust 写出的 Matroska 封装与 Tags | 已完成 |
| C | C1 MP4 元数据 atom；C2 MP4 打包器的探测与运行；C3 IAMF bridge 调用 | 已完成 |
| D | APAC：worker 子进程与心跳协议迁入 Rust，AudioToolbox 编码移入 Apple 岛；删除 `mradm_audio_island` | 已完成 |

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
- `sort_padding` 后用同目录的独占临时文件写回，成功后改名替换，失败时保留原文件；
- I/O 读回调先初始化 libFLAC 提供的缓冲区，再交给 Rust 读入。实际到达文件末尾时设置 EOF，seek 清除该标记，读取失败交给 libFLAC 报错；
- Rust 按 libFLAC 的枚举值格式化状态名，适用于 Windows 的静态库与 DLL 导入库；
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
- **失败**：源缺失、9 声道、非法采样率、元数据目标缺失和非法 FLAC 的错误码与文字。复制音频时注入读取错误，验证原文件字节和目录内容；
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

## B：Opus MKA

### 迁入 Rust 的部分

**`mradm-ffi` `opus_mka`** 取代 `opus_mka_io.cpp`（835 行），逐项照搬原岛的元素、字节布局、顺序与错误文字。

编码器：
- libopus 的 surround multistream 编码器，`OPUS_APPLICATION_AUDIO`；
- mapping family：HOA（`hoa*` 且声道数为平方数）用 2，1–2 声道用 0，标准 5.1 / 7.1（或无布局且不超过 8 声道）用 1，其余用 255；
- 5.1 / 7.1 按 Vorbis 顺序重排声道；
- VBR，不限制 VBR；码率为每声道 64 kbps，1–2 声道用 128 kbps，另可指定 6–320 kbps 每声道；
- lookahead 作为 preskip；
- `ctl` 是 C 变参函数，Rust 按变参声明调用，aarch64 Apple 上的 ABI 也正确。

Matroska 封装：
- EBML 头、未知长度的 Segment；
- Info 中先写占位的 Duration，关闭时回填；
- Tracks：A_OPUS、CodecPrivate 为 OpusHead、CodecDelay、SeekPreRoll、DefaultDuration；
- 每 20 ms 一帧写成 SimpleBlock，帧时刻距 cluster 起点超过 5 秒时开新 cluster，相对时间戳为 int16；
- 关闭时最后一个不完整的帧补零编码，再回填 Duration。

细节：
- size vint 的宽度照原实现的边界：恰为全 1 标记的值进位到下一宽度；
- track UID 是随机的 64 位数：原实现取自 `std::random_device`，现在取自标准库 `RandomState`，CLI 记录对它做归一化；
- 元数据（原 `write_mka_metadata`）：Targets 后依次为 ENCODER、DATE_RECORDED、RENDERER、OUTPUT_LAYOUT、INTEGRATED_LOUDNESS（1 位小数）与 TRUE_PEAK（2 位小数），组成 Tags 插在第一个未知长度的 Cluster 之前；小数格式取 `mradm_engine::fmt::fixed`，与原 `std::fixed` 相同。
- 写标签时分块查找并复制 Cluster 前后的数据，以限制内存用量。临时文件写完后替换原文件。

`Runner::run` 接下 `Island::EncodeOpus` 与扩展名为 `.mka` 的 `Island::WriteMetadata`。原岛对 Opus 不报进度，现在也不报。C++ 岛对岛 5 返回 2。

### 删除

- `opus_mka_io.cpp`；
- `audio_io.h` 的 `FloatOpusMkaWriter` 与 `convert_to_opus_mka`；
- `write_mka_metadata` 与 `.mka` 元数据分派；
- `island.cpp` 的岛 5；
- `mr_adm_audio` 对 Opus 的链接：libopus 仍列在交给 Cargo 的编解码库清单中；
- `opus_mka_smoke_test.cpp` 与 ctest `mr_adm_opus_mka_smoke_tests`。

剩下的 C++ 编码器（APAC、IAMF）在 Linux 上不可用，`audio_island_test.cpp` 因此只检查不接的岛（2、3、4、5、0、10）与 NULL 请求。

### 测试的去向

`mradm-ffi` `opus_mka` 的 Rust 测试：
- **OpusHead**：2 声道用 family 0；`wav71` 用 family 1，5 个流、3 个耦合流，mapping 为 0 6 1 2 3 4 5 7；`hoa3` 用 family 2，16 个流、0 个耦合流，mapping 为恒等；24 声道用 family 255；
- **Duration**：4,800 帧回填为 100 ms；1,000 帧补零成 2 帧，回填为 40 ms；
- **cluster**：12 秒的文件有 3 个 cluster（起于 0、5,020、10,040 ms）；
- **Tags**：验证第一个 Cluster 前后的字节、各标签及负零符号。Cluster 标记跨读取块时仍能找到，缺少标记时报错并保留原文件；
- **检查**：44.1 kHz 源、5 kbps 码率被拒；开始前取消时不留输出、源文件保留；取消在每块之前检查；
- **规则**：size vint 与 ID 的字节、扩展名判断。

### 对照与故障注入

**对照**：与迁移前的构建（同一参考 `mradm`）对照，删除 C++ 之前和之后各做一次，都是 134 条命令 0 差异。其中逐字节相同的 Opus MKA 输出共 12 个：
- 2 / 6 / 8 / 10 / 12 / 16 / 24 声道与 HOA3；
- 6 / 64 / 320 kbps 码率；
- 12 秒长输入与裁剪；
- 响度归一化后的 Tags；
- `-v` 日志。

96 kHz 的拒绝路径（不产生文件）的退出码与错误文字也相同。

**故障注入**：两处植入的缺陷都被抓到。
- cluster 间隔改为 4 秒：只有超过 4 秒的两个输出（`encode-mka-51-long`、`encode-mka-trim`）不同，符合预期；
- 标签 `DATE_RECORDED` 改名为 `DATE`：12 个 MKA 输出全部不同。

**其他检查**：
- ctest 23/23；
- Rust 质量检查、两个目标的交叉 clippy、FFI 头校验、许可证检查与 `check-changed.sh` 都通过。

### 规模

- 生产 C/C++ 8,024 → 7,141 行；
- 岛接口 1,054 → 1,051 行；
- C++ 测试 9,201 → 8,788 行。

### 发布说明

- Opus MKA 的字节、错误与日志不变（track UID 本来就是随机的）。
- Windows 上的改进：原 C++ 用窄字符 `fopen` / `fstream` 打开路径，当前代码页表示不了的字符会失败；现在经 UTF-8 路径打开宽字符路径，同时修正了 `.mka` 元数据的同一问题。

## C：IAMF 与 MP4

C 分三个子批：C1 MP4 元数据，C2 MP4 打包器（mp4box / ffmpeg）的探测与运行，C3 IAMF bridge 调用。

### C1：MP4 元数据

**迁入 Rust 的部分**：`mradm-ffi` `mp4_metadata`（原 `metadata_io.cpp` 的 `write_mp4_metadata`）。
- 在顶层 `moov` 末尾追加 `udta/meta(hdlr mdir appl)/ilst`，文本项依次为 ©too（ENCODER）、©day（DATE）、©cmt（COMMENT，与 FLAC / MKA 共用 `metadata_comment`），值为空的项不写，全部为空时不读也不写文件；
- `moov` 原地增长：它必须位于文件末尾，或紧跟一个足够大的 `free` atom 让出字节，所以 `stco` / `co64` 的偏移不变；
- atom 规则照原实现：size 1 为 64 位长度，size 0 延伸到文件末尾；取最后一个顶层 `moov`；`moov` 的头是 16 字节或增长后超过 32 位时报 unsupported；
- 错误码与文字照原实现（`cannot open MP4 for metadata read`、`malformed MP4 atom table`、`MP4 metadata: missing moov atom` 等），context 为 `path=<路径>`；
- 原实现把整个文件读进内存后截断重写；现在只读 atom 头，经同目录的独占临时文件分块复制，成功后改名替换（与 7.2 A / B 的元数据相同），失败时保留原文件。
- POSIX 上临时文件的模式至多为 `0600`，安装时恢复原文件模式。

`Runner::run` 接下扩展名为 `.m4a` / `.mp4` 的 `Island::WriteMetadata`。WAV（bext / ambi）与 CAF（info）的元数据五期起就由 `mradm-engine` 直接写出（`write_own_metadata`），岛 9 实际只剩 FLAC、MKA 与 MP4，所以 C++ 岛对岛 9 返回 2。

**删除**：
- `metadata_io.cpp`（`compact_metadata_comment`、MP4 atom 写出与 `write_file_metadata` 分派）；
- `wav_io.cpp` 的 `write_wav_metadata` / `write_caf_metadata` 与 `audio_io.h` 的 `MetadataFields`、`write_file_metadata`：生产没有调用者，只有 `core_smoke_test` 在用；
- 函数表的 `caf_append_info` 与 `wav_append_bext`（Rust 与 `rust_services_ffi.h`，35 → 33 项）、`mradm-ffi` 的两个入口与 `wav_ffi.h` 的 `MradmWavBext`；
- `island.cpp` 的岛 9。

**测试的去向**：
- `mradm-ffi` `mp4_metadata` 的 Rust 测试：`udta` 的逐字节布局（独立拼写的期望值）与空项；`moov` 在文件末尾；`free` 让出字节（文件长度不变）、恰好等于插入长度时缩到只剩头、不够时报错；无 `free`、缺 `moov`、atom 过短或截断、16 字节头的 `moov` / `free` 的错误码、文字、context，且原文件不变；64 位 `mdat`、多个 `moov`、size 0 的 `moov`；扩展名判断。这些测试加入 ctest `mr_adm_encoder_tests`；
- `core_smoke_test` 的 bext 字段、HOA3 ambi 与超过 4 GiB 的稀疏 RF64 上追加 bext 三项检查移到 `mradm-render` `tests/wav_container.rs`（`metadata_fields_land_in_bext_and_hoa3_gets_ambi`、`bext_follows_more_than_four_gib_of_rf64_data`；Windows 同样需要 `MR_ADM_RUN_LARGE_FILE_TESTS`）。CAF info 由 `mradm-wav` `tests/caf.rs` 覆盖，bext 的字节由 WAVE 容器 golden 覆盖；
- `apac_smoke_test`（macOS）仍检查真实 AudioToolbox `.m4a` 写入元数据后能解码，改经测试函数表新增的 `mp4_write_metadata` 调用 Rust（`tests/support/ffi/island_test_ffi.h`）。

**对照与故障注入**：
- Linux 的 CLI 不产生 MP4（APAC 只在 macOS，IAMF 需要 bridge），所以另做了 atom 级对照：随机生成 3,000 个顶层 atom 序列（`ftyp` / `mdat` / `moov` / `free` / `skip`，含 64 位长度、size 0、截断、尾部垃圾与损坏的长度字段，`free` 的长度覆盖插入长度的边界），分别交给仍在的 C++ `write_file_metadata` 与 Rust，结果码、文字、context 与输出字节全部相同（663 个成功，其中 108 个经 `free` 原地增长）；另一个种子的 600 个用例也相同；
- 故障注入：`free` 长度判断由 `>=` 改为 `>`，20 个用例的结果与字节不同；改为取第一个 `moov`，340 个结果、264 个文件不同；
- 删除 C++ 之后，CLI 记录与迁移前的构建对照，134 条命令 0 差异；
- 新的 bext 字段测试经故障注入（originator reference 改写为 encoder）确认会失败；测试函数表字段的签名改错时 FFI 头校验报错。

**其他检查**：ctest 25/25；Rust 质量检查、两个目标的交叉 clippy、FFI 头校验（57 个结构体）与 `check-changed.sh` 通过。`rust_services_ffi.h` 补登记了 cppcheck 的 `unusedStructMember` 抑制：函数表的字段由 Rust 填写，6.6 起漏登。

**规模**：生产 C/C++ 7,141 → 6,720 行；岛接口 1,051 → 1,036 行；C++ 测试 8,788 → 8,379 行。

**发布说明**：
- MP4 元数据的字节、错误码与文字不变；写入改为经临时文件替换，失败时原文件不再可能被截断；
- 维护者在 macOS 复核：`apac_smoke_test`，以及 CLI 记录中 `encode-apac-*` 的元数据与迁移前一致。

### C2：MP4 打包器

**迁入 Rust 的部分**：`mradm-ffi` `mp4_packager`（原 `iamf_io.cpp` 的进程辅助、`detect_iamf_mp4_packager` 与 `package_iamf_to_mp4`）。
- **查找**。POSIX 按 `PATH` 各目录用 `access(X_OK)` 依次找 `mp4box`、`MP4Box`、`ffmpeg`，跳过空项。运行时按名字再次查找。Windows 直接启动 `SearchPathW` 找到的 `.exe` 完整路径。
- **ffmpeg 版本**：运行 `ffmpeg -version`，退出码非 0 或没有输出时视为没有打包器；首行按 `sscanf("ffmpeg version %d.")` 的规则解析（格式中的空白匹配任意长度的空白、`%d` 跳过前导空白并接受正负号），主版本须大于 0；
- **运行**。通过 `std::process` 直接启动。stdout 和 stderr 共用管道，最多保留 4096 字节，截断时追加一次 `...(truncated)`。主进程退出后读完已就绪的输出并关闭读取端。每 20 ms 检查一次取消。POSIX 向进程组发 SIGTERM，等待 500 ms 后仍在运行则发 SIGKILL。Windows 以 `CREATE_NO_WINDOW` 启动，取消时终止进程。信号退出返回 -1，启动失败返回 127。
- **打包**：mp4box 为 `-add <iamf> -new <mp4>`，ffmpeg 为 `-y -i <iamf> -c copy <mp4>`；失败消息为 `mp4box packaging failed: ` / `ffmpeg packaging failed: ` 加去掉末尾换行的输出，context 为空；找不到打包器时为原来的 unsupported 消息；取消为 `render cancelled`，context 为 `path=<mp4>`。

`Runner::run` 接下 `Island::ProbeMp4Packager` 与 `Island::PackageMp4`。输出格式文档的打包器可用性（只在 IAMF 构建中探测）也改由 Rust 探测，`MradmPlatformIslands` 删除 `iamf_mp4_packager` 字段，`platform_island.cpp` 删除对应函数。C++ 岛只剩 APAC（岛 6）与 IAMF（岛 7）。

**删除**：`iamf_io.cpp` 的 POSIX / Windows 进程辅助（`run_process`、`find_in_path`、输出截断）与打包器的探测和运行；`audio_io.h` 的 `IamfMp4PackagerKind`、`IamfMp4PackagerInfo`、`detect_iamf_mp4_packager`、`iamf_mp4_packager_available` 与 `package_iamf_to_mp4`；`island.cpp` 的岛 1 与岛 8；平台表的 `iamf_mp4_packager`（C 头与 Rust）。

**测试**：`mradm-ffi` `mp4_packager` 的 Rust 测试（Unix）在临时目录中放假的打包器脚本，`PATH` 只含这些目录与脚本用到的几个工具的链接，不受机器上安装的 ffmpeg / mp4box 影响：
- 查找顺序（`mp4box` 在每个目录都先于 `MP4Box`）、空项与不可执行文件；
- ffmpeg 版本的 `sscanf` 规则（13 种首行）、非 0 退出、无输出、版本只在 stderr；
- 输出合并、截断（10,000 字节）、退出码、找不到程序（127）、被信号结束（-1）、开始前取消；
- 取消时进程组先收到 SIGTERM、被忽略后 500 ms 收到 SIGKILL，留下的孙进程随进程组结束；
- 两种打包器的参数（含空格的路径）、失败消息（`\r` 保留、末尾 `\n` 去掉）、unsupported 与两种取消。

这些测试加入 ctest `mr_adm_encoder_tests`。`audio_island_test` 改为检查 C++ 岛不接岛 1 与岛 8。

**对照与故障注入**：
- 本机 CLI 不产生 IAMF，所以另做了对照：17 个假打包器场景（各种查找结果、版本行、失败输出、截断、信号、`\r\n`、空格路径）分别交给仍在的 C++ `detect_iamf_mp4_packager` / `package_iamf_to_mp4` 与 Rust，探测结果、错误码、消息与 context 全部相同；
- ffmpeg 版本解析与 C `sscanf` 在 5,002 行随机首行上对照：824 行解析出版本，结果相同；只有 155 行的数字超出 `int`（C 的 `sscanf` 在此为未定义行为，glibc 截断成任意值），Rust 视为没有版本；
- 故障注入：失败消息改为同时去掉 `\r`，1 个场景不同；ffmpeg 参数 `copy` 改为 `-copy`，2 个场景不同；
- 删除 C++ 之后，CLI 记录与迁移前的构建对照，134 条命令 0 差异。

**其他检查**：ctest 25/25；Rust 质量检查、两个目标的交叉 clippy（含 Windows 的 `SearchPathW` 与 `CommandExt`）、FFI 头校验与 `check-changed.sh` 通过。

**规模**：生产 C/C++ 6,720 → 6,311 行；岛接口 1,036 → 1,014 行。

**发布说明**：
- 打包器的选择、参数、消息与取消行为不变。
- ffmpeg 主版本超出 32 位整数时视为没有可用的 ffmpeg（原实现结果未定义）。
- Windows 的输入输出参数保持 Unicode，封装器的探测和启动使用同一条完整路径。
- 维护者复核：带 bridge 的构建在 macOS / Windows 上以 mp4box 与 ffmpeg ≥ 7 打包 IAMF MP4，含取消；Windows 的进程终止与输出捕获只在交叉 clippy 中编译过。

### C3：IAMF bridge

**迁入 Rust 的部分**：`mradm-ffi` `iamf`（原 `iamf_io.cpp` 的 `convert_to_iamf` 与 `iamf_encoding_available`）。
- `mr_iamf_aom_bridge_abi_version` 与 `mr_iamf_aom_encode_wav_to_iamf` 经手写 `extern` 调用，`MrIamfAomEncodeOptions` 为 `#[repr(C)]` 的 `IamfEncodeOptions`，接口以 bridge 自己的头 `tools/iamf_aom_bridge/mr_iamf_aom_bridge.h` 为准（`src/adm_audio/iamf_aom_bridge.h` 是它的副本，随 C++ 删除）；
- 检查顺序照原实现：取消（`render cancelled`，`path=<iamf>`）、9.1.6 / atmos916（unsupported）、读源文件（错误码与文字来自 WAVE 读取，`path=<wav>`）、48 kHz（invalid_argument）、bridge ABI 为 0 或大于 3（io_error）、ABI 低于 3 时不接受 scalable layers（unsupported）；
- 选项照原实现填写：`abi_version` 取 bridge 的版本，profile 为 AUTO，响度与峰值带“有值”标记；ABI ≥ 2 时填 `struct_size` 与取消回调，ABI ≥ 3 且有层时填逗号分隔的层；
- 取消回调可能在 bridge 自己的线程上调用：回调读取岛上下文的取消检查（原 C++ 的 `Cancel` 同样从监视线程读取），panic 视为未取消；
- bridge 返回 `CANCELLED` 或调用方已取消时为 `render cancelled`；其他非 0 结果为 `AOM IAMF bridge encode failed: ` 加 bridge 的错误文字（为空时为 `unknown bridge error`），context 为 `path=<iamf>`；
- POSIX 路径传 UTF-8。Windows 与 C API 共用 MSVC 文件系统代码页选择规则。线程的 C locale 为 UTF-8 时优先使用 UTF-8，否则按 Win32 文件 API 的 ANSI 或 OEM 设置转换。代码页表示不了的路径报 `render island failed`。

**条件编译**：bridge 只在 `MR_ADM_ENABLE_IAMF=ON` 的构建中链接。岛的 CMake 工程此时在链接清单里写 `feature:iamf`，`mradm-islands-sys` 把它转成 `cargo::metadata=feature_iamf=1`，`mradm-ffi` 新增的 `build.rs` 据 `DEP_MRADM_ISLANDS_FEATURE_IAMF` 设置 `cfg(mradm_iamf)`。没有 bridge 时 `.iamf` 输出照旧报 unsupported（`IAMF encoding requires MR_ADM_ENABLE_IAMF=ON and the official AOM iamf-tools bridge`），不回退到任何自写的 OBU writer。

`Runner::run` 接下 `Island::EncodeIamf`；平台事实的 IAMF 可用性改为 Rust 的编译期常量，`MradmPlatformIslands` 删除 `iamf` 字段。C++ 音频岛只剩 APAC（岛 6）。

**删除**：`iamf_io.cpp`、`src/adm_audio/iamf_aom_bridge.h`、`audio_io.h` 的 `convert_to_iamf` 与 `iamf_encoding_available`、`island.cpp` 的岛 7、`mr_adm_audio` 对 bridge 的链接与 `MR_ADM_ENABLE_IAMF` 定义、平台表的 `iamf`（C 头与 Rust）、`platform_island.cpp` 的对应函数；`iamf_smoke_test.cpp` 与 ctest `mr_adm_iamf_smoke_tests`。

**测试**：`mradm-ffi` `iamf` 的 Rust 测试：
- 没有 bridge 的构建：unsupported 的码、文字与空 context（即使已取消）；
- 有 bridge 的构建（原 `iamf_smoke_test` 的检查）：开始前取消、9.1.6 / atmos916、源文件缺失、44.1 kHz 源；
- 取消回调读取闭包、panic 与空指针时返回 0；
- `IamfEncodeOptions` 在 64 位目标上的字段偏移与大小（104 字节）。

`IamfEncodeOptions` 另由 FFI 头校验与 bridge 的头逐字段配对（`ISLAND_RECORDS`，头列入 `FFI_HEADERS`）。这些测试加入 ctest `mr_adm_encoder_tests`。

**对照与故障注入**：本机没有 AOM SDK，所以写了一个与 bridge ABI 相同的桩库：记录收到的选项（各字段、回调与层），按环境变量返回成功、带文字或空文字的失败、非常规返回码、`CANCELLED`，或在自己的线程上轮询取消回调。
- 用仍在的 C++ `convert_to_iamf` 与 Rust 分别链接桩库，跑 22 个用例：ABI 1 / 2 / 3 / 0 / 4，有无层与响度，各种失败与取消（含 30 ms 后由 bridge 线程轮询到的取消），9.1.6 / atmos916，源缺失、损坏、44.1 / 96 kHz，含空格与非 ASCII 的路径。结果码、文字、context 与桩库记录的选项全部相同；
- 有 bridge 的 Rust 单元测试在桩库下通过；
- 故障注入：ABI 1 时也填 `struct_size`，2 条选项记录不同；去掉对 `CANCELLED` 返回码的判断，1 个结果不同；
- 删除 C++ 之后，CLI 记录与迁移前的构建对照，134 条命令 0 差异（没有 bridge 的构建在请求判定时就拒绝 `.iamf`，读的是 Rust 的 IAMF 可用性）；
- 有无 `cfg(mradm_iamf)` 两种情形都在本机、Windows 与 macOS 目标上跑过 clippy；FFI 头校验对 bridge 选项结构的字段改动报错（故障注入）。

**其他检查**：ctest 24/24（`mr_adm_iamf_smoke_tests` 删除）；Rust 质量检查、FFI 头校验（58 个结构体）与 `check-changed.sh` 通过。

**规模**：生产 C/C++ 6,311 → 6,100 行；岛接口 1,014 → 988 行；C++ 测试 8,380 → 8,324 行。

**发布说明**：
- IAMF 的选项、结果与消息不变。
- 需维护者用真实 bridge 复核：macOS / Windows 上带 `MR_ADM_ENABLE_IAMF=ON` 的构建（含 `cmake` crate 直接构建的路径）链接 bridge，`encode-iamf-*` 与 IAMF MP4 的 CLI 记录与迁移前一致，取消生效；Windows 的代码页转换与 DLL 导入库只在交叉 clippy 中编译过。

## A、B 的维护者实机复核

2026-11-20，macOS arm64 和 Windows x64 的 Release 构建均通过 15 项编码器及 C ABI FLAC 测试，也通过 CLI 冒烟。
macOS 干净构建由 `mradm_native` 构建两个静态库。Windows 另外使用同版本 libFLAC 的 DLL 导入库，通过 9 项 FLAC 测试。

用迁移前的 `c7544954` Release 构建对照，两个平台的 FLAC 与 Opus MKA 命令均无差异。
Windows 的 134 条命令无契约差异。macOS 全表中曾出现一条 APAC 的容器字节差异。
迁移前构建可产生两种 APAC 结果，在固定输入和输出路径下用两个版本复验，APAC 载荷逐字节一致。

## D：APAC worker 与 AudioToolbox

离线渲染的 `EncodeApac` 由 `mradm-ffi::apac` 处理。父进程优先使用可执行的 `MRADM_APAC_HELPER`，其次查找当前映像旁的 `mradm`，最后检查宿主可执行文件是否就是 `mradm`。相对路径的覆盖项相对于当前目录解析。用独立进程加载 C ABI 动态库，已验证缺少 helper、忽略 PATH 中的同名程序、使用库旁的 helper 和显式相对路径四种行为。

父进程每 50 ms 检查取消和心跳。启动、块写入、收尾的默认超时分别为 30、10、15 秒，沿用 `MRADM_APAC_STARTUP_MS`、`MRADM_APAC_WRITE_STALL_MS` 和 `MRADM_APAC_FLUSH_BUDGET_MS`。stdout 与 stderr 共用非阻塞管道。取消或超时会终止整个进程组、回收 worker 并清理部分输出。Rust 测试覆盖拆行、混合两路输出、子进程错误、三个阶段分别超时，以及取消时回收后代进程。CLI 在编码中收到 SIGINT 时实测退出 130，保留已有目标文件并清除本次暂存文件。

`mradm __apac-encode` 用 `mradm-wav` 每次读取最多 4,096 帧。worker 保留布局校验、空间布局默认码率和 `wav71` 的侧后声道交换。每次写完一块发送 `P <done> <total>`，收尾前发送 `F`，成功后再发送终态进度，失败时发送 `E <code> <message>`。Apple 岛通过 `MradmApacEncoder` 提供打开、写帧和关闭三个 AudioToolbox 操作。Windows 的 worker 继续返回退出码 1 和原来的 `E 2` 诊断。

通用 C++ 入口 `mradm_audio_island`、`mradm_audio_apac_child` 及其私有事件记录已删除。C++ 音频模块只保留 Apple 渲染和测试使用的 WAVE/CAF 包装。生产 C/C++ 从 6,100 降至 5,248 行。岛接口从 988 增至 1,009 行，含新登记的 AudioToolbox 接口及实现。公开 C ABI 仍导出 164 个函数。

### Release 验收

参考实现为 `9c238e10`。每组均用同一构建树、命令表和 fixture 工具采集参考 CLI 和候选 CLI，使用仓库原有的逐字节比较器。

| 平台与配置 | CLI 对照 | 额外核对 |
| --- | --- | --- |
| macOS arm64，IAMF 关闭 | 134 条，0 差异 | 真实 APAC M4A 和 CAF 解码、7.1 声道顺序、HOA3、元数据与强制收尾停滞 |
| macOS arm64，启用 AOM bridge 与 MP4Box | 134 条，0 差异 | IAMF MP4 可读出 Audio Element 和 Mix Presentation 两组，保留 GPAC 文件缺少相邻 free atom 时的元数据告警 |
| Windows x64，MSVC Release，IAMF/SOFA 关闭 | 134 条，0 差异 | APAC 不可用诊断、编码器、C ABI 和 CLI 测试 |

macOS 默认 Release 配置还通过了 `mradm-capi` 的 122 项和 `mradm-ffi` 的 61 项库测试。编码器 CTest 包含 APAC 派发与平台能力测试。

**APAC 的重复性。** 额外采集中，7.1.4 的 `render-apac` M4A 出现过 154,477 和 154,478 字节两种文件。首个归一化差异在偏移 830 的包长字段，音频载荷也相差 1 字节。截取送入两个 worker 的 48,000 帧、12 声道 float32 PCM，2,304,000 字节完全相同。使用同一可执行文件位置、输入和输出，分别改变进程环境的长度，两版均产生同样两种 `mdat` 摘要。心跳序列一致，其余 133 条命令、CAF 和 MP4 标签均通过对照。原始差异保留在记录中，比较器照常逐字节核对 APAC 载荷。

### 耗时与峰值内存

在接电的 macOS arm64 M4 Pro 上，用 12 秒 `objects-long` fixture 做 7.1.4 APAC 离线渲染。两个 Release 版本各预热一次，每轮交换执行顺序。表内为中位数，RSS 取自 macOS `wait4`。

| 测量 | 每版轮数 | 参考耗时 | 候选耗时 | 参考峰值 RSS | 候选峰值 RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| 完整离线渲染，首次 | 6 | 877.3 ms | 890.6 ms | 75.4 MiB | 90.2 MiB |
| 完整离线渲染，同一可执行文件位置复测 | 12 | 893.3 ms | 882.0 ms | 81.1 MiB | 81.1 MiB |
| 直接编码同一份暂存 WAVE，复测 | 18 | 562.3 ms | 552.9 ms | 75.5 MiB | 75.6 MiB |

首次完整渲染的候选 RSS 中位数增加 19.6%，触发复测。12 轮中两版均出现约 75–101 MiB 的样本。将 helper 换成启动即返回诊断的程序时，两版峰值中位数分别为 30.6 和 30.5 MiB。直接运行真实 worker 的 6 轮中也出现过 11.1% 的中位数差异，再测 18 轮为 0.2%。[逐轮数据、构建与载荷摘要](evidence/rust-apac-worker/validation.json)
