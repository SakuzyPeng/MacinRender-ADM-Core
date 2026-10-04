# Rust Meter 迁移记录

> 2026-10-04：生产计量已统一使用 Rust；公开 C ABI 保持兼容。

## 实现

- `rust/crates/mradm-dsp/src/meter.rs`：安全计量对象，固定 `ebur128 0.1.10`，I、TP、I+TP、M+S+I 四类模式。
- `rust/crates/mradm-ffi/src/meter.rs`：创建/销毁、输入、重置和查询；校验指针、长度、枚举与输出参数，panic 转为错误。
- `src/adm_dsp/meter.h`：项目自有可移动 RAII owner；8 处原生调用已接入，第三方类型不进入公开头文件。
- 原 C 库仅由 `MR_ADM_BUILD_EBUR128_REFERENCE_TESTS=ON` 引入参考可执行文件，默认 OFF，生产 target 不链接它。

当前默认声道图和 HOA 显式 7.1.4 权重保持原样；未将已有普通多声道默认图问题混入换库。
保持双耳静音峰值 -200 dB、其它路径的 optional 峰值，以及 LFE 的独立峰值合并。
输入非有限值或计量失败会得到错误；离线 True Peak 测量遇到短读不再使用不完整结果调整文件。

实时计量继续在 worker 上，固定格式 seek 原地重置，清空三种 LUFS 快照。
完整积分历史可能扩容；这里没有作无限时长零分配承诺。Histogram 和已知有缺陷的历史截断 API 均未暴露。

## 验证

| 环境 | 结果 |
|---|---|
| macOS arm64 Debug | 53/53 CTests 通过；计量/实时/后处理的最终增量复查 9/9 |
| macOS arm64 Release，参考测试开启 | 54/54 CTests 通过 |
| macOS arm64 Release，最终生产配置 | 53/53 CTests 通过，参考开关已恢复 OFF |
| Windows x64 canonical Release，SOFA ON、参考测试开启 | 53/53 CTests 通过 |
| Windows x64 canonical Release，最终生产配置 | 52/52 CTests 通过，参考开关已恢复 OFF |
| Rust 质量检查 | fmt 与 Clippy `-D warnings` 通过 |
| C++ 质量检查 | changed-file clang-format、clang-tidy、cppcheck 通过；既有复杂度等建议保留 |
| 依赖与许可 | Cargo.lock 覆盖、资源校验、许可证 bundle、manifest 和 SBOM 一致性通过 |
| 公共 ABI | 两端的全部 139 个 `adm_*` 函数保留；无私有 Rust 计量导出，生产库不链接 C 计量实现 |

参考测试覆盖 48 组完整 C++→私有 FFI→Rust 对照：44.1/48/96/192 kHz，1/2/4/5/6/12 声道，
离线和监听模式，12 声道使用显式 HOA 图。门限为 0.001 LU / 0.01 dBTP。
另用 Rust 后处理实际写出 WAV，再由 C 库独立读取，确认归一化到 -23 LUFS（±0.01 LU）
和峰值调整到 -35 dBTP（±0.001 dB）。这些输出验证使用 Release。

Rust 及 C++ 单元测试覆盖独立 -23 LUFS / -20 dBTP 信号、跨采样峰值、400 ms 门限、LFE、
分块、移动所有权、输入校验和失败不污染状态。分配探针确认准备后的短窗口处理和重置无分配。
实际 Monitor 回归确认暂停 seek 后 M/S/I 全部清空，恢复播放后响度回到原水平。

Windows 验证复用原有工作树和构建目录，备份并保留原有修改；Monitor 及 realtime fixture 的既有差异
通过三方合并保留。验收对应工作树内容，不将其描述为干净的本地 Git 提交。
Windows 保留原有自动导出策略；139 个公开 C 入口单独核验，附带 C++ 实现符号不属于稳定 C ABI 承诺。
本轮没有运行 Linux CI，也没有重新执行完整 EBU 官方音频测试集。

## 复现

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-licenses.sh --build-dir build/debug

cmake --preset release -DMR_ADM_BUILD_EBUR128_REFERENCE_TESTS=ON
cmake --build --preset release --target mr_adm_ebur128_reference_tests
build/release/mr_adm_ebur128_reference_tests
cmake --preset release -DMR_ADM_BUILD_EBUR128_REFERENCE_TESTS=OFF
```

评估期的原始数值/性能结果保留在 [候选评估](RUST_EBUR128_EVALUATION.md)，不当作整个渲染器的新性能结果。
最终验收与源文件指纹见 [机器可读记录](evidence/rust-ebur128-migration/validation.json)。
决策见 [ADR 0010](../adr/0010-rust-loudness-meter.md)。
