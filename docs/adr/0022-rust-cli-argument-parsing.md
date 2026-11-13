# ADR 0022：Rust CLI 参数解析采用 clap，退出码沿用 CLI11

> 状态：已接受（用户决定 2026-11-12，六期 6.4 落地）
> 日期：2026-11-13
> 适用范围：`mradm` 命令行（`rust/crates/mradm-cli`）。GUI、C ABI 调用方不受本 ADR 约束。
> 取代：[ADR 0006](0006-cli-argument-library.md)。

## 背景

[ADR 0021](0021-rust-primary-project.md) 把 `mradm` CLI 排在六期改为 Rust（6.4）。原 CLI 用 CLI11 解析参数、spdlog 写日志。
用户在 2026-11-12 确定六期的 CLI 契约：选项名、取值与默认值、stdout、输出文件、退出码、领域错误的中文消息不变；
`--help` 的排版与参数解析错误的措辞可以变化，写进发布说明；解析器用 clap，退出码另行映射为与 CLI11 相同。

## 决策

1. **clap 4.5，builder API。** 版本 `=4.5.41`，关闭默认 features，只开 `std`、`help`、`usage`、`error-context`：不用 derive
   （不引入过程宏依赖），不用 color、suggestions、wrap_help。依赖只多 `clap_builder`、`clap_lex`、`anstyle`，均为 MIT OR Apache-2.0。
2. **选项表单独描述，clap 只负责分词与帮助。** 每个子命令的选项写成一张表（名字、别名、类型、取值集或范围、必选、依赖、互斥），
   由表生成 clap 命令；所有值以原始字节交给 clap，取值检查、转换与必选关系按 CLI11 2.5.0 的顺序在表上进行：
   - 行尾缺值：114；`--x=` 后仍有参数时，照 CLI11 消费下一个参数；
   - `-h` / `--help`：帮助，0；
   - `--version`，然后按表序逐个选项：不在取值集或范围内 105、无法转换 104、单值选项给了两次 114；
   - 缺子命令 106，再按表序：缺必选项 106、缺所需选项 107、互斥 108；
   - 多余参数：109（clap 报出的未知参数逐个移出后重新解析，留到最后报告）。
   整数照 CLI11 的 `strtoull` 进制规则（`0x`、前导 `0` 八进制、`0o`、`0b`、数字分隔符、尾部空白），布尔照 `to_flag_value`，
   小数照 `mradm-dsp` `decimal`（与原 CLI 同一解析器）。组合短选项中的 `-x=值` 同样保留前导 `=`。
   macOS／Windows 的 base-0 转换不自动识别 `0b`，无符号整数保留 CLI11 的独立二进制前缀处理。
   错误消息沿用 CLI11 的措辞，Windows stdout／stderr 保留 CRT 文本流的 LF → CRLF 转换。
3. **CLI 只构造请求。** `mradm-cli` 保持 `#![forbid(unsafe_code)]`，需要引擎的操作经 `Service` trait：渲染、场景导入与检查、AXML、
   策略模板、export、房间包、APAC 编码 worker 与 SIGINT。`mradm-capi` 实现它，用的是 C ABI 导出同样的入口（渲染驱动、岛、
   消息缓冲），CLI 与 C ABI 因此不会各自漂移。目录、布局与格式表直接读 `mradm-engine` / `mradm-scene`。
4. **入口。** 6.6 构建改以 Cargo 为主之前，`mradm` 仍由 CMake 链接：`src/adm_cli/main.cpp` 只调用 `mradm-capi` 的
   `mradm_cli_main`（私有入口，声明在 `src/adm_cli/cli_ffi.h`，FFI 头校验核对），参数由 Rust 从进程读取（Windows 为宽字符命令行）。

## 备选方案

- **手写 CLI11 兼容解析器。** 能逐字节复刻全部边角，但与用户“解析器用 clap”的决定不符，也失去 clap 的帮助生成。
- **clap derive。** 引入 `clap_derive`、`syn`、`quote`、`proc-macro2` 的构建期依赖；表驱动的检查顺序本来就要单独描述，derive 不省事。
- **lexopt / pico-args。** 依赖更少，但帮助文本要手写，选项表与帮助会各写一份。

## 后果

- 帮助排版改为 clap 格式（选项与说明文字不变）；解析错误的措辞在 Linux 的约 150 条探针上与 CLI11 逐字节相同，但不作为承诺。
- 已知边角：CLI11 先完成整行分词再处理 `--help`，clap 遇到 `--help` 即停止；只有在 `--help` 之后还有其它分词错误时才可能不同，
  行尾缺值已在预扫描中照 CLI11 报 114。
- CLI11 与 spdlog 移出构建与许可证清单（日志格式 `[level] message` 由 Rust 写出，彩色终端上照 spdlog 着色；Windows 控制台不着色）。
- 将来改选项必须同时改选项表与 `cli-commands.json` 的记录，并保持 `cli-transcript.py` 对照 0 差异。
