# CI 设计

本文档记录 MacinRender ADM Core 的 GitHub Actions CI 设计与当前落地状态。目标是先建立稳定的
跨平台构建与测试基线，再逐步完善发布打包、许可证 bundle 和更重的静态分析。

## 目标

- 每个 pull request 都能验证 CMake 配置、编译、单元 / fixture 测试和 CLI smoke。
- macOS 路径必须覆盖 APAC、CAF layout、AudioToolbox 和 Accelerate 相关代码。
- Linux 路径用于验证跨平台构建和非 Apple 输出格式，避免 Apple framework 偶然泄漏到公共路径。
- 质量检查与本地脚本一致，优先复用 `scripts/quality/*`。
- CI 不依赖私有音频素材；只使用测试代码运行时生成的小 fixture。

## 阶段划分

### 第一阶段：必需 CI

当前已落地在 `.github/workflows/ci.yml`。PR、push 到 `main` 和手动触发都会运行。

| Job | Runner | 内容 | 说明 |
|---|---|---|---|
| `macos-debug` | `macos-15` 或当前最新 macOS arm64 | `cmake --preset debug`、`cmake --build --preset debug`、`ctest --preset debug` | 主验证路径；覆盖 APAC smoke 和 CoreAudio layout |
| `linux-debug` | `ubuntu-24.04` | `cmake --preset debug`、`cmake --build --preset debug`、`ctest --preset debug` | APAC 测试会自动 skip；验证跨平台核心 |

这两个 job 均显式使用 `MR_ADM_FLAC_PROVIDER=VENDORED` 和 `MR_ADM_OPUS_PROVIDER=VENDORED`
作为 CI 默认值，减少系统包差异。系统仍需安装 CMake、Ninja、Boost headers、ccache 和平台编译工具；
`libbw64/libadm/libear/SAF/FLAC/Opus` 由 FetchContent 或 vendored provider 处理。

### 第二阶段：质量 CI

当前已落地在 `.github/workflows/quality.yml`。质量 job 初期只跑在 macOS，因为
clang-tidy 脚本已经包含 macOS SDK 参数处理，且项目当前主要开发环境是 macOS。

| Job | Runner | 触发 | 内容 |
|---|---|---|---|
| `quality` | `macos-15` | pull request | `cmake --preset debug`、`scripts/quality/check-changed.sh --base origin/main --build-dir build/debug` |
| `quality` | `macos-15` | push 到 `main` / 手动 full | `cmake --preset debug`、`scripts/quality/check-all.sh build/debug` |
| `quality` | `macos-15` | 手动 changed | `check-changed.sh` |

`check-all.sh` 会全量扫描 `include/`、`src/`、`tests/`。如果后续耗时过长，可以继续保留
PR changed / main full 的分层策略。

### 第三阶段：发布构建

当前已落地在 `.github/workflows/release.yml`。发布构建用于验证 vendored static provider 和优化配置，
不在 PR 或普通 push 中运行。

| Job | 触发 | 内容 |
|---|---|---|
| `release-macos` | tag `v*`、手动触发 | 构建 `mradm_exe`、打包并上传 `mradm-<version>-macos-arm64.tar.gz` 与 `.sha256` |
| `release-linux` | tag `v*`、手动触发 | 构建 `mradm_exe`、打包并上传 `mradm-<version>-linux-x86_64.tar.gz` 与 `.sha256` |

release workflow 使用 `scripts/release/package.sh` 生成首版可审计发行包。包内包含 `bin/mradm`、
`LICENSE`、`THIRD_PARTY_NOTICES.md`、`BUILD_INFO.txt` 和 `DEPENDENCIES.txt`。macOS 包会拒绝
`/opt/homebrew` 与 `/usr/local` 动态库，并只允许 Apple 系统库/framework；Linux 包记录 `ldd` 清单，
并拒绝缺失库、构建目录依赖和 `/usr/local` 依赖。两个平台都会在上传前通过
`scripts/release/smoke-package.sh` 解包、校验 checksum，并运行 `mradm --version` / `mradm backends`。
Linux 支持基线为 Ubuntu 24.04 x86_64。签名、notarization、GitHub Release 自动创建和完整 license
bundle 留到后续阶段。

## 缓存策略

CI 使用两层缓存，不缓存 CMake build tree。

| 缓存 | 路径 | 用途 | key 依据 |
|---|---|---|---|
| FetchContent | `.fc-cache` | 缓存第三方源码 checkout，降低网络波动 | OS + `cmake/MRDependencies.cmake` + `CMakeLists.txt` + `CMakePresets.json` |
| ccache | `.ccache` | 缓存编译产物 | OS + job 类型 + commit SHA，带 OS/job restore key |

所有 job 都 fresh configure。这样即使 CMake cache 或 FetchContent 状态变化，也不会复用旧 build tree。

## 依赖安装建议

### macOS

```bash
brew install cmake ninja boost llvm cppcheck ccache
```

CI 中应显式把 Homebrew LLVM 放入 `PATH`：

```bash
echo "$(brew --prefix llvm)/bin" >> "$GITHUB_PATH"
```

### Linux

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build build-essential git pkg-config ccache libboost-all-dev libopenblas-dev liblapacke-dev
```

如果 Linux job 后续启用质量检查，再安装：

```bash
sudo apt-get install -y clang-format clang-tidy cppcheck
```

## 需要注意的边界

- APAC 编码只在 macOS 可用；Linux / Windows 上 `mr_adm_apac_smoke_tests` 会跳过。
- SAF 使用 Apple Accelerate 的路径只在 macOS 存在；Linux 走 SAF 的通用路径。
- SOFA reader 默认开启，但 NetCDF 关闭；CI 不需要下载外部 SOFA 数据集。
- Release preset 默认会让 FLAC / Opus 的 `AUTO` provider 走 vendored static；Debug 在本机可能优先系统库。CI 建议显式指定 provider，减少 runner 差异。
- `mradm` 是唯一正式 CLI 二进制名；CI 不应检查或生成 `adm` 兼容入口。

## 当前 workflow 摘要

```yaml
.github/workflows/ci.yml
  pull_request / push main / workflow_dispatch
  macOS debug + Linux debug

.github/workflows/quality.yml
  pull_request / push main / workflow_dispatch
  PR: changed quality
  main/manual full: full quality

.github/workflows/release.yml
  tag v* / workflow_dispatch
  macOS release + Linux release tarballs/checksums
```

## 后续实施顺序

1. 观察第一轮 GitHub runner 上 FetchContent、SAF、vendored FLAC/Opus 是否稳定。
2. 如果 quality 太慢，保留 PR changed，必要时把 main full 改成夜间 schedule。
3. release job 后续补 macOS 签名/notarization、GitHub Release 自动创建和完整第三方 license bundle。
4. Windows 暂用 `.github/workflows/windows-bringup.yml` 手动验证 MSVC + Ninja 的 CLI 构建边界。libadm
   需要的 Boost 组件通过 vcpkg 安装，并使用本地文件型 vcpkg binary cache；OpenBLAS/LAPACKE 使用
   OpenBLAS 官方 Windows x64 预编译包并缓存解压目录，避免每次从源码编译 OpenBLAS。该 workflow 先关闭测试，
   以 `MinSizeRel` 构建 `mradm_exe` 并运行 `mradm --version` / `mradm backends`；绿灯后再逐步打开 smoke tests、
   运行实际渲染 fixture，并评估是否加入默认 CI 矩阵。当前使用 `MinSizeRel` 是为了绕开 MSVC 14.44 在 SAF
   `saf_utility_filters.c` 上的 `/O2` 内部编译器错误；正式发行前需要重新评估是否改为单文件降优化或 pin
   工具链版本。
