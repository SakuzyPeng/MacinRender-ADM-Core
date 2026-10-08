# Rust PoseBridge OSC 协议解析迁移

> 2026-10-08：PoseBridge 协议 3 的 OSC 数据报解码、source_id 校验和姿态流顺序判定迁入 Rust `mradm-osc`。

## 模块与边界

OSC 头追踪接收器绑定回环 UDP 端口，`recvfrom` 读到的每个字节都来自网络。原先由 C++ 手写解析：
OSC 字符串分帧、大端整数与浮点、UTF-8 校验，以及用 nlohmann_json 解析遥测载荷。这是项目里
唯一直接处理不可信网络输入的手写 C++ 解析代码，因此整体迁入 `#![forbid(unsafe_code)]` 的新 crate。

- `rust/crates/mradm-osc`：`decode`、`valid_source_id`、`SourceOrder`。依赖 `mradm-dsp` 的
  `scene_math::pose`（原 C++ 解码同样经 FFI 调用它）和 `serde_json`（开启 `std` / `float_roundtrip`，只用无类型 `Value`）。
- `rust/crates/mradm-ffi/src/osc.rs` 与手写头 `src/adm_realtime/osc_ffi.h`：
  - `mradm_osc_decode` 把结果写入调用方的 POD 结构；遥测 source_id 来自 JSON 转义，因此复制到
    256 字节定长数组；JSON 原文只以偏移/长度指回调用方数据报。
  - `MradmOscSourceOrder` 是由 C++ 持有的 POD 状态，零初始化即初始状态，不需要句柄。
  - panic 一律视为拒绝，不穿过 C 边界。
- C++ `osc_head_tracking_protocol.{h,cpp}` 保留原有 API（`decode_head_tracking_osc`、
  `valid_source_id`、`OscSourceOrder`），改为薄包装，并校验 Rust 返回的长度与偏移。
  `osc_head_tracking.cpp` 和 `adm_c_api.cpp` 不变。

socket 创建、回环来源过滤、快照/遥测缓存与公开 C ABI 仍在 C++。接收器构造快照 JSON 时会再用
nlohmann 解析已经接受的遥测原文，所以 Rust 的接受范围必须与 nlohmann 完全一致。

## 行为等价

接受的语法以冻结的旧实现为准，逐条保留：
- 8 KiB 上限、4 字节对齐且填充为零；
- 姿态包的固定 tag 串、精确长度、字段的正数与 int64 范围、`source_age_at_send_ns < 0.5 s`、
  sample_time_kind 规则；
- 遥测的十进制字符串标识、内外层一致、坐标约定和 11 种状态白名单；
- `/posebridge/v*` 与非 3 的 schema/协议号判为不兼容。

serde_json 与 nlohmann 之间的差异逐项对齐：

| 方面 | 处理 |
|---|---|
| 开头 UTF-8 BOM | nlohmann 跳过；Rust 解析前去掉，保存的原文仍含 BOM |
| 嵌套深度 | nlohmann 回调在任一 token 位于 ≥17 层未闭合容器内时拒绝（第 17 层空容器可接受）；Rust 解析前按同一规则扫描 |
| 重复键 | 两者都取最后一个 |
| 超出 f64 的数字（如 `1e400`、`1.79769313486231581e308`） | 启用 `float_roundtrip` 精确解析；两者都拒绝整条消息，有限值仍接受 |
| `-0`、`3.0`、`3e0` 作为 schema | 两者都不是无符号整数，拒绝 |
| 非字符串字段、孤立代理、非法 UTF-8、控制字符、注释、尾随内容 | 两者都拒绝 |

没有有意改变的接受范围。

## 验收结果

迁移初验（Linux x64，GCC 13，Debug）：

- `mr_adm_osc_protocol_reference_tests`：冻结 C++ 解码器与 Rust 逐字段比较，共 285068 个数据报，其中 5650 个被接受。
  比较项包括接受与否、kind、四元数和欧拉角的 float 位模式、全部 timing 字段、source_id、JSON 原文和遥测字段。语料包括：
  - 合法报文及其每个截断前缀；
  - 约 80 种 JSON 边界：深度 14–18、BOM、数字边界、转义和重复键；
  - 2 万个姿态字段边界组合；
  - 10 万次字节级变异，以及 10 万次 JSON 级变异（变异后重新封装成 OSC，让两边的 JSON 解析器都真正处理到）。

  另外，source_id 校验做了 20 万例随机比较，顺序判定做了 2000 条随机流、每条 64 步。
- 灵敏度检查：把深度上限改成 18，或去掉 BOM 处理，差分测试都会立即失败。
- `mr_adm_osc_head_tracking_tests`（原有真实 socket、遥测与 C ABI 回归）不改，全部通过。
- `cargo test -p mradm-osc`：覆盖正常报文、每条拒绝规则、UTF-8 边界、深度边界、顺序判定，
  以及 10 万次变异不 panic。

新增 Rust 依赖为 serde_json 1.0.140 及 serde / serde_core 1.0.229、itoa 1.0.18、ryu 1.0.23。
serde_derive 只因 serde 的版本锁定写进 Cargo.lock，并不参与编译。这些依赖都登记在许可证清单、SBOM 和 `third_party/licenses/`。

### JSON 浮点边界回归

`serde_json` 默认浮点解析会在 f64 溢出临界值处与 nlohmann 不一致：
`1.79769313486231581e308` 会被错误接受，写入遥测缓存后使 C++ 快照重解析抛出
`out_of_range.406`，公开 C ABI 返回 `ADM_ERROR_INTERNAL`；部分有限值也会被错误拒绝。
启用 `float_roundtrip` 后按精确舍入判定，依赖版本和公开 ABI 均不变。

新增回归覆盖 info/status、正负数、科学记数法和长整数表示；对照旧解码器验证接受范围，
并通过真实回环 UDP 和 `adm_osc_head_tracking_snapshot_json` 验证溢出报文被拒绝、
既有缓存保持可读，最大有限值仍可进入快照。

2026-10-08 macOS arm64 Debug 验证：新增 Rust 测试和两组 C++ OSC 测试在修复前均失败，
启用精确解析后全部通过。OSC 差分语料增至 294704 个数据报（5641 个接受），
10 个 OSC Rust 单元测试与 7 个 OSC/HpTF CTest 全部通过。

## 复现

```bash
cargo test --manifest-path rust/Cargo.toml -p mradm-osc --locked
cmake --build build/debug --target mr_adm_osc_protocol_reference_tests mr_adm_osc_head_tracking_tests
ctest --test-dir build/debug -R "mr_adm_osc_" --output-on-failure
python3 scripts/quality/check-ffi-headers.py
scripts/quality/check-licenses.sh --build-dir build/debug
```

## 参考实现退出

- 单元 `osc_protocol`（`tests/reference/osc_protocol/legacy.h`、`provenance.json`）：
  - 内容是旧实现合并成的 header-only 版本，命名空间改为 `mradm::osc_protocol_legacy`，算法不变；
  - 由默认构建的 `mr_adm_osc_protocol_reference_tests` 比较。

按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)的通用条件退役；登记与文件哈希见
[`tests/reference/retention.json`](../../tests/reference/retention.json)。当前状态：保留（Rust 实现尚未随正式 tag 发布）。
