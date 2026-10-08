# Rust HpTF ParametricEQ 文本解析迁移

> 2026-10-08：AutoEq ParametricEQ 文本解析迁入 Rust `mradm-dsp` 的 `hptf::parametric_eq`。
> 本文接续 [Rust HpTF 耳机补偿 DSP 迁移](RUST_HPTF_MIGRATION.md)；那次迁移把文本解析留在了 C++。

## 模块与边界

ParametricEQ 文本是用户输入，来源有三种：耳机补偿文件、GUI 粘贴，以及 C ABI `adm_hptf_parse_parametric_eq`。
解析原先由 C++ 手写，包括按行切分、空白分词、关键字查找，以及用 `istringstream` 在 classic locale 下读数字。

- `rust/crates/mradm-dsp/src/hptf/parametric_eq.rs` 实现文本到 `Profile`（前级加 band 列表）的解析，失败时返回
  错误种类和出错行在原文中的字节范围。算法库继续 `#![forbid(unsafe_code)]`，没有新增依赖。
- `mradm-ffi` 的 `mradm_dsp_hptf_parse` 把 band 写入调用方缓冲区，同时总是报告完整的 band 数，
  不够时调用方扩容重试。C++ 先给 16 个槽位，超出时按报告的数量重试一次。
- C++ `parse_parametric_eq` 保留原签名，Rust 只返回错误种类，原有的中文错误消息和行上下文仍由 C++ 生成。
  范围校验 `validate_hptf_profile` 不变：内存参数接口同样调用它，而且文本解析本来就不包含范围检查。
- 文件读取 `load_parametric_eq_file`、C ABI 和实时链路都不变。

## 行为等价

接受的语法以冻结的旧实现为准：
- 按 `\n` 分行，去掉首尾空白（因此 `\r` 也被去掉），跳过空行和以 `#` 开头的行；
- `Preamp`/`Preamp:` 后的第一个 token 必须是数字，出现多行时最后一行生效；
- `Filter` 行要先找到 ON/OFF；之后没有类型词，或类型词不认识，这一段跳过；
- `Fc`/`Gain`/`Q` 在整行里按关键字查找第一处，后面紧跟的 token 必须是数字；
  缺少关键字时，Fc 记为 0、Gain 记为 0、Q 记为 0.707；
- 没有任何可用行时报错。

C locale 的细节按原实现逐项保留：
- 空白是 C `isspace` 的六个字节，包括 Rust `is_ascii_whitespace` 不认的 `\v`；
- 大小写不敏感只作用于 ASCII；
- 数字必须是完整的 token 且有限：
  - `1.`、`.5`、`+2`、`1E2`、真零 `0e-400` 都接受；
  - `1e`、`1,5`、十六进制、`inf`/`nan`、溢出的 `1e400`、带单位的 `1Hz` 都拒绝。

旧实现读数字依赖平台 C++ 标准库的 `num_get`。Rust 用的是与平台无关、正确舍入的十进制解析。

**有意统一的一处**：下溢写法，即尾数非零、但舍入后变成 0 或次正规数的数字（如 `1e-400`、`1e-310`）。
旧实现在这里三平台不一致：libstdc++（Linux）把它当作 0 或次正规数接受，libc++（macOS）和 MSVC（Windows）
因 ERANGE 拒绝。这是首轮 CI 在 macOS/Windows 上发现的。Rust 统一为拒绝，与两个桌面平台的旧行为一致，
也避免手误把参数悄悄变成 0。差分测试把含这类 token 的文本单列出来，不和旧实现比较，改为直接断言 Rust 拒绝；
同时确认 `0e-400`、`-0.0` 这类显式零仍能解析。除此之外，三平台上新旧实现对整个差分语料逐项一致。
旧实现的 `isspace`/`tolower` 跟随进程的 C locale；Rust 固定用 C locale 的规则。项目从不修改进程 locale，
所以对现有调用方没有行为变化。

## 验收结果

本机（Linux x64，GCC 13，Debug）：

- `mr_adm_hptf_parse_reference_tests` 把冻结的 C++ 解析器与新实现逐项比较，共 204390 段文本，其中 17696 段解析成功；
  另有 4341 段含下溢写法，按上文单独断言。
  - 比较项：成功时比较前级、band 数，以及每个 band 的类型、开关和三个 double 的位模式；失败时比较错误码、消息和上下文。
  - 语料：
    - 固定用例及其每个前缀，包括 17 个类型别名、35 种数字写法 × 3 个位置、`\v`/`\f`、BOM、NUL、
      非 ASCII 字节，以及超过 16 段（触发扩容重试）、超过 32 个启用段（校验失败）的文件；
    - 10 万段由 AutoEq 词表随机拼接的文本；
    - 10 万次字节级变异。
- 灵敏度检查：从空白集合里去掉 `\v`，或去掉有限性检查，差分测试都会立即失败。
- 原有 `mr_adm_hptf_tests`、`mr_adm_hptf_parameters_tests`、`mr_adm_hptf_regression_tests` 不改，全部通过。
- Rust 单元测试覆盖 AutoEq 样例、关键字查找、数字 token、出错行定位，以及 5 万段随机字节不 panic。

## 复现

```bash
cargo test --manifest-path rust/Cargo.toml -p mradm-dsp --locked parametric_eq
cmake --build build/debug --target mr_adm_hptf_parse_reference_tests mr_adm_hptf_tests mr_adm_hptf_parameters_tests
ctest --test-dir build/debug -R "mr_adm_hptf" --output-on-failure
python3 scripts/quality/check-ffi-headers.py
```

## 参考实现退出

- 单元 `hptf_parse`（`tests/reference/hptf_parse/legacy.h`、`provenance.json`）：
  - 内容是旧的文本辅助函数、`parse_parametric_eq` 和 `validate_hptf_profile`，命名空间改为 `mradm::hptf_parse_legacy`，语法不变；
  - 由默认构建的 `mr_adm_hptf_parse_reference_tests` 比较。

按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)的通用条件退役；登记与文件哈希见
[`tests/reference/retention.json`](../../tests/reference/retention.json)。当前状态：保留（Rust 实现尚未随正式 tag 发布）。
