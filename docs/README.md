# MacinRender ADM Core 文档

## 架构文档

- [C++ ADM 渲染平台化重构规划](architecture/CPP_ADM_PLATFORM_REWRITE.md)
- [ADM 特性覆盖审计](architecture/ADM_FEATURE_COVERAGE.md)

## 架构决策记录

- [ADR 0001：新 C++ 核心采用 C++20](adr/0001-cpp-standard.md)
- [ADR 0002：先建立 C++ 地基，后续渐进引入 Rust](adr/0002-cpp-first-rust-later.md)
- [ADR 0003：自有 ADM 领域模型与后端边界](adr/0003-owned-domain-model-and-backend-boundaries.md)
- [ADR 0004：第三方依赖管理策略](adr/0004-third-party-dependency-management.md)
- [ADR 0005：错误处理模型与 ABI 错误码翻译](adr/0005-error-handling-model.md)
- [ADR 0006：CLI 参数解析库采用 CLI11](adr/0006-cli-argument-library.md)
- [ADR 0007：C ABI 稳定性承诺与版本策略](adr/0007-c-abi-stability-policy.md)

## 使用指南

- [CI 设计草案](guides/CI.md)
- [质量工具配置](guides/QUALITY.md)
- [第三方许可证与发行边界](THIRD_PARTY_LICENSES.md)

## 维护说明

- 架构规划放在 `architecture/`。
- 已接受或废弃的重要技术决策放在 `adr/`。
- 使用指南放在 `guides/`。
- 新增依赖、语言路线、公共 API 和后端边界变化都应补充 ADR。
