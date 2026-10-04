# ADR 0010：以统一 Rust Meter 替换 C 计量库

> 状态：已接受、已实现。日期：2026-10-04。

## 决策

SAF 子集迁移后，用户选择优先统一项目计量模块。基于 [适配性评估](../architecture/RUST_EBUR128_EVALUATION.md)，
锁定纯 Rust `ebur128 0.1.10`，通过普通 Rust API 使用，关闭额外 features。
`mradm-dsp::meter::Meter` 提供安全算法接口，`mradm-ffi` 承接有长度检查、panic 保护和明确所有权的私有 C 接口。
C++ 使用 `src/adm_dsp/meter.h` 的可移动 RAII owner，公开 C ABI 按 ADR 0007 保持兼容。

统一迁移 EAR、通用扬声器、双耳、HOA、Apple、Monitor、响度归一化和峰值调整 8 处生产调用。
C `libebur128` 不再进入生产链接，只通过默认 OFF 的 `MR_ADM_BUILD_EBUR128_REFERENCE_TESTS`
构建独立参考测试，不提供运行时回退。

## 行为约束

- 保留原有四类模式、默认声道图及 HOA 显式 7.1.4 图；保留 LFE 排除 LUFS、单独测峰值的策略。
- 静音或不足门控时长仍返回不可用综合响度；不同 renderer 原有静音峰值 optional / -200 dB 规则保留。
- 保留完整历史算法和 worker 归属。默认积分历史会增长，不能在实时音频回调中使用；
  暂不引入有量化近似的 Histogram 模式，也不暴露评估中确认有缺陷的 `set_max_history()`。
- 固定格式下 seek 原地清空计量状态，保留存储与声道图；销毁必须在异步任务和 worker 完成后发生。
- 非法尺寸、模式、查询及非有限 PCM 返回项目错误。拒绝输入时不推进计量状态；
  不将上游用于参数错误的 `NoMem` 直接当作内存失败。
- C++ 异步计量错误经既有 future 传回；Monitor 的计量失败清空快照并记录诊断，下一次重建可恢复。
  跨 FFI 的 Rust panic 被翻译为错误，不穿越 ABI。

## 后果与验证

新增 5 个 Cargo 锁定组件，移除默认生产的 C 计量构建依赖。两个 dasp crate 发布归档缺少许可文本，
已按发布提交补齐上游原文和来源说明，纳入现有许可证/SBOM 校验。

验证包括独立电平及跨采样峰值、FFI 错误边界、重置与分块、实际监听 seek、原有 renderer 回归，
以及完整 C++→FFI→Rust 对照和输出文件的旧 C 库复测。数值与性能证据使用 Release。
不将语言迁移当作跨平台位一致的保证；确定性仍遵循 ADR 0008 的后续路线。

具体平台、测试数量和限制见 [迁移记录](../architecture/RUST_METER_MIGRATION.md)。
