# ADR 0013：Rust EAR 算法与生产依赖移除

> 状态：已接受。日期：2026-10-06。

本决议更新 ADR 0008 暂不迁移 libear 的阶段边界。目标为移除生产原生依赖，保留已有
EAR 声像、能量、路由及时序语义；接受严格受控的浮点误差，跨平台逐位一致留给二期。

- 新增普通 Rust 库 `mradm-ear`，禁止 unsafe，通过已有 `mradm-ffi` staticlib 接入。
  原始指针、UTF-8、长度、别名和错误翻译只位于私有 FFI；panic 不穿越 C ABI。
- 移植项目实际使用的布局/声像拓扑、Objects extent、DirectSpeakers、HOA AllRAD 与
  512-tap FIR 设计。保留 nominal/effective 坐标区别，未以已有 VBAP 或 HOA 编码器替代。
- C++ 继续持有场景与调度、channelLock/divergence 等语义预处理及 22.2 LFE 策略。
  Rust 生成 f64 增益和 f32 FIR，交给既有 Rust 混音、插值、卷积及 255 样本延迟。
- CLI `ear`、兼容 backend 名 `libear`、公开 C ABI 不变。实现版本为 `rust-ear-0.1.0`。
  不新增未支持的 ADM 能力；HOA screenRef/nfcRefDist 继续警告并忽略。
- 默认构建及默认测试不获取或链接 libear。旧库固定于 `2db69f8f`，仅在
  `MR_ADM_BUILD_LIBEAR_REFERENCE_TESTS=ON` 使用，禁止运行时回退。
- 保留上游 Apache-2.0 版权与数据来源；发行清单区分移植代码和可选历史依赖。

算法对照使用事先固定的门限：普通 f64 增益/矩阵 `1e-9*(1+abs(ref))`，extent
`1e-5*(1+abs(ref))`，FIR `2e-7*(1+abs(ref))`；音频 fixture 门限
`2e-5*(1+abs(ref))`。精确路由、LFE 零槽、独立实例和时间行为另作严格检查。
旧库比较不替代独立语义回归，也不构成完整 BS.2127 认证。

性能按准备与持续 DSP 分开测量，使用 Release。数值、平台验证及限制见
[迁移记录](../architecture/RUST_EAR_MIGRATION.md)。
