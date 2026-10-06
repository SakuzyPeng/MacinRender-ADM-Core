# Rust 整数 WAVE/BW64 迁移

本轮以项目自有 `mradm-wav` 替换 libbw64 的生产用途：整数 ADM 音频读取，以及浮点 WAVE 到整数 PCM 的位深转换。dr_wav、C++ 容器元数据读取／改写、ADM XML 语义和渲染算法保持各自职责。

## 边界与数值契约

`mradm-wav` 只依赖 Rust 标准库，禁止 unsafe；支持可 seek 的输入／输出流、RIFF/RF64/BW64、PCM16/24/32、float32 和对应的 WAVE_FORMAT_EXTENSIBLE；可读取低有效位深左对齐存储（如 24-in-32），有效位深单独记录在 Info 中。AXML 作为原始字节承载，CHNA 提供固定字段结构；未知 chunk 仅索引位置，不加载其 payload。应用当前仍使用原有 C++ 元数据路径。

整数解码返回与 libbw64 0.10.0 相同的 f32。整数编码使用原有规则：f32 限幅到 [-1, 1]，转换为 f64，乘该位深的正最大整数并向零截断。无穷值限幅，NaN 返回 invalid_argument；float32 不限幅。兼容承诺针对 PCM 字节，不要求 RIFF 的 padding、JUNK 或 chunk 顺序与旧库相同。

默认 writer 先写 RIFF；长度需要 64 位时，整数升级为 BW64、浮点升级为 RF64。也可显式指定容器，BW64 输出拒绝浮点。采样率为 u32，不再有 65535 Hz 限制。最终产品输出的布局和元数据封装继续由 C++ 收尾流程决定。

writer 必须显式 `finish()`，回填长度和 padding 并 flush；析构只释放资源。reader 的 u64 绝对帧定位会限制到 EOF，不读取 data 之后的元数据。截断、无效格式、溢出、I/O 和 finish 错误通过 Result 返回。不可恢复的音频 I/O 错误使句柄失效，禁止继续使用残留样本。

## C++ 与 FFI

私有 `mradm_wav_*` 接口加入现有 mradm-ffi 静态库，使用 opaque 句柄、按帧批量处理、调用方错误缓冲区以及 Rust 配对销毁。Rust 边界捕获 panic；公开 C ABI 的符号、结构和版本不变。

私有 FFI 路径统一使用 UTF-8。Windows C++ 包装层先按当前进程代码页将原生窄字符路径转换为 UTF-8，与既有 C++ WAVE 文件操作保持一致；声明 UTF-8 代码页的 GUI 同样适用。不得通过字节是否为合法 UTF-8 来猜测原生路径的编码。

RenderInputReader 整数分支使用 Rust，浮点和 channel-bed 分支当时继续使用 dr_wav（后续已迁移，见 [Rust 浮点 WAVE 迁移](RUST_DR_WAV_MIGRATION.md)）。read 返回 Result，seek_frame 使用 u64；所有消费点检查错误和短读后再进入 DSP。整数转换使用排他创建的临时文件，仅在成功 finish、关闭所有文件句柄后安装新输出。失败和取消保留原文件。

## 测试与依赖

默认构建不查找或获取 libbw64。普通测试使用独立的小型 RIFF/PCM 夹具作者，C ABI 消费者测试继续只链接公开 ABI 目标。低层 Rust 测试用手工字节结构、硬编码量化边界、可注入故障的流以及虚拟稀疏流验证实现。

`MR_ADM_BUILD_LIBBW64_REFERENCE_TESTS=ON` 启用 libbw64 0.10.0 对照：三种容器 × 三种整数位深 × 1/2/11/128 声道，以及每位深 65536 个编码样本。已有 `MR_ADM_BUILD_LIBADM_REFERENCE_TESTS` 同样需要旧 libbw64 生成历史对照输入。两个开关默认关闭，旧库仅作为可选测试依赖保留许可证。

真实大文件测试默认忽略：设置 `MRADM_WAV_LARGE_TEST_DIR` 为已挂载的临时目录，在 Release 运行 `real_large_writer_promotes_and_reads_back`；测试实际跨过 4 GiB 并在结束时删除文件。常规测试使用稀疏流覆盖大偏移，不要求外置磁盘。

## 验收记录

2026-10-05 已完成 macOS arm64 与 Windows MSVC 验收，摘要见 [acceptance.json](../../tests/reference/libbw64/acceptance.json)。

- macOS 默认 Debug CTest 64/64；开启两个旧库对照时 66/66。Windows 规范 Release 构建默认 CTest 63/63，两项旧库对照均通过。
- libbw64 对照矩阵共 36 组解码／定位，16/24/32-bit 每种 65536 个编码样本；PCM 全部一致。
- 两平台各 12 组 Release 渲染比较，覆盖 Objects、DirectSpeakers、HOA 输入和 f32/i16/i24 输出；PCM、格式字段、AXML/CHNA 和语义报告全部一致。
- Rust WAVE 默认测试 7 项通过；Release 加入实际 >4 GiB 写入测试后 8 项通过。覆盖低有效位深 extensible PCM、超长定位、ds64 重复 FourCC 表、I/O/finish 故障和异常输入；大文件已清理。
- 两平台 C API bundle 均导出与现有头文件一致的 139 个 adm_* 符号，无私有 mradm_wav_* 导出。默认依赖记录中无 libbw64。
- Rust fmt、clippy（-D warnings）、变更 C++ 质量门禁、许可证清单与 SBOM 检查通过；保留既有 Apple／fixture 的非阻断风格告警。

Release 共享库全量构建还补齐了 C API bundle、core/EAR/channel-bed 测试及 Apple/APAC 测试的直接链接依赖；没有改变公开 ABI 或渲染算法。原有未提交的 Triple Balance 链接修正保留。

长期代码、测试和摘要位于仓库；大文件临时材料可放在 cache 外置盘，永久构建不依赖其挂载状态。

## 参考实现退出

- 单元 `libbw64`（`tests/reference/libbw64/`）：在默认 OFF 的 `MR_ADM_BUILD_LIBBW64_REFERENCE_TESTS` 下编译，由 `mr_adm_libbw64_reference_tests` 比较。

按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)的通用条件退役；登记与文件哈希见 [`tests/reference/retention.json`](../../tests/reference/retention.json)。当前状态：保留（Rust 实现尚未随正式 tag 发布，独立回归与二期需求待评审）。
