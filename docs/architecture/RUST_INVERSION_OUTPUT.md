# PCM 写出与后处理迁入 Rust（四期 4.8）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)四期 4.8。
> 本切片是结构迁移：锁定矩阵的 PCM、内核文件、诊断检查点、日志、错误码与错误消息都要求与迁移前逐字节相同。
> 维护者决定一并修正的四处缺陷与其他行为差别见[发布说明](#发布说明)，都不改变锁定矩阵的 PCM。

## 范围

最终 float PCM 的写出端、CAF 容器，以及渲染之后对输出文件的全部遍历（增益改写、trim、整数降位、WAV 布局收尾、AXML 替换）迁入 Rust，连同它们的临时文件与安装。没有生产调用者的 peak / loudness 公共 API 删除。WAVE 的样本字节、量化、头、布局改写与 bext / ambi 在此之前已经在 `mradm-wav`（见 [WAV 容器迁移](RUST_WAV_CONTAINER_MIGRATION.md)），本切片迁走的是外面的块循环、sidecar 命名、安装与 CAF。

| 批次 | 内容 | 原 C++ | Rust |
| --- | --- | --- | --- |
| A | 删除 `apply_peak_limit` / `apply_loudness_norm` 公共 API：两者没有生产调用者，`RenderService` 的后处理增益一直经 `apply_gain_to_file` 与 Rust `post_gain` | `include/adm/peak.h`、`loudness.h`，`src/adm_peak`、`src/adm_loudness`，`ADMPeak` / `ADMLoudness` 目标 | （删除） |
| B | CAF 容器：10 项布局标签表、写出（`caff` / `desc` / `chan` / `data`，data 大小先写 −1 再回填，小端 float32）、按块扫描的读取（C++ 不检查 `fseek` 结果、size −1 读到末尾、`csize−4` 按无符号回绕、后出现的块覆盖先出现的）与 `info` 追加；最终 float 写出端（RF64 WAVE 或 CAF）、进程级 writer 编号与 `writer/pass{N}-s{pos}.80-pcm.f32` 检查点 | `caf_io.cpp`，`FloatWavWriter` 的 C++ 检查点与 `next_writer_trace_id` | `mradm-wav::caf`，`mradm-render::output`，`mradm_wav::create_file_writer` |
| C | 分步遍历：增益改写（`|g−1| < 1e-6` 时跳过）、trim（范围钳制、先读掉起点之前的部分）、整数降位（排他创建的 `.bitdepth_tmp.NNNN`）、WAV 布局收尾（扬声器掩码表、7.1.4 置换、两种 profile、经 `mradm-adm` generate 的 ADM 元数据，2048 帧一步）与 AXML 替换（`.axml_tmp.NNNN`，目标为普通文件时才走备份替换）；sidecar 命名（随机的 `{stem}.{purpose}.{016x}{ext}` 与编号的 `.NNNN`，按 `std::filesystem` 的 stem / extension 语义）；安装（rename，失败时先删除再 rename；布局改写与降位先把原文件移到 `.layout_backup.NNNN`，失败时恢复）；CAF reader 取 `chan` 块的标签 | `audio_handles.cpp` 的 `apply_gain_to_file` / `trim_file_frames`，`wav_io.cpp` 的 `downconvert_to_int` / `finalize_wav_layout` / `replace_wav_axml` 与共用的 sidecar、`TempPathGuard`、`replace_wav_with_rewrite`，`wav_output.cpp` 的布局表 | `mradm-render::postprocess` |

C++ 侧只剩适配与编码器：
- `src/adm_audio/wav_backend.h`：Rust 句柄的 C++ 包装，以及 C 加入的分步循环壳 `run_output_pass`。壳在开始前、每一步之前、需要时在安装之前检查取消（文字 “render cancelled”，context `path=`），按 Rust 报告的 `done` / `total` / `message` 发进度；错误的 message 与 context 由 Rust 返回。
- `audio_io.h` 的接口不变：`FloatWavWriter`、`FloatCafWriter`（B 起多了 `finish()`）、`FloatCafReader`、`apply_gain_to_file`、`trim_file_frames`、`downconvert_to_int`、`finalize_wav_layout`、`replace_wav_axml` 都是薄壳。`WriterHandle`（WAV / CAF / FLAC 三选一）与 `ReaderHandle` 保留，后者只剩 `pcm_bits` 工具与 `core_smoke_test` 使用。
- `src/adm_engine/wav_output.cpp`（17 行）只把 `finalize_rendered_wav` 转发给 `audio::finalize_wav_output`。
- `RenderService` 的编排顺序（trim → 增益计划 → 增益改写 → 降位 → 编码 → 安装 → WAV 布局收尾 → 容器元数据）与进度区间不变，它归五期。
- 编码器整体留在 C++（维护者决定）：FLAC（libFLAC）、Opus MKA（libopus 与 EBML 封装、Tags）、IAMF（AOM bridge）、APAC（AudioToolbox 子进程），以及 MP4 的 udta 元数据与 `compact_metadata_comment`。审计 4.8 一行所说的“编码器沿用同一 sink 接口”，在 4.8 解释为编码器继续经 Rust reader 读取已写完的 float WAVE，不另建 C++ sink 抽象，因为那只会增加 C++。

`src/adm_audio` 由 4,909 行降到 4,184 行，其中 3,000 余行是编码器与 MP4 元数据。

### 保持不变的规则

- **消息与检查顺序。** 错误码、文字、context 与检查顺序逐字保留，包括 CAF 的英文错误、各遍历的短读 / 短写文字、`failed to replace output after {op}: {os}` 与备份恢复失败时给出备份路径的三条文字。
- **进度与取消。** 每种遍历的进度阶段、消息（如 “gain applied”、“trimming output”、“WAV channel layout finalized”）与 fraction 不变；取消检查的位置与原 C++ 循环相同，降位与布局收尾在安装前还检查一次，取消后原文件不变、不留临时文件。
- **sidecar 与安装。** 四种操作原来各自的命名与安装方式保留，没有合并成一套；测试检查的名字不变。
- **检查点与编号。** `writer/pass{N}-s{pos}.80-pcm.f32` 改由 Rust 经 `diagnostics::process` 写出，每个名字只归一方；进程级编号移到 Rust（`AtomicU64`），WAVE 与 CAF writer 共用，只在打开成功时取号，所以编号顺序不变。整数降位用的 writer 原来不取号，现在也不取号。
- **块大小。** 增益、trim、降位 4096 帧一块，布局收尾 2048 帧一步，与 C++ 相同。

### 维护者决定

2026-10-30：
- 4.8 分 A–D 四批，每批单独提交，维护者本机全量对照（普通与诊断）通过后再开始下一批。
- 删除 peak / loudness 公共 API 与它们的目标（ADR 0021 §4：`include/adm` 除 `c_api.h` 外不再是对外接口）。
- 迁移中一并修正四处缺陷，都记为行为变化：写出端的 `finish()` 与错误报告、改写保留源 CAF 的布局标签、`.caf` 拒绝 i16 / i24、`trim_file_smoke_test` 中永远通过的断言。
- 编码器整体留在 C++，含 FLAC、Opus MKA、IAMF、APAC 与 MP4 udta。
- C 中临时文件的安装也迁入 Rust。OS 错误描述取 Rust `io::Error` 去掉 “ (os error N)” 后的部分，在 Linux / macOS 上与 C++ `std::error_code::message()` 同为 strerror；Windows 上的措辞、编码与 rename 语义，以及 Windows context 中的路径改为 UTF-8，归为一类逐字节对照例外（见[验证](#验证)）。

## 发布说明

都不改变锁定矩阵的 PCM：
1. **删除的公共头与目标（A）。** `include/adm/peak.h`、`include/adm/loudness.h`（`apply_peak_limit`、`apply_loudness_norm`）、`src/adm_peak`、`src/adm_loudness`、`ADMPeak` / `ADMLoudness` 目标及其安装规则。只由它们写出的 `post/peak.*`、`post/loudness.*` 诊断检查点随之消失；锁定矩阵与比较器都不依赖它们，比较器中 `post/` 的排序规则保留，供对照旧数据使用。C ABI 没有变化。
2. **写出端报告收尾失败（B，修正 1）。** 扬声器、双耳与 Apple 后端写完后调用 `finish()`，失败时返回错误，不再在析构中丢掉（room 原本就如此）；`WriterHandle::finish` 对 CAF 也真正收尾，FLAC 仍在析构中收尾。
3. **CAF 的 IO 错误（B）。** CAF 写入与 `info` 追加不再忽略 IO 错误；写入改为全部写入或报错，不再返回部分帧数；偏移改为 64 位，Windows 上读取超过 2 GiB 的 CAF 不再受 `long` 限制；异常的 CAF 采样率（NaN、超出 u32）改为饱和转换，原来是未定义行为；CAF 路径与 WAVE 一样按 UTF-8 传给 Rust。
4. **改写保留 CAF 布局标签（C，修正 2）。** 增益改写与 trim 从源 CAF 的 `chan` 块读取布局标签并原样写回，源文件没有 `chan` 块时才用 `output_layout`；原来按 `output_layout` 重新打标签，Apple `0+2+0` 输出的 binaural 标签会被改成 `0+2+0`，与输出布局不符的还会报错。
5. **`.caf` 拒绝整数位深（C，修正 3）。** 终稿为 PCM `.caf`（不是 APAC-in-CAF）且 `--output-bit-depth` 为 `i16` / `i24` 时，在渲染前报 invalid_argument：“--output-bit-depth i16 is not supported for .caf output (CAF is float32 only)”（`i24` 同理）。原来静默写出 float32；默认位深就是 f32，不受影响。
6. **测试断言（C，修正 4）。** `trim_file_smoke_test` 检查 `.bitdepth_tmp.NNNN` 残留的断言原来匹配不到实际名字、永远通过，改为按实际命名匹配。
7. **增益与 trim 不再接受 FLAC 输入（C）。** 对 `.flac` 调用 `apply_gain_to_file` / `trim_file_frames` 返回 unsupported “{op} does not support FLAC input”，文件逐字节不变。生产路径不会改写 FLAC：FLAC 终稿先渲染到临时 WAVE，后处理之后才编码；原来只有 `flac_io_smoke_test` 走这条路。
8. **Windows 的安装与错误文字（C）。** 安装改用 Rust 的 `std::fs::rename` 语义，OS 错误文字取 Rust `io::Error`，context 中的路径为 UTF-8。Linux / macOS 上与原来逐字节相同。
9. **删除与改变的私有 FFI。** 删除 `mradm_wav_layout_*` 与 `MradmWavLayout`（C，显式选项改用 `WavLayoutOptions` 经 `mradm_output_finalize_begin` 传入）；`mradm_wav_replace_axml` 改为自己完成安装并返回 context；新增 `mradm_output_writer_*`、`mradm_caf_reader_*`、`mradm_caf_append_info`（B）与 `mradm_output_{gain,trim,bitdepth,finalize}_begin`、`mradm_output_post_{step,finish,destroy}`（C）。这些只在 `mradm-ffi` 与 C++ 之间使用，C ABI bundle 本来就不导出。
10. **其他。** `metadata::generate` 此后只有 libadm 参考测试（`tests/reference/libadm/reference_test.cpp`）调用，生产的 ADM 元数据由 Rust 直接调 `mradm_adm::generate`；按参考保留规则，它随该参考一起退役。
11. **维护者补充的修正（C 合并时）。**
    - `c04ca18`：`run_output_pass` 在第一次进度回调之前就接管 pass 句柄，回调抛出异常时不再泄漏句柄与临时文件，并补了异常清理的测试；Rust 中会创建 writer 的测试共用 `output::TEST_WRITER_LOCK`，避免并行测试交错取号；
    - `e2c171b`：被拒绝的 FLAC 改写逐字节核对文件不变；
    - `f929084`：sidecar 测试按原生路径比较。

## 黄金数据

| 语料 | 生成方 | 规模 | 结果 |
| --- | --- | --- | --- |
| `mradm-wav/tests/caf-v1/`（`cases.jsonl`、`expected.jsonl`、`samples/`） | main `4207627` 的 C++ `caf_io.cpp`，由 scratchpad 里的临时生成器（不提交）链接当时的 ADMAudio 一次性采集 | 376 例：写出（10 种布局 × 帧数 0 / 1 / 4095 / 4097 × 位模式与特殊值 NaN、±Inf、−0、次正规数 × 分块 × 采样率）、拒绝（未知布局、声道数不符）、`info` 追加（空、ASCII、UTF-8 的组合，非 CAF 与文件不存在）、畸形输入的读取（截断、坏 magic、缺 `desc` / `data`、非 lpcm、位深与标志、data size −1、未知块 size −1、重复块、`frame_count` 超出实际字节等）；记录错误码 / 文字 / context、读出的位 FNV 与每次读到的帧数，另有 23 份完整样例文件 | Rust 全部一致，样例文件逐字节相同 |

C 的遍历没有另冻结黄金数据：它们的输出字节由已有的 C++ 测试（`trim_file_smoke_test`、`core_smoke_test`、`wav_rust_test`、`wav_container_test` 对照冻结的 legacy、`render_trim_fixture_test`、`c_api_fixture_test` 的取消与残留文件、`normalization_acceptance_test`、`binaural_render_fixture_test`）与下面的新旧直接 API 对照覆盖。`mradm-render::postprocess` 的模块测试覆盖 sidecar 命名（含 dotfile 与结尾的 “.”）、任一步放弃后只剩原文件、安装失败的回退与恢复文字、掩码表 / 7.1.4 置换 / 两种 profile、CAF 标签保留与单位增益跳过、trim 钳制，以及 OS 文字去掉后缀。

每批都植入缺陷，确认测试会报错：

| 批次 | 植入的缺陷 | 结果 |
| --- | --- | --- |
| B | `chan` 字段顺序、data 大小回填、小端序列化、`csize−4` 不回绕、打开失败仍消耗编号 | 都被抓到 |
| C | 7.1.4 置换一项、增益的 1e-6 阈值、备份恢复的顺序、改写不保留 CAF 标签、sidecar 名的格式 | 都被抓到 |

## 验证

**逐批检查（Linux x64，提交前）。**
- ctest 全部通过（A 81/81；B、C 81/81 各连跑 3 次）；
- `mr_adm_rust_unit_tests`、fmt 与 clippy `-D warnings`；
- `mr_adm_ffi_header_check`（B 之后 356 个函数、141 个结构体；C 之后 359 个函数、142 个结构体）；
- 变更 C++ 的 clang-format 与 clang-tidy；
- `check-reference-retention.py`、`version_metadata.py --check`、`language-surface.py`、`check-licenses.sh`；
- 显式构建 `tests/manual` 下的目标；A 另打开 `MR_ADM_BUILD_EBUR128_REFERENCE_TESTS` 构建一次，48 组计量对照与后处理增益对照通过。

本机没有 cppcheck，由 CI 运行。

**新旧对照（scratchpad 工具，不提交）。** 基线是各批开始时的 `main`。

| 批次 | 基线 | 用例 | 结果 |
| --- | --- | --- | --- |
| A | `a89d4fb` | ctest 与 ebur128 参考对照（被删的 API 没有生产调用者，PCM 路径不变） | 通过 |
| B | `4207627` | CLI 460 例：`.wav` / `.caf` × 扬声器、HOA、双耳、EAR 布局 × 窗口 × 响度目标 / 增益 / 峰值选项 × 位深，比较退出码、`-v` 日志与输出字节；矩阵子集 79 个离线行与 28 个文件流式行，普通与诊断（含 Rust 写出的 `writer/pass*` 检查点） | CLI 0 差异；矩阵 0 个问题 |
| C | `e9a9541` | 同一 CLI 460 例，另存 stderr 核对新报错的文字；直接 API 373 个操作（scratchpad 探针在新旧两份构建中各编译一次，经 `audio_io.h` 调用：WAV / CAF × 增益 1、1+5e-7、0.5、NaN，trim 越界 / 整个文件 / 0 帧，位深 16 / 24 / 32 / 8 含 NaN 输入，布局收尾的全部布局 × 两种 profile 与未知布局，AXML 同路径 / 异路径 / 目标为目录，并逐一中途取消），比较码、文字、context（sidecar 名按规则规范化）、进度序列、残留文件与输出字节；矩阵子集普通与诊断 | CLI 51 例不同，全部是 `.caf` + i16 / i24 由成功改为报错（发布说明第 5 项）；直接 API 36 个操作不同，全部是 CAF 输入（修正 2：保留标签，原来报布局不符）或 FLAC 输入（第 7 项）；全部 WAVE 操作的结果、进度、取消与残留文件相同；矩阵 79 + 28 行 0 个问题 |

逐字节对照的例外按类列出：
- 离线文件比较时把 bext 的墙钟日期时间与 CAF `info` 的 date 置零；
- C 的 51 个 CLI 用例与 36 个直接 API 操作，即发布说明第 4、5、7 项的有意变化；
- Windows 上的安装语义、OS 错误文字与 context 路径编码（发布说明第 8 项），本机无法对照，由维护者在 Windows 上确认。

**维护者本机验证。**
- C：macOS arm64 Debug CTest 82/82（`f929084`）；Windows x64 规范 Release CTest 8/8，Rust render 单元测试 22/22；写出端与后处理的并发检查 8 线程 30/30 通过；变更 C/C++ 的质量门禁、Rust fmt / clippy、FFI 头与棘轮检查通过。
- A、B 由维护者本机全量对照后合入（`4207627`、`e9a9541`）。

**规模。** 生产 C/C++ 由 33,718 行降到 32,551 行（−1,167），平台岛行数不变：

| 批次 | 行数 |
| --- | --- |
| A | 33,718 → 33,378 |
| B | 33,378 → 33,016 |
| C | 33,016 → 32,551 |

**三平台。** 每次合入 `main` 后，Consistency 由 push 触发，在 macOS arm64、Linux x64、Windows x64 上采集 config A、B 的普通与诊断结果。

| 运行 | 提交 | 内容 | 结果 |
| --- | --- | --- | --- |
| [41037527666](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/41037527666) | `4207627` | A | 四份比较报告均为 283 个 PCM id 相同、0 个不同、平台集完整、门禁失败 0 |
| [41074793418](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/41074793418) | `e9a9541` | B | 同上 |
| [41152970680](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/41152970680) | `8278a4b` | C | 写本记录时仍在运行，结果待回填 |
