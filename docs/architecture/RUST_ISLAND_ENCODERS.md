# 七期 7.2：编码器迁入 Rust

> 状态：A、B 已完成（2026-11-19，本机 Linux x64），维护者已在 macOS / Windows 复核（2026-11-20）；C 进行中（C1 MP4 元数据、C2 MP4 打包器已完成，2026-11-21）；D（APAC 编排）待做。
> 切片与用户决定见[审计七期一节](PCM_CONSISTENCY_AUDIT.md#七期平台岛收缩)，清单与对照方法见 [7.1 记录](RUST_ISLAND_BASELINE.md)。

7.2 把编码器的 C++ 封装迁入 Rust：
- C 库（libFLAC、libopus、IAMF bridge）由 Rust 经手写 `extern` 直接调用（用户决定 2026-11-19）；
- 库仍由岛的 CMake 工程构建，vendored 只有一份。`mradm_native` 先构建链接清单中的编码库，再由 Cargo 链接；
- 每批与迁移前的构建做逐字节对照，用 CLI 记录的 `encode-*` 矩阵与其他命令。

| 批 | 内容 | 状态 |
| --- | --- | --- |
| A | FLAC 写出与 Vorbis comment；删除 dr_flac 与没有生产调用者的 FLAC 读取链 | 已完成 |
| B | Opus MKA：libopus multistream 编码与 Rust 写出的 Matroska 封装与 Tags | 已完成 |
| C | C1 MP4 元数据 atom；C2 MP4 打包器的探测与运行；C3 IAMF bridge 调用 | C1、C2 已完成 |
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
- **查找**：POSIX 按 `PATH` 各目录（跳过空项）用 `access(X_OK)` 依次找 `mp4box`、`MP4Box`，再找 `ffmpeg`，返回名字本身，运行时再经 `PATH` 解析，与 `execvp` 相同；Windows 照原实现调用 `SearchPathW`（`.exe` 扩展名）；
- **ffmpeg 版本**：运行 `ffmpeg -version`，退出码非 0 或没有输出时视为没有打包器；首行按 `sscanf("ffmpeg version %d.")` 的规则解析（格式中的空白匹配任意长度的空白、`%d` 跳过前导空白并接受正负号），主版本须大于 0；
- **运行**：经 `std::process` 直接运行，不走 shell；stdout 与 stderr 共用一个管道，最多保留 4096 字节，截断时只追加一次 `...(truncated)`；取消每 20 ms 检查一次。POSIX 上打包器在自己的进程组中运行，取消时向进程组发 SIGTERM，500 ms 后发 SIGKILL；Windows 上终止进程，`CREATE_NO_WINDOW`。被信号结束时退出码为 -1；无法启动时与原实现的 `execvp` 失败一样视为退出码 127、没有输出；
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
- Windows：原实现把原生窄路径当作 UTF-8 转宽字符，非 ASCII 路径会被破坏；现在路径经 UTF-8 正确传递。程序解析改用 Rust 标准库：在 `PATH` 与系统目录中查找，不再查找当前目录（`SearchPathW` 的探测不变）。
- 维护者复核：带 bridge 的构建在 macOS / Windows 上以 mp4box 与 ffmpeg ≥ 7 打包 IAMF MP4，含取消；Windows 的进程终止与输出捕获只在交叉 clippy 中编译过。

## 维护者实机复核

2026-11-20，macOS arm64 和 Windows x64 的 Release 构建均通过 15 项编码器及 C ABI FLAC 测试，也通过 CLI 冒烟。
macOS 干净构建由 `mradm_native` 构建两个静态库。Windows 另外使用同版本 libFLAC 的 DLL 导入库，通过 9 项 FLAC 测试。

用迁移前的 `c7544954` Release 构建对照，两个平台的 FLAC 与 Opus MKA 命令均无差异。
Windows 的 134 条命令无契约差异。macOS 全表中曾出现一条 APAC 的容器字节差异。
迁移前构建可产生两种 APAC 结果，在固定输入和输出路径下用两个版本复验，APAC 载荷逐字节一致。
