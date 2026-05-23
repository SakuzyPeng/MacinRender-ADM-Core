# ADR 0001：新 C++ 核心采用 C++20

> 状态：已接受  
> 日期：2026-05-16  
> 适用范围：未来 `src/adm_*`、`include/adm/*`、`tools/mradm/*` 等新 C++ 平台化代码。旧 Objective-C/Objective-C++ 路径不受本 ADR 强制约束。

## 背景

项目计划将 CLI（命令行界面）和核心渲染能力向跨平台 C++ 技术栈迁移，并接入 `libadm`、`libbw64`、`libear`、Spatial Audio Framework（空间音频框架，简称 SAF）等生态。新核心需要同时满足：

- 跨平台构建和测试。
- 音频 buffer（缓冲区）处理的安全性和性能。
- 渲染任务取消、进度和未来 GUI（图形用户界面）调用。
- 长期维护时的可读性和可调试性。

## 决策

新 C++ 核心采用 C++20。

推荐 CMake 配置：

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

也就是说，使用标准 `c++20`，不使用 `gnu++20` 扩展模式。

## 写法约束

采用“C++17 心智 + C++20 基础设施”的保守写法：

- 代码应优先清晰、直接、可调试。
- 模板、concepts（概念）、ranges（范围库）只在能明显降低复杂度时使用。
- 不为了展示现代 C++ 语法而增加维护负担。
- 公共 API 应避免暴露复杂模板类型，尤其是跨模块、跨语言绑定边界。

## 推荐使用

- `std::span`：表达非拥有音频 buffer 和数组视图，减少裸指针与长度参数散落。
- `std::filesystem`：跨平台路径处理。
- `std::optional`：表达可选值。
- `std::variant`：表达有限状态或不同 ADM 元素类型。
- `std::string_view`：避免不必要字符串拷贝。
- `std::unique_ptr` / `std::shared_ptr`：表达清晰的对象所有权。
- `std::jthread` / `std::stop_token`：支持渲染任务取消，便于未来 GUI 和服务端调用。
- 少量 concepts：用于约束明确、低噪声的模板工具。

## 谨慎或暂不使用

- modules（模块）：工具链、IDE、CMake 和第三方库组合仍可能增加复杂度。
- coroutines（协程）：渲染主链路暂不需要，调试和跨平台行为成本较高。
- 复杂 ranges 链式写法：可读性收益不足时不使用。
- `std::format`：暂不作为核心依赖，避免不同标准库实现差异。
- C++20 calendar/timezone：当前音频渲染核心不需要。
- C++23-only 标准库能力，例如 `std::expected`。

## 为什么不是 C++17

C++17 更保守，但会失去几个对本项目很实用的基础设施：

- `std::span` 对音频 buffer 接口非常合适。
- `std::jthread` 和 `std::stop_token` 对渲染取消模型很自然。
- 少量 concepts 可以让模板错误更清晰。

由于项目是面向未来的新 C++ 核心，C++20 的收益超过 C++17 的保守收益。

## 为什么不是 C++23

C++23 的 `std::expected` 等能力很有吸引力，但跨平台工具链、标准库实现和 CI（持续集成）一致性仍更容易增加风险。第一阶段不应把包管理、编译器版本和标准库差异变成主要工作。

如果后续 C++23 支持足够稳定，可以通过新的 ADR 再评估升级。

## 错误处理建议

短期不要等待 C++23 `std::expected`。可以二选一：

- 自研轻量 `Result<T>`，适合项目强控制错误模型。
- 引入 `tl::expected` 这类成熟单头库，未来再评估是否替换为标准库。

公共 C ABI（应用二进制接口）不得暴露 C++ 异常或模板错误类型。

## 工具链影响

- `.clang-format` 的新 C++ 部分应匹配 C++20。
- `clang-tidy` 对新 C++ 目录可以逐步启用 `modernize-*`、`performance-*`、`bugprone-*`、`portability-*` 等检查。
- CI 应至少覆盖 macOS 和 Linux 的 C++20 编译。
- 旧 Objective-C/Objective-C++ 代码可以继续使用现有配置，避免一次性工具噪声过大。
