# ADR 0002：先建立 C++ 地基，后续渐进引入 Rust

> 状态：已接受  
> 日期：2026-05-16  
> 适用范围：跨平台 ADM 渲染平台化重构的语言路线、模块边界和未来 Rust 引入策略。

## 背景

项目计划将当前 macOS 优先的 ADM CLI（命令行界面）工具重构为跨平台 ADM 渲染平台。最初曾考虑直接使用 Rust，因为 Rust 在内存安全、错误处理、Cargo 工具链、CLI、测试和长期维护方面有明显吸引力。

但本项目的关键复杂度不在普通 CLI，而在 ADM/BW64/BS.2127/SAF/多声道 DSP（Digital Signal Processing，数字信号处理）这一整套专业音频生态。当前较成熟、能直接服务生产方案的基础库主要集中在 C++：

- `libbw64`：BW64/RF64/WAV 容器和 ADM BWF chunk 读写。
- `libadm`：BS.2076 ADM XML 解析和序列化。
- `libear`：BS.2127 ADM 渲染核心库。
- Spatial Audio Framework（空间音频框架，简称 SAF）：VBAP、HOA、HRTF、扩散、去相关等空间音频能力。

Rust 空间音频生态值得关注，但 ADM/BW64/BS.2127 生产级链路还不如 C++ 完整。若第一阶段直接 Rust 化，项目很可能需要大量 FFI（Foreign Function Interface，外部函数接口）胶水、绑定维护，甚至自研 ADM/BW64/渲染能力，风险和工作量都会明显放大。

## 决策

第一阶段不一步到位 Rust。先使用 C++20 建立跨平台核心地基，完成：

- Apple 平台依赖脱钩。
- ADM/BW64/BS.2127/SAF 生态接入。
- 核心领域模型、IO、渲染后端、后处理、CLI 薄壳和 C ABI 边界。
- golden fixtures（黄金样本）回归测试。

Rust 作为后续渐进引入方向，而不是第一阶段主语言。

## 原则

### 1. C++ 选择基于生态便利

选择 C++ 不是因为 Rust 不适合长期维护，而是因为现阶段项目需要最快、最稳地接入已有 ADM 生产生态。

### 2. C++ 地基必须 Rust-friendly

C++ 核心不能写成未来 Rust 的障碍。需要从第一阶段就保持：

- 公共边界使用 C ABI（Application Binary Interface，应用二进制接口）或稳定窄接口。
- 不在公共 API 暴露 STL 容器、C++ 异常、复杂模板或第三方库类型。
- 领域模型独立于 `libadm`、`libbw64`、`libear` 和 SAF。
- renderer（渲染器）后端插件化。
- IO、渲染、后处理、平台适配分层清晰。
- 使用 golden fixtures 保护行为，便于未来按模块替换实现。

### 3. Rust 按模块进入，不规划全量二次重写

不把路线设计成“先写 C++，以后全量 Rust 重写”。更合理的方式是：

- C++ core 长期作为标准生态桥梁。
- Rust 先进入收益高、风险低、边界清晰的模块。
- 当某个 Rust 模块成熟到可以替换对应 C++ 模块时，再通过 ADR 单独决策。

## Rust 可优先进入的区域

- 新 CLI：Rust 做参数解析、配置加载、输出展示，通过 C ABI 调 C++ core。
- 工具链：fixture 管理、golden 摘要生成、报告生成、批量诊断。
- metadata 诊断：ADM scene dump、差异对比、兼容性检查。
- 小型 DSP 或后处理实验：只在边界清楚、测试充分时进入。
- 脚本绑定：给 Python/Node/Swift 等调用方提供更稳定的包装或工具。

## 暂不建议 Rust 直接承担的区域

- 第一阶段主 ADM BWF/BW64 读写。
- 第一阶段 BS.2127 标准渲染主路径。
- 第一阶段 SAF 替代实现。
- 大规模多声道 DSP 主链路。
- 需要大量 FFI 包装且回归基线尚未稳定的模块。

## 迁移路线

```text
第一阶段：
  C++20 core
  libbw64 + libadm + libear + SAF
  CMake
  C ABI
  golden tests
  Apple 脱钩

第二阶段：
  评估 Rust 进入点
  优先 CLI / 工具 / 测试生成 / metadata dump / 报告

第三阶段：
  若 Rust 模块成熟，再按模块替换或并行实现
```

## 影响

- `docs/architecture/CPP_ADM_PLATFORM_REWRITE.md` 中的“C++ ADM 渲染平台化”仍成立，但其含义是“以 C++ 接入成熟生态并建立平台地基”，不是排斥 Rust。
- `adm_c_api` 的重要性提高，因为它既服务未来 GUI（图形用户界面），也服务未来 Rust CLI 或 Rust 模块。
- 测试基线和领域模型边界比具体语言更重要。只要边界干净，Rust 后续进入不会变成又一次推倒重来。

## 风险

### C++ 地基过度复杂

如果 C++ 内部过度模板化、过度抽象或泄露 STL/第三方类型到公共 API，未来 Rust 集成成本会升高。

应对：

- C++20 保持保守写法。
- 公共接口窄化。
- 复杂类型只留在模块内部。

### Rust 进入过早

如果在 IO/渲染主链路未稳定前引入 Rust，可能同时维护 C++、Rust、FFI 和音频回归差异，复杂度会上升。

应对：

- Rust 首批只进入边界清楚的工具和外围模块。
- 每个 Rust 试点都应有明确收益和退出条件。

### C++ 长期固化

如果只完成 C++ 地基但没有保留 Rust-friendly 边界，后续 Rust 只会停留在外围脚本。

应对：

- 从第一阶段就要求 C ABI、模块化后端和 golden fixtures。
- 后续新增公共 API 时评估跨语言绑定影响。

## 参考资料

- `libear` 文档：https://libear.readthedocs.io/
- EBU 关于 `libear`、`libbw64`、`libadm` 的说明：https://tech.ebu.ch/news/2019/09/easy-nga-implementations-with-c-library-from-bbc-and-irt
- Rust `vbap` crate：https://docs.rs/vbap/latest/vbap/
- Rust `oximedia-spatial` crate：https://docs.rs/oximedia-spatial/latest/oximedia_spatial/
