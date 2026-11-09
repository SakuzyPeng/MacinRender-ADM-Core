# FLAC 读取完整性、未知长度与所有权

2026-11-06。处理 [FLAC 选型记录](RUST_FLAC_DECODER_PLAN.md) 中的三个读取接口遗留项。
本批保留 dr_flac 的 PCM 解码及现有 libFLAC provider，没有引入新的解码库或独立构建缓存。
公开 `c_api.h` 和 FLAC 输出编码规则不变。

## 行为

- `FloatFlacReader::open` 先经 Rust 私有边界 `mradm_flac_validate` 完整校验输入，确认帧序、
  声道／采样率／位深、帧 CRC、可用的 STREAMINFO MD5、声明长度与实际长度。
  解码器报 EOS 后还核对最后一个完整帧的字节位置，拒绝被误当作正常 EOF 的半帧截断及尾部垃圾。
- 声明 `total_samples=0` 时，`frame_count()` 返回校验遍历得到的实际帧数。
  已知长度也完整校验；不会仅凭解码帧数相等就接受缺帧补零、CRC 恢复或错误 MD5。
- `FloatFlacReader::read` 和测试支持中的 `ReaderHandle::read` 返回 `Result<uint64_t>`。
  正常 EOF 为成功的 0，零帧请求不推进状态；dr_flac 未能读满已校验区间时返回持续的 `io_error`。
  `pcm_bits` 传播该错误，失败时不覆盖已有输出。WAVE／CAF 的读取语义沿用原接口。
- 解码句柄由 `Impl` 内的 `unique_ptr<drflac, drflac_close>` 持有。
  覆盖已有实例的移动赋值会立即关闭旧句柄；分配／构造中途失败也按所有权释放。
  Windows 使用 dr_flac 的宽路径入口，与已有 native-path → UTF-8 校验边界配套。

`ReaderHandle` 已经只供 PCM 校验工具和 core 测试调用，因此其声明和实现移至
`tests/support/audio_reader.*`。这是内部 C++ 接口调整（ADR 0021 §4），不增加公开 C ABI。
主线 `152b1bc6` 上的生产 C/C++ 由 29,297 行降至 29,260 行，减少 37 行。
原研究分支验证时为 29,408 → 29,371 行，历史计数保留在证据中。

## 实现边界

新增完整性与计数流程在 `mradm-ffi/src/flac.rs`；所有 native 指针、libFLAC 回调与释放都位于 FFI crate。
Rust 回调不跨 C 展开。libFLAC 继续由 CMake 的既有系统／vendored provider 链接，不由 Cargo 另行构建。
PCM 仍由原 dr_flac 路径返回，其格式能力限制继续存在；校验通过后若 dr_flac 不支持该帧编码，读取会报错。

`mradm-wav/src/flac.rs` 提供有界 native/Ogg 输入。Ogg 1.0 单逻辑流逐页检查 CRC、页序号、
continuation、EOS 和最终 granule position，再将 FLAC 包内容送入 native libFLAC。
因此 vendored libFLAC 的 `WITH_OGG=OFF` 不影响既有单流 Ogg FLAC 校验；不静默选取多路复用或串接流中的一部分。
缓冲按 Ogg 页和解码块分配，不按 STREAMINFO 声明的总长度分配 PCM 或磁盘缓存。
格式依据见 [Xiph Ogg mapping](https://xiph.org/flac/ogg_mapping.html)，回调依据见
[libFLAC decoder API](https://xiph.org/flac/api/group__flac__stream__decoder.html)。

打开文件增加一次完整解码校验，随后 dr_flac 再提供 PCM，因而有额外读取和计算成本；本批没有性能收益声明。
调用者目前是 PCM 校验工具和测试，不在离线渲染、文件流式渲染或 Scene 流式渲染的生产输入链上。
gain／trim 仍按 4.8 的决定拒绝 FLAC 输入。

没有总长度、MD5 或外部长度约束时，恰好截在完整帧边界的短流无法与真实短文件区分，仍可作为短流接受。
本批不把校验视作文件随后被并发改写的快照保证。

## 验证

- **macOS arm64**：Debug 的 FLAC reader、FLAC I/O、core、trim、C API 五项 CTest 通过；
  Release 的四项 C++ 定向 CTest 和 71 项 FLAC reader 用例通过。
- **Windows x64**：canonical MSVC/Ninja、vendored FLAC/Opus、SOFA/IAMF OFF，Release 的同四项 CTest、
  71 项 FLAC reader 用例和 CLI `backends` 通过。复用原 canonical、Cargo 与 FetchContent 缓存。
- **独立 PCM oracle**：Python 直接构造整数样本与 verbatim FLAC，覆盖 4/8/12/16/20/24/32-bit、
  1–8 声道组合、48/96 kHz、短尾、零长度、未知长度、有／无 MD5、native/Ogg 与中文空格路径。
  PCM 位模式与原整数样本的 float32 转换逐字节相同。
- **拒收与保留**：覆盖头部／音频截断、首／中／末帧 CRC、缺帧、重复帧、MD5 不符、声明长度偏大／偏小、
  Ogg 缺页／缺 EOS／CRC／granule position、尾部垃圾；每个失败都检查错误退出和已有输出逐字节不变。
- **所有权**：扩展原 C++ FLAC round-trip smoke，连续 64 次覆盖移动赋值，macOS 文件描述符和
  Windows 进程句柄数均不增长，随后短块读取仍正确。C++ smoke 同时核对所借用的 libFLAC frame-header ABI 布局。
- **Rust**：两平台 Ogg 页检查和跨页 continuation 两项单元测试通过；macOS FFI 单元测试程序可链接；
  `cargo fmt`、针对两个修改 crate 的全 targets Clippy `-D warnings` 通过。
- **质量**：私有 FFI 头校验、变更文件质量检查、补充的未跟踪 C++ 支持文件检查、规模棘轮与 diff 检查通过。
  clang-tidy 保留原编码器及第三方 fmt 的既有诊断；没有把它们计为本批新问题。
- **链接与导出**：macOS Rust 离线一致性工具可独立链接；两平台 Release C ABI bundle 都恰好导出
  原有 164 个 `adm_*` 函数，新增私有验证入口没有泄漏到公开导出面。

以上为本批定向正确性验证，不是一次全仓库 Consistency 或性能验收。
源码身份与结果见[汇总](evidence/flac-reader-integrity-20261106/summary.json)，两平台 71 个输入的指纹逐项一致。
Mac 工作区基于 `10b6b63e`；修复补丁也在 Windows `152b1bc6` main 上重新通过验证。
Windows 临时修改已恢复，回到干净的 `152b1bc6` main 并重建、通过原有定向 smoke；验证时使用临时补丁，未发布二进制。
本机详细记录位于未跟踪的 `local/flac-reader-fix-20261106/`。
