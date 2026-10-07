# dr_flac 替换准备与优先级

> 2026-10-07；状态：选型和本机风险验证完成，生产实现未切换。
> 用户已决定暂缓替换，继续保留 dr_flac。以下方案留待后续重新评估，不是迁移验收记录或新增格式支持承诺。

## 保留现有实现的影响

目前不替换的影响较小。生产 `dr_flac` 调用集中在
[`FloatFlacReader`](../../src/adm_audio/flac_io.cpp)：open、close、格式信息与 float32 流式读取。
`ReaderHandle` 的生产使用者是 `apply_gain_to_file` 和 `trim_file_frames`。
实际渲染输入由 `RenderInputReader` → `RustWavReader` 承担；解码器替换不属于
[二期已经验收的 DSP/PCM 一致性矩阵](RUST_PHASE2_CLOSEOUT.md)。

`libFLAC` 仍负责编码和 Vorbis Comment 改写。因此替换 dr_flac 可以减少一份生产 C 解码实现，
并使默认构建不再为它获取 dr_libs，但不能移除 libFLAC 或其发行许可证。
miniaudio 的生产翻译单元已定义 `MA_NO_DECODING`，没有另一个需要一起迁移的内置解码入口。
dr_wav 参考测试仍需要 dr_libs；其保留政策不随本任务改变。

保留的成本主要是依赖更新和输入错误处理维护，不是当前核心渲染的确定性障碍。
本轮没有做吞吐、RSS 或二进制体积基准，不能声称替换会让产品更快、更小。
“继续压缩 C/C++ 依赖面”本身不足以抵消新解码器补丁、FFI、参考测试和许可证维护的成本。

## 候选与结论

| 路径 | 实测和接口情况 | 建议 |
|---|---|---|
| 保留锁定的 dr_flac | 本轮已有实现可正确解码的 33 个夹具全部符合独立 PCM oracle；错误只能表现为短读/读零 | 当前默认保持；错误表达可以独立加固 |
| Symphonia 0.6.1，关闭默认 features，只启用 `flac,ogg` | 上述 33 个夹具全部符合 oracle；支持原生 seek 和 Ogg；另发现 32-bit 立体声错误及无长度截断检测缺口 | 后续 Rust 迁移的首选研究对象，尚不满足直接切换条件 |
| Claxon 0.4.3 | 常见位深和多声道可用；没有原生 seek；本轮 4-bit、32-bit、Ogg、ID3 前缀用例失败 | 不作为当前迁移首选 |
| 使用已经链接的 libFLAC 解码 | 本轮未实现或测量；不会新增一套 FLAC 原生库，但也不实现纯 Rust 解码目标 | 如果将来目标仅是移除 dr_libs，可重新比较此方案 |

Symphonia 的 FLAC bundle 自身禁止 unsafe；这不是整个传递依赖图无 unsafe 或无解析错误的证明。
此次 features 未启用可选 SIMD，也未引入其他音频编解码器；传递依赖包括 core、common、metadata、
Ogg、lazy_static、regex-lite 等。它并非单头库的一对一轻量替代。

Symphonia 为 MPL-2.0，Claxon 为 Apache-2.0。若将 Symphonia 纳入生产，需要按
[依赖管理政策](../adr/0004-third-party-dependency-management.md)登记完整 Cargo 锁定依赖、许可证、
SBOM 和源码获取说明；修改的 MPL 文件保留相应许可和源码提供义务。本轮候选只存在于独立实验
manifest，不进入生产 `rust/Cargo.lock`、CMake、发行包或当前生产许可证清单。

## 可复现的本机证据

原始报告：[`selection.json`](evidence/rust-flac/selection.json)。
复现入口：[`scripts/flac-eval/run.py`](../../scripts/flac-eval/run.py)，使用独立锁文件，复用现有
Cargo 目标目录与 Release libFLAC，不建立新的项目构建缓存。夹具在临时目录生成并删除。

环境为 macOS arm64、Rust 1.98.0 Release、C++ `-O2`、libFLAC CLI 1.5.0；精确工具链信息、
源码指纹、每个输入和输出的 SHA-256 均在报告中。这里只验证解码行为，不作为性能或试听证据。

- 32 个基础夹具：4/8/12/16/20/24/32-bit × 1/2/6/8 声道，以及 24-bit 的 3/4/5/7 声道。
  采样率覆盖 8/44.1/48/96/192 kHz。每个夹具 8193 帧，包含整数极值、噪声、常量、斜坡、
  相关声道和可产生 wasted bits 的样本。
- 加入未知总长度、有/无 MD5、Vorbis Comment 的 Unicode/声道掩码、APPLICATION、1 MiB padding、
  未知 metadata、SEEKTABLE、Ogg FLAC 和 ID3v2 前缀，共 38 个有效/兼容性夹具。
  解码兼容不等于已经验证标签读取、标签保留或声道掩码重排。
- 10 个损坏夹具和 1 个无总长度/无 MD5 的完整短前缀，共 49 个输入。
- 5 种输入各 8 个 seek 位置，共 40 个探测；其中范围内 30 个位置在 Symphonia 上全部逐位正确。
  每个 seek 使用新句柄，尚未覆盖同一句柄连续后退、失败恢复和 seek 后校验状态。

夹具由 libFLAC 编码，并由 `flac -t` 检查有效输入。正确答案来自生成前的整数 PCM，独立于三个
被比较解码器；同时比较左对齐 i32 和归一化 f32 的位模式。正确性不以旧 dr_flac 为唯一依据。
参考 dr_flac 使用循环 `[1,7,127,511,1024]` 请求；Symphonia 按其自然 packet 边界输出，
因此本轮尚未验证未来适配层的任意分块缓存。

| 解码器 | 38 个有效/兼容性夹具的 PCM 完全相同数 |
|---|---:|
| dr_flac（仓库锁定版本） | 33 |
| Symphonia 0.6.1 | 37 |
| Claxon 0.4.3 | 28 |

dr_flac 的另外 5 个输入是 4 个带新版 32-bit frame header 的夹具和 ID3v2 前缀；它们不能作为
“当前产品已经支持”的证据。Claxon 还失败于 4-bit header 从 STREAMINFO 取位深和 Ogg。
报告中的 `pcm_exact` 只比较输出字节：seek 报错且期望为空时也可能是 true，不能单独算通过。

## 已确认的风险

### 32-bit 立体声不是只差浮点舍入

`pcm32-2ch` 经 libFLAC 验证有效。Symphonia 在第 4096 帧左声道（从 0 开始计数）首次偏离：
期望整数 `2147483647`，实际为 `-1`；完整解码 MD5 也返回 `Some(false)`。
所有帧仍被返回，时间戳连续，因此仅比较帧数无法发现它。

0.6.1 的 stereo decorrelation 使用 i32 中间值，Side 可能需要 33 位。这个源码结构与观测相符，
但本轮没有修补并做因果对照，不把定位方向写成已经完成的修复。
未来若接受 32-bit 输入，必须覆盖 Independent/LeftSide/RightSide/MidSide 和宽位深 LPC/residual，
在修复或明确报 `unsupported` 前不能返回错误 PCM。不能因旧版本也不支持这些 fixture 就省略此门禁。

### EOF 不足以证明文件完整

Symphonia 在 `crc-first`、`crc-middle` 上跳过损坏帧，最终返回 EOF；报告同时出现时间戳缺口、
少 1024 帧和 MD5 不符。`missing-frame-no-total-no-md5` 也返回 EOF，只能从时间戳缺口发现问题。
适配层必须显式检查这些状态，不能照搬示例中跳过解码错误的循环。

更强的反例是 `truncated-packet-no-total-no-md5`：尾部只剩半个 packet，Symphonia 返回正常 EOF，
已有 PCM 的时间戳也连续，MD5 为 None。仅包装 `next_packet()`、检查帧数/时间戳/MD5 仍不足以
严格检测此输入，需要修正或补充 demuxer 的尾部完整性检查。

`prefix-no-total-no-md5` 则恰好在完整帧边界结束，是无法从该文件自身区分的完整短流。
没有总长度、MD5 或外部约束时，不能承诺识别这种丢失尾部的情况。

### 现有 API 也有可以独立处理的问题

- `FloatFlacReader::read` / `ReaderHandle::read` 只返回帧数，无法区分正常 EOF 与解码错误。
  gain/trim 已检查已知长度下的短读，不应误写成它们目前会无条件安装所有截断结果。
- FLAC 的 `total_samples=0` 表示未知长度。现有 `frame_count()` 返回 0，gain 的按长度循环可能
  跳过实际音频；这是静态调用链发现，尚未补做产品级故障复现。
- 目前 FLAC 类和 `ReaderHandle` 没有 seek；trim 通过读取并丢弃前段定位。
  新库提供 seek 不会自动改善现有产品行为。
- 当前 reader 在外层析构函数释放裸句柄，默认 move assignment 会销毁不负责 close 的旧 Impl。
  后续调整所有权时应把关闭责任放进 Impl，并测试移动赋值释放旧句柄；本轮未修改该实现。

## 未来迁移的顺序与门槛

1. **先完成严格解码契约和候选修复。** 保持生产默认不变；处理以上 32-bit 和截断反例。
   普通 Rust 模块拟为 `mradm-flac`，不含 FFI unsafe。若需要 vendor 补丁，锁定来源、最小差异、
   许可证和独立回归，不修改 Cargo 全局 registry 中的源码。
2. **再接私有 FFI 和 C++ 适配。** 沿用 `mradm-ffi` 的 opaque handle、UTF-8 路径、分配侧释放、
   panic 捕获和项目错误码；公开 C ABI 保持不变。保留旧 C++ read 签名作为兼容层，同时让内部
   gain/trim 使用能表达错误的读取入口。写入临时文件的路径只在读取完整性检查通过后安装结果。
3. **明确长度、seek 和状态。** 未知总长度不得当成空文件；优先考虑只对这类文件做固定内存的
   完整校验/计数扫描，再重新打开，以满足现有 frame_count 和 trim 的精确长度需求，并测量开销。
   范围内 seek 采用 accurate seek 加 packet 内丢弃；EOF/越界统一为项目约定，不能透传各容器
   不同的错误。seek 后废弃旧缓存；部分解码不得冒充全文件 MD5 校验通过。
4. **通过回归后才切生产。** 独立测试保护整数和 f32 位模式、任意短块/短尾、EOF、已知和未知长度、
   CRC/MD5、时间戳缺口、非法 metadata、受限内存下的超长声明、Windows Unicode 路径、反复 seek、
   移动/析构及原文件在失败时不被替换。增加真实文件、全部目标位深/声道组合和不同编码方式；
   本轮小型合成矩阵不是完整 FLAC 标准验收。macOS/Linux/Windows 做 Debug 正确性及 Release
   PCM 对照、吞吐/seek/RSS 测量；Windows 使用规范 MSVC/Ninja 构建树。
5. **最后清理构建依赖。** 将 dr_flac 移入默认关闭的参考测试，登记 `tests/reference/retention.json`；
   dr_libs 仅在 dr_flac 或 dr_wav 参考测试启用时获取。更新生产依赖清单、FFI 头校验及导出检查，
   并运行现有三平台一致性矩阵，不放宽原有门禁。参考实现按既有退出政策保留。

恢复这项替换工作的合理触发条件：具体 FLAC 兼容性/维护问题需要解决、项目明确要求进一步减少
生产 C 解析代码，或候选版本已经消除上述阻塞且测量表明维护成本可接受。
当前更有直接收益的小切片是先验证并修正未知长度、错误传播和句柄所有权；它们不必绑定 Rust 迁移。

## 本轮检查与限制

- 独立 Release 探测完成 49 个输入和 40 个 seek；失败作为选型证据保留，没有把它们标记成迁移通过。
- 实验 Rust 代码通过 rustfmt 和 clippy `-D warnings`。
- 现有 Debug `mr_adm_flac_io_smoke_tests`、`mr_adm_trim_file_smoke_tests` 两项通过。
- 本轮不修改生产代码、生产依赖锁定、二期历史证据或参考退役状态；未执行跨平台产品验收。
- 已核查 0.6.1 的通用 boxed-slice 读取改为逐步增长，不能直接套用旧版本的长度分配缺陷报告；
  metadata 累计预算和损坏输入的内存上限仍须纳入后续测试。

版本与源码入口：[Symphonia 0.6.1](https://docs.rs/crate/symphonia/0.6.1)、
[FLAC bundle 0.6.1](https://docs.rs/crate/symphonia-bundle-flac/0.6.1/source/src/decoder.rs)、
[Claxon 0.4.3](https://docs.rs/crate/claxon/0.4.3)。
