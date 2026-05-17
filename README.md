# MacinRender ADM Core

MacinRender ADM Core 是 MacinRender 的跨平台 ADM（Audio Definition Model，音频定义模型）渲染核心。它从一开始就按 C++20、CMake、C ABI（Application Binary Interface，应用二进制接口）和可替换渲染后端来组织，目标是逐步接入 `libbw64`、`libadm`、`libear` 和 Spatial Audio Framework（空间音频框架，简称 SAF）。

这个仓库不是旧 macOS CLI 的逐行翻译，而是新的平台地基。旧 `MacinRender-ADM-Tool` 仓库仍可作为 macOS 工具、GUI（图形用户界面）、历史实现和 golden fixtures（黄金样本）参考。

## 当前状态

当前仓库处于 M1 skeleton（骨架）阶段：

- C++20 core library：`mr_adm_core`
- 稳定 C ABI 占位层：`mr_adm_c_api`
- CLI11 + fmt + spdlog 驱动的最小 CLI：`adm`
- 最小单元测试：`mr_adm_core_unit_tests`
- 架构规划和 ADR（Architecture Decision Record，架构决策记录）

渲染尚未实现。下一步目标是 ADM BWF probe（探测）和 scene dump（场景摘要导出）。

## 构建

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

运行 CLI：

```bash
./build/adm --version
./build/adm render -i input.wav -o output.wav --renderer auto --output-layout binaural
```

第一次配置时，CMake 会优先查找系统安装的 CLI11、fmt、spdlog；找不到时默认通过 `FetchContent` 拉取固定版本。可通过以下选项关闭自动拉取：

```bash
cmake -S . -B build -DMR_ADM_CORE_FETCH_DEPS=OFF
```

可选启用 Cppcheck：

```bash
cmake --preset quality
cmake --build --preset quality
```

独立运行质量工具：

```bash
scripts/quality/format.sh --check
scripts/quality/clang-tidy.sh build/debug
scripts/quality/cppcheck.sh build/debug
```

## 目录

```text
include/adm/      公共 C++ API 和 C ABI 头文件
src/adm_core/     跨平台核心实现
src/adm_c_api/    C ABI 包装层
src/adm_cli/      CLI11/fmt/spdlog 命令行入口
tests/unit/       最小单元测试
cmake/            CMake 辅助模块
docs/architecture 架构规划
docs/adr/         架构决策记录
```

## 设计原则

- core 不依赖 Apple 平台框架。
- CLI 不直接调用 `libadm`、`libbw64`、`libear` 或 SAF。
- 第三方库类型不进入公共 API。
- C ABI 保持窄接口，为未来 GUI、Rust CLI 和脚本绑定预留。
- C++20 采用保守写法，优先清晰、直接、可调试。
- Rust 作为后续可渐进引入方向，而不是第一阶段主语言。

## 文档

- [C++ ADM 渲染平台化重构规划](docs/architecture/CPP_ADM_PLATFORM_REWRITE.md)
- [ADR 0001：新 C++ 核心采用 C++20](docs/adr/0001-cpp-standard.md)
- [ADR 0002：先建立 C++ 地基，后续渐进引入 Rust](docs/adr/0002-cpp-first-rust-later.md)
- [ADR 0003：自有 ADM 领域模型与后端边界](docs/adr/0003-owned-domain-model-and-backend-boundaries.md)
- [质量工具配置](docs/guides/QUALITY.md)

## 许可证

许可证待定。引入第三方音频库前，需要先记录依赖许可证和链接边界。
