# ADR 0006：CLI 参数解析库采用 CLI11

> 状态：已接受  
> 日期：2026-05-17  
> 适用范围：`src/adm_cli/`、未来基于核心库的命令行工具（例如 `tools/mradm/*`）。GUI（图形用户界面）、Rust 工具链、C ABI（应用二进制接口）调用方不受本 ADR 约束。

## 背景

M1 skeleton 阶段需要为新 CLI（命令行界面）选择参数解析库。当时为了让 skeleton 能落地，直接选用了 CLI11，并在 `src/adm_cli/main.cpp` 实现了最小可用版本。这一选择尚未被 ADR 正式锁定，存在两个风险：

- 后续 contributor 出于个人偏好替换为 cxxopts、argparse 或自研 parser，导致 CLI 维护成本周期性反复。
- M5（新 CLI 替代旧 CLI）阶段移植旧 Objective-C CLI 选项时，可能因为参数库不够强大而被迫切换，但切换时已积累的子命令、帮助文案、补全脚本会被破坏。

本 ADR 把"CLI11"这条既成事实正式记录，并说明候选评估和长期约束，使未来变更需要走 ADR 流程而非个人偏好。

## 决策

新 CLI 的参数解析库采用 **CLI11**，版本通过 `cmake/MRDependencies.cmake` 中的 `FetchContent` 锁定（当前为 `v2.5.0`），与 `fmt`/`spdlog` 共用 find-or-fetch 策略。

CLI11 在以下方面满足项目需求：

- header-only 单库，不引入 boost、Qt 等重依赖。
- 子命令（subcommand）支持完整，能表达 `mradm render`、`mradm probe`、`mradm batch`、`mradm diagnose` 这类多动词 CLI 形态。
- 类型安全的 option binding，避免手写字符串转 enum 的常规错误（参见 `parse_renderer()` 的当前简化用法和 `CLI::IsMember` 校验）。
- 活跃维护，最近一次 release 距今较新。
- 与 macOS/Linux/Windows 三平台编译器组合良好。

## 候选评估

| 库 | 子命令 | header-only | 类型安全 | 维护活跃度 | 主要劣势 |
|---|---|---|---|---|---|
| CLI11 | 强 | 是 | 高 | 高 | 编译期模板较重，header 较大 |
| cxxopts | 弱（手写） | 是 | 中 | 中 | 子命令需自行组合 |
| argparse | 中 | 是 | 高 | 中 | API 风格偏 Python，子命令支持晚 |
| Boost.Program_options | 强 | 否 | 中 | 高 | 引入 Boost 依赖，与 ADR 0001 保守风格冲突 |
| 自研极简 parser | 弱 | — | 低 | — | 重新发明轮子，子命令/校验/帮助生成全部要自写 |

CLI11 在子命令完整度和维护活跃度上最优；它的劣势（编译期模板较重）在本项目 CLI 规模下不构成问题——CLI 是薄壳，文件量小，增量编译可控。

cxxopts 和自研方案在未来要支持 `batch`、`probe`、`diagnose` 这类多子命令时会显著恶化。argparse 是较接近的备选，但当 CLI11 已有现成 skeleton 时切换的收益不足以抵销迁移成本。

## 写法约束

- `CLI11` 类型仅出现在 `src/adm_cli/`。`adm_core` 与 `adm_c_api` 公共头禁止 `#include <CLI/CLI.hpp>`，符合 ADR 0003 的边界要求。
- CLI 不在 `main` 之外暴露 `CLI::App` 引用；解析结束后立即构造 `adm::RenderRequest` 等核心类型并丢弃 CLI 上下文。
- 选项校验优先用 CLI11 内置 `check()`（例如 `CLI::IsMember`、`CLI::ExistingFile`），避免手写校验分散在各处。
- 帮助文案与错误消息一律以中文为主（与项目其余日志风格一致），CLI11 自身的 fallback 英文消息允许保留。
- 不使用 CLI11 的 `Config` / `--config-file` 自动加载机制；如需配置文件，在 CLI 层手动读后再灌入 option。

## 旧 CLI 兼容性

旧 Objective-C CLI 的选项风格（例如 `--vbap-spread-mode`、`--vbap-diffuse-mode`、`--output-layout`、`--peak-limit`、`--loudness-normalize` 等）会在 M5 阶段统一评估保留或借机改名。本 ADR **不预先承诺** 任何选项名兼容，原因有二：

- 老 CLI 部分选项命名本身带有命名混乱（参见旧仓库 `--vbap-spread-mode legacy` 已重命名为 `basic` 的经验），不应被锁死。
- 真正的兼容评估需要 M3 端到端渲染闭环跑通后，才能判断哪些选项已经过时、哪些需要保留并提供 alias。

M5 阶段将单独出 ADR 决定选项命名映射表。

## 限制与已知不足

- **GNU-style 选项粘连**：CLI11 不支持 `-vio file.wav` 这种 GETOPT 风格粘连；必须写 `-v -i io -o file.wav` 或 `--input=file.wav`。这与 Apple 命令行工具习惯接近，可接受。
- **自定义 formatter**：帮助文案的高度自定义需要继承 `CLI::Formatter`；保持默认格式即可，不投入定制。
- **shell completion**：CLI11 不自带 completion 生成；若 M5 后用户要求 zsh/bash completion，再考虑手写或迁移工具。

## 影响

- `cmake/MRDependencies.cmake` 中已注册 CLI11 find-or-fetch，无需追加修改。
- 文档应在 `docs/README.md` 的 ADR 索引追加本条。
- 后续 contributor 若提议替换参数库，需先发起新 ADR 走流程，本 ADR 才能被废弃。

## 后果

优点：

- CLI 选型不再悬而未决，contributor 不会因为个人偏好反复切换。
- header-only + FetchContent 让 CI 与本地构建一致，无需额外安装步骤。

代价：

- CLI11 的编译开销不算最小，但本项目 CLI 体量小，可忽略。
- 未来若 CLI11 维护停滞，需要走 ADR 流程评估迁移（argparse 是首选备胎）。

## 参考资料

- CLI11 项目：https://github.com/CLIUtils/CLI11
- 现行使用代码：`src/adm_cli/main.cpp`
- 依赖管理（FetchContent + find_package 兜底）：`cmake/MRDependencies.cmake`、ADR 0004
- 边界约束：ADR 0003 第 "禁止事项" 段
