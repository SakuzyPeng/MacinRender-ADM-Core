# ADR 0005：错误处理模型与 ABI 错误码翻译

> 状态：已接受  
> 日期：2026-05-17  
> 适用范围：`adm_core`、`adm_io`、`adm_render*`、`adm_process`、`adm_c_api`、`adm_cli` 中所有可恢复错误路径。OOM（Out Of Memory，内存不足）和编程错误（assertion 失败）走 C++ 异常 / `std::terminate`，不受本 ADR 约束。

## 背景

M2 起会出现大量"可能失败"的函数：

- `adm_io::import_scene(path) -> AdmScene`：文件不存在、`axml` chunk 缺失、`libadm` 解析失败、不支持的 ADM 特性。
- `adm_render_*::render(plan) -> RenderResult`：布局不支持、声道数超限、采样率不匹配。
- `adm_process::measure_loudness(buffer) -> Loudness`：缓冲长度不足、参数越界。

C++ 表达可恢复错误目前三种主流路径：

- **异常 (`throw`)**：标准；但跨 C ABI 边界必须 catch，且公共 API throw 会让 Rust/Swift 绑定方很难处理。
- **自研 `Result<T>`**：完全可控，可绑定项目错误码。需要维护，与 `std::expected` 长期对齐又会带来迁移成本。
- **`tl::expected`**：单头库，API 与 C++23 `std::expected` 近似一致，未来切换近乎零成本。

ADR 0001 第 "错误处理建议" 段已经把候选缩到"自研 `Result<T>`" 与 `tl::expected` 二选一。本 ADR 做最终决定，并明确 C++ 异常的边界、C ABI 错误码翻译规则、错误信息结构。

## 决策

核心错误处理类型采用 **`tl::expected`**（通过 `cmake/MRDependencies.cmake` 以 FetchContent + find_package 兜底接入），并在 `include/adm/errors.h` 中通过 `namespace adm` 提供项目专用 type alias：

```cpp
// include/adm/errors.h
namespace adm {

enum class ErrorCode {
    ok = 0,
    invalid_argument,
    unsupported,
    io_error,
    render_failed,
    cancelled,
    internal_error,   // 防御性，不应出现在 happy path
};

struct Error {
    ErrorCode code{ErrorCode::ok};
    std::string message;            // 人类可读，UTF-8
    std::string context;            // 可选，定位用（文件路径、模块、阶段）
};

template <typename T>
using Result = tl::expected<T, Error>;

inline tl::unexpected<Error> make_error(ErrorCode code, std::string message, std::string context = {}) {
    return tl::unexpected{Error{code, std::move(message), std::move(context)}};
}

}  // namespace adm
```

`tl::expected` 命名空间不在公共 API 中暴露；只暴露 `adm::Result<T>` 别名。后续若切换到 `std::expected`，仅 `include/adm/errors.h` 与 `MRDependencies.cmake` 需要修改。

## C++ 异常边界

- **完全允许**：`adm_core` 与所有 `adm_*` 模块内部使用 `throw`，只要它不跨越公共边界。例如 `std::vector::push_back` 抛出 `std::bad_alloc` 不强制 catch。
- **公共边界必须不抛**：
  - `adm_c_api` 所有导出函数必须 `noexcept`（在 `.cpp` 实现内 catch-all，将异常翻译为错误码）。
  - `adm_core` 的公共 API 函数推荐用 `Result<T>` 返回，不通过异常表达可恢复错误。
- **编程错误走 assertion**：违反前置条件（例如向 `RenderPlan` 传 nullptr `IRenderer`）使用 `assert` 或 `std::terminate`，不进入错误码。
- **OOM 走异常**：内存分配失败按异常处理；公共 API 在边界把 `std::bad_alloc` 翻译为 `ErrorCode::internal_error`，不写诊断逻辑。

## C ABI 错误码翻译

`include/adm/c_api.h` 已定义 `adm_error_code_t` enum（参见现有 c_api.h）。本 ADR 规定：

- `adm::ErrorCode` 与 `adm_error_code_t` 必须 **一对一映射**，且对应数值相同。在 `include/adm/errors.h` 中用 `static_assert` 强制保证：

```cpp
static_assert(static_cast<int>(ErrorCode::ok) == 0, "ABI mismatch: ok");
static_assert(static_cast<int>(ErrorCode::invalid_argument) == 1, "ABI mismatch: invalid_argument");
// ...
```

- C ABI 函数（例如 `adm_render_file`）的实现必须用全 `try { ... } catch (const std::exception&) { ... } catch (...) { ... }` 包裹：

  ```cpp
  adm_error_code_t adm_render_file(...) noexcept {
      try {
          auto result = ctx->service.render(req, progress, logs);
          if (!result) {
              // 写入 result 句柄的 error 字段
              return to_c_error_code(result.error().code);
          }
          return ADM_ERROR_OK;
      } catch (const std::bad_alloc&) {
          return ADM_ERROR_RENDER_FAILED;  // 或者新加 ADM_ERROR_INTERNAL
      } catch (const std::exception& e) {
          // 写入 result 句柄供 adm_render_result_message 读取
          return ADM_ERROR_RENDER_FAILED;
      } catch (...) {
          return ADM_ERROR_RENDER_FAILED;
      }
  }
  ```

- 错误消息通过 `adm_render_result_message()` 访问；callee 拷贝字符串到 result 句柄，调用方在 `adm_destroy_render_result()` 之前可一直读取。

## 错误信息内容

- `Error::message`：人类可读，UTF-8，**中文为主**（与项目日志风格一致）。
- `Error::context`：用于把"在哪里出错"和"为什么出错"分开。例如：
  - `message = "axml chunk 缺失"`, `context = "input=/tmp/foo.wav"`
- 不预先设计本地化框架。如果未来要支持 i18n（国际化），统一通过 `ErrorCode` enum 在调用方做 message 重写，不在 core 内部建多语言表。

## 不做的事

- 不引入 `boost::outcome`、`leaf` 等更复杂的错误处理库。
- 不区分"内部错误"和"用户错误"的多层 enum 体系；`ErrorCode::internal_error` 只用于防御性边界，不细分。
- 不要求 `Result<T>` 的链式 `and_then` / `or_else` 风格写法；允许 `if (!result) return ...;` 早返回。
- 不在 `Error` 内附 stack trace；如需要再单独走日志（spdlog）路径。

## 写法约束

- 公共 API 函数返回值类型必须是 `Result<T>` 或不返回值（`void` / fire-and-forget）。返回 `T` 的"可能失败"函数视为接口设计错误。
- `Result<T>` 不应作为内部辅助函数的传染——内部小函数允许直接 `throw` 或返回 `std::optional<T>`。规模标准：跨模块或跨翻译单元的 API 用 `Result<T>`，单模块内部辅助可自由选择。
- `Error::message` 不应包含敏感信息（绝对路径中的用户名、临时密钥）。如必要，由 `adm_cli` 与 GUI 调用方在展示时再做脱敏。
- `static_assert` 必须覆盖所有 `adm_error_code_t` enum 值，确保 ABI 同步。

## 影响

- `include/adm/errors.h` 需要在 M2 启动前更新结构，从当前最小版本扩展为本 ADR 描述的形式。
- `cmake/MRDependencies.cmake` 需要新增 `tl::expected` 的 find-or-fetch 分支（FetchContent 拉 `TartanLlama/expected` 仓库 `v1.1.0`）。
- `adm_c_api/adm_c_api.cpp` 需要补充 try/catch 包裹与错误码翻译；M1 阶段的 stub 实现允许暂未完整，但 M2 第一个真实导出函数必须遵循。
- `adm_cli/main.cpp` 已经用 `result.success()` 模式；接入 `Result<T>` 后改为 `if (!result) { /* result.error().message */ }`。

## 后果

优点：

- C++23 `std::expected` 标准化后，迁移成本仅"换 include + 改 namespace alias"。
- 公共 API 不抛异常，对 Rust/Swift/Python 绑定友好（与 ADR 0002 Rust-friendly 原则一致）。
- `Error` 结构足够简单，便于在 C ABI 和未来 GUI 中传递。

代价：

- `tl::expected` 是额外的第三方依赖，虽然 header-only 但仍要维护版本。
- 内部异常 + 公共 `Result<T>` 的混合模型需要 contributor 理解何时用哪种，需在 CONTRIBUTING（如有）文档强调。
- 错误码与消息分离意味着错误码必须粗粒度，detail 全部进 `message`；调用方做精细判断时不如细粒度 enum 友好。

## 参考资料

- C++ 标准与错误处理初步讨论：ADR 0001 第 "错误处理建议" 段
- C ABI 现状：`include/adm/c_api.h`
- 边界约束：ADR 0003
- `tl::expected`：https://github.com/TartanLlama/expected
- C++23 `std::expected` 提案：https://en.cppreference.com/w/cpp/utility/expected
