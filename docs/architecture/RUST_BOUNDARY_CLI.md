# CLI 改为 Rust（六期 6.4）

> 状态：已完成（2026-11-13），本机 Linux x64 验收；macOS / Windows 原生验证待维护者。
> 路线见 [PCM 一致性审计六期一节](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)，解析器决定见 [ADR 0022](../adr/0022-rust-cli-argument-parsing.md)。

`mradm` 的全部子命令（`render`、`inspect`、`backends`、`layouts`、`input-layouts`、`formats`、`export`、`room-check` 与内部的
`__apac-encode`）改由新 crate `mradm-cli` 实现。`src/adm_cli` 由 1,410 行降到 19 行（只剩调用 Rust 入口的 `main`），
CLI11 与 spdlog 移出构建与许可证清单，新增 clap 4.5.41（及 `clap_builder`、`clap_lex`、`anstyle`）。

## 结构

- **`mradm-cli`**（rlib，`#![forbid(unsafe_code)]`）：
  - `args.rs`：每个子命令的选项表（名字、别名、类型、取值集或范围、必选、依赖、互斥），由表生成 clap 命令，再照 CLI11 2.5.0 的顺序检查：
    行尾缺值 / `--x=` 空值 114 → 帮助 0 → `--version` 与按表序的取值检查（105 / 104 / 114）→ 缺子命令与必选、依赖、互斥（106 / 107 / 108）→
    多余参数 109。`-x=值` 照 CLI11 把 `=值` 当作值（clap 会去掉 `=`），错误消息沿用 CLI11 的措辞。
  - `convert.rs`：CLI11 的整数（`strtoull` 进制规则、数字分隔符、尾部空白、`0o` / `0b`）、布尔与 flag 取值转换。
  - `catalog.rs`：`backends`、`layouts`、`formats`、`input-layouts` 直接读 `mradm-engine` 的目录与表、`mradm-scene` 的输入布局表，
    排版照 fmt（宽字符按两列补齐）。
  - `output.rs`：spdlog 的 `[level] message` 日志（stdout，彩色终端上照 spdlog 着色），`lib.rs`：`render` 的请求构造、进度与日志行、
    Ctrl-C（25 ms 轮询的看守线程，退出码 130），以及 `inspect`、`export`、`room-check`、`__apac-encode`。
  - 需要引擎的操作经 `Service` trait。
- **`mradm-capi` `cli.rs`** 实现 `Service`，用的是 C ABI 导出同样的入口：渲染照 `RenderService::render` 驱动引擎的分步状态机并在步内执行岛
  （`render.rs` 的驱动抽出 `drive` 与 `Sink`，C ABI 与 CLI 共用），场景导入、AXML、策略模板、export（`export_logged` 带出策略日志）、
  房间包与 `inspect_room` 复用 C ABI 的函数和消息缓冲；屏幕配置照 `load_screen_config` 读文件后复用 Scene 的解析与校验。
  新导出 `mradm_cli_main`（私有入口，声明在 `src/adm_cli/cli_ffi.h`，`check-ffi-headers.py` 核对）。
- **APAC 编码 worker**：`run_apac_encode_child` 经新的岛接口 `src/adm_audio/apac_worker_island.{h,cpp}`（`mradm_audio_apac_child`，
  登记在 `island-interfaces`）调用，心跳协议行仍由 C++ 写出。单遍链接器的 `--undefined=` 列表加上这个符号；`mradm` 可执行文件改为链接
  `mr_adm_c_api`（进度回调跳板与 `--undefined=` 选项都在那里）。
- **Windows 路径**：参数从宽字符命令行读出，以 UTF-8 交给引擎；C ABI 的辅助函数把调用方路径视为代码页窄字符串，CLI 调用它们时经
  `error::with_utf8_paths` 跳过这次转换。

## 发布说明

- `mradm` 由 Rust 实现；子命令、选项名与取值、默认值、stdout、输出文件、退出码与领域错误消息不变。
- `--help` 改为 clap 的排版（选项与说明文字不变，`__apac-encode` 仍不显示）。
- Windows：命令行参数不再经过当前代码页（以前无法用代码页表示的字符被替换），日志与消息以 UTF-8 写出；控制台日志不再着色。
- 构建不再获取 CLI11 与 spdlog。

## 验证

本机 Linux x64 Release（一致性 config A），参照为 main `cad0993` 的同配置构建（`d05e869` 之前只多 Monitor 的 C ABI 修正，CLI 不变）：

- **CLI 记录**（`cli-transcript.py`，`--tool mradm=参照`）：102 条命令 0 差异，退出码与全部工作目录文件变化相同；只有 3 条 help
  （`help`、`backends-help`、`render-help`）文本不同，属允许的排版变化；
- **解析探针**（临时脚本，未入库）：约 150 条边角命令行（缺值、重复、`=` 写法、进制与分隔符、flag 取值、`--`、未知参数与子命令、
  `--help` / `--version` 与错误的先后、各子命令的必选 / 依赖 / 互斥）分别跑两个可执行文件，退出码、写出的文件、stdout 与 stderr 全部相同；
- **C ABI 采集**（`capi-capture.py`，`--library 参照`）：3,283 行、36 个文件 0 差异（渲染驱动与 export 的重构不改变 C ABI），
  consumer 测试在两个库上都通过；共享库恰好导出 164 个 `adm_*`，新增的未定义符号只有 libc 的 `isatty` 与 `__printf_chk`
  （CLI 代码与 APAC worker 随同一目标文件进入共享库，约 0.5 MB，6.6 分出 cdylib 后不再带入）；
- **完整一致性采集**（`collect.py --tool mradm=参照`，`compare-collections.py`）：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、
  日志 666 全部相同；
- ctest 86/86（含 `mr_adm_cli_smoke_tests`；`boundary_tools_test` 改为从 Rust 选项表读子命令）、`mradm-cli` 单测 7 个、`mradm-capi` 单测 52 个、
  FFI 头校验（私有入口 1 个：`mradm_cli_main`）、导出校验、许可证与 SBOM、参考登记、质量脚本测试、Rust fmt / clippy（Linux 与
  `x86_64-pc-windows-msvc`）、改动 C/C++ 的 clang-format / clang-tidy / cppcheck；
- 规模：生产 C/C++ 16,705 → 15,314 行（`src/adm_cli` 1,410 → 19），`island-interfaces` 988 → 1,026 行（新增 `apac_worker_island` 38 行）。

## 下一步

macOS：`mr_adm_apac_smoke_tests`（APAC 经 `__apac-encode` worker）与 CLI 对照；Windows：canonical 构建上的 CLI 对照与非 ASCII 路径。
6.5 删除 C++ 外壳与 SDK；6.6 改由 Cargo 产出 `mradm` 时去掉 `main.cpp`。
