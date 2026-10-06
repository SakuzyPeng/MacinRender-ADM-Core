# Rust 浮点 WAVE 迁移（移除 dr_wav）

本轮将 dr_wav 的生产用途改由项目自有的 `mradm-wav` 承担：

- `FloatWavReader`：把 WAVE 解码为 float32，供 ADM/channel-bed 导入、probe、后处理和各编码器读取中间文件；
- `FloatWavWriter`：写出 float32 RF64，供渲染输出、峰值限制、响度归一化、trim/gain 改写使用。

至此，生产路径上的 WAVE 样本读写全部经过 Rust。以下部分保持原有职责，不在本轮范围内：C++ 的容器元数据读取与改写（`finalize_wav_layout`、`write_wav_metadata`、chunk 扫描）、ADM XML 语义和渲染算法。dr_libs 仍随 dr_flac 获取，用于 FLAC 解码。

## 边界与数值契约

- 公开 C++ API（`include/adm/audio_io.h`）的类名和方法签名不变，只新增 `FloatWavWriter::finish()` 和 `WriterHandle::finish()`。
- 失败约定与 dr_wav 一致：`FloatWavReader::read` 失败时返回 0，`seek` 返回 false，`FloatWavWriter::write` 返回 0。
- `RenderInputReader` 不再区分浮点 ADM 与整数 ADM，所有输入统一使用 Rust reader，错误经 `Result` 返回。后续清理删除了 `RenderInputReader::open` 已不起作用的 `channel_bed` 参数（内部 C++ API，不影响 C ABI）。

解码结果与 dr_wav 逐位一致：

- PCM16 除以 32768；
- PCM24 左移进 32 位后除以 2^31，结果等同于除以 2^23；
- PCM32 除以 2^31；
- float32 原样拷贝。

float32 输出仍固定为 RF64，因为流式 writer 写入时无法预知最终大小，统一用 `ds64` 承载长度。这与 dr_wav 时期一致，`finalize_wav_layout` 也会保留 64 位容器。兼容承诺只针对样本字节和格式字段：Rust writer 会多写一个 `fact` chunk，chunk 布局与 dr_wav 不同，不要求逐字节相同。

writer 的创建方式：

- `FloatWavWriter` 以截断方式创建文件，与 dr_wav 的 `fopen("wb")` 覆盖语义一致；
- 整数位深转换仍然排他创建临时文件，只有在 finish 成功后才安装。

writer 收尾的规则：

- 析构时会尽力 finish，保持原有的 RAII 用法；
- 先写临时文件、再改名替换原文件的路径会显式调用 `finish()` 并检查结果，避免安装头部不完整的文件。这些路径包括 `apply_gain_to_file`、`trim_file_frames`、峰值限制和响度归一化。
- `finish()` 缓存首次收尾结果；失败后的再次调用仍返回原错误，不会把未完成的文件报告为成功。

## 行为差异

dr_wav 顺带支持、但项目从未承诺的输入格式现在明确返回 `unsupported`：

- 8-bit PCM；
- float64；
- A-law、µ-law、ADPCM；
- W64；
- AIFF。

channel-bed 导入和 `finalize_wav_layout` 原本就只接受 PCM16/24/32 与 float32，因此这一改动只影响"读取原始 WAVE 事实"的 probe 回退路径。

头部声明长度超出实际文件大小的损坏 WAVE 会直接报错，不再静默读到 EOF 为止。整数 ADM 自 libbw64 迁移起就采用这一行为。

同时放宽了一项 Rust reader 的检查：ds64 的 `sampleCount` 不再必须等于 data 推算出的帧数。该字段对应可选的 fact chunk，各家写入工具对它的单位（帧数还是样本数）理解不一，dr_wav 也会忽略它。帧数一律按 data 长度计算。

## C++ 与 FFI

`mradm_wav_reader_open` 返回的 `MradmWavInfo` 增加三个字段：

- `channel_mask`：没有 extensible 头时为 0；
- `bits`：容器位深；
- `format`：1 表示 PCM，3 表示 IEEE float。

`mradm_wav_writer_create` 增加两个参数：

- `float_output`：写 float32 RF64，此时位深必须为 32；
- `exclusive`：拒绝替换已存在的文件。

C++ 包装 `RustWavWriter` 统一承担整数和浮点输出，`IntegerWavWriter::create` 保留为它的排他整数写入入口。FFI 头的校验由 `mr_adm_ffi_header_check` 覆盖。

## 测试与依赖

默认构建不再定义 `dr_wav::dr_wav`。dr_libs 只为 dr_flac 获取；dr_wav 只在 `MR_ADM_BUILD_DRWAV_REFERENCE_TESTS=ON` 时作为参考测试依赖暴露。

`mr_adm_drwav_reference_tests` 对比新旧实现，fixture 在运行时生成：

- 读取：PCM16/24/32、float32 × RIFF/RF64/BW64 × 普通头与 extensible 头，共 24 组。用奇数块大小读取，再 seek 到第 123 帧回读，断言样本和格式字段与 dr_wav 逐位一致。dr_wav 不认识 BW64，对照侧按旧实现的做法把头部标签改写为 RF64 后再读。
- 写入：Rust 写出的 float32 RF64 由 dr_wav 读回；dr_wav 按旧 `FloatWavWriter` 配置写出的 RF64 由 Rust 读回。两个方向都要求样本逐位一致。

默认测试的覆盖：

- `mr_adm_wav_rust_tests` 新增：float writer 覆盖已存在的文件、finish 幂等、finish 后写入失败、输出为 RF64、BW64 标签的 float 输入可读，以及 8-bit、A-law、float64 返回 `unsupported`。
- Rust 单元测试新增：FFI 的 float RF64 往返、截断模式、`Info` 新字段，以及 ds64 `sampleCount` 不一致时仍然接受文件。

审查修正：参考测试末块按剩余帧数限制请求量，保证传入 Rust FFI 的切片完整位于分配的缓冲区内；
`mr_adm_wav_rust_tests` 在 POSIX 平台用文件大小限制注入收尾 I/O 失败，验证恢复 I/O 后再次 finish
仍保留原始错误、后续写入被拒绝，且未完成的文件不能作为有效输出读取。

## 验收记录

2026-10-06，Linux x86_64（GCC 13）：

- 默认 Debug CTest 67/67 通过。其中 `mr_adm_scene_export_tests` 的 BW64 fixture 写入的 ds64 `sampleCount` 与实际帧数不一致，正是这个用例暴露了上述 Rust 检查过严的问题。
- Release 下 `mr_adm_drwav_reference_tests` 通过（24 组解码，加双向写读往返）。故意把 PCM16 换算系数改为 /32767 后，测试能够报出差异。
- Release 端到端比较：分别用 `origin/main`（`5dd0378`，dr_wav）和本分支渲染同一批输入，共 81 组，全部一致。
  - 输入共 12 个：`mr_adm_make_fixture` 生成的 4 种 24-bit ADM（objects-point、objects-extent-multi、directspeakers、hoa），把它们转成 float32 RF64 的版本，一份 float32 BW64 ADM，以及 5.1 float32、5.1 int16、7.1.4 float32 三个 channel-bed。
  - 输出用例共 7 个：7.1.4 WAV、双耳 FLAC、5.1 i24 加响度归一化、HOA3 WAV、5.1 trim、7.1.4 Opus MKA、i16 双耳加峰值限制。
  - 63 组输出逐字节相同。其余 18 组的样本一致，差异只在以下几处：
    - 渲染时刻写入的 bext 时间戳；
    - FLAC 文件头；
    - MKA 的容器 ID：main 自己连续渲染两次也不相同。MKA 用 ffmpeg 解码后比较，PCM 一致。
- 本分支的 `mradm` 和 C ABI bundle 中已没有 `drwav_*` 符号（main 有 146 个）。bundle 仍然恰好导出 139 个 `adm_*`。

## 参考实现退出

- 单元 `dr_wav`（`tests/reference/dr_wav_reference_test.cpp`）：在默认 OFF 的 `MR_ADM_BUILD_DRWAV_REFERENCE_TESTS` 下编译，由 `mr_adm_drwav_reference_tests` 比较。dr_libs 仍需保留给 dr_flac，因此退役这个单元只删除测试与开关，不涉及依赖获取和许可证条目。

按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)的通用条件退役；登记与文件哈希见 [`tests/reference/retention.json`](../../tests/reference/retention.json)。当前状态：保留（Rust 实现尚未随正式 tag 发布，独立回归与二期需求待评审）。
