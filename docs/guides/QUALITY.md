# 质量工具配置

本仓库从初始化阶段就配置 `clang-format`、`clang-tidy` 和 `cppcheck`。三者分工如下：

- `clang-format`：统一 C/C++ 代码格式，提交和 CI 中应强制检查。
- `clang-tidy`：语义级静态分析，主要覆盖 bugprone、modernize、performance、portability、readability 等检查。
- `cppcheck`：补充检查可移植性、未初始化、空指针、越界和 C 风格问题。

## 安装

macOS 可使用：

```bash
brew install llvm cppcheck
```

如果 Homebrew 的 LLVM 不在 `PATH` 中，需要把对应路径加入 shell 配置，例如：

```bash
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
```

## 常用命令

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

scripts/quality/format.sh --check
scripts/quality/clang-tidy.sh build/debug
scripts/quality/cppcheck.sh build/debug
```

本地日常开发建议优先跑增量检查，只扫描相对 `origin/main`、暂存区和工作区发生变化的 C/C++ 文件：

```bash
scripts/quality/check-changed.sh --build-dir build/debug
```

一键检查：

```bash
scripts/quality/check-all.sh build/debug
```

也可以通过 CMake 的 quality preset 在编译时挂接 clang-tidy 和 cppcheck：

```bash
cmake --preset quality
cmake --build --preset quality
ctest --preset quality
```

## 策略

- 新 C++ 代码默认遵循 `.clang-format`。
- `.clang-tidy` 先保持 warning-only，不在第一阶段把所有 warning 设为 error。
- `cppcheck` 的 suppression 应保持很小，优先修代码或缩小扫描范围。
- 第三方代码、构建产物和 vendored 依赖不纳入默认扫描。
