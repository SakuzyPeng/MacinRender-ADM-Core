# ADR 0004：第三方依赖管理策略

> 状态：已接受  
> 日期：2026-05-17  
> 适用范围：`CMakeLists.txt`、`cmake/MRDependencies.cmake`、所有 `adm_*` 模块对外部 C/C++ 库的接入路径。CI（持续集成）配置、`docs/SAF_LINKING_AND_LICENSE.md` 类许可证清单与本 ADR 配合。

## 背景

M1 skeleton 阶段已用 `FetchContent` 接入 `fmt`、`spdlog`、`CLI11` 三个 utility 类依赖（参见 `cmake/MRDependencies.cmake`）。但项目核心功能依赖的四个 ADM/DSP（Digital Signal Processing，数字信号处理）库——`libbw64`、`libadm`、`libear`、Spatial Audio Framework（空间音频框架，简称 SAF）——尚未进入构建。M2（接入 `libbw64` 与 `libadm`）的第一步就需要明确这四个库的入栈方式，否则会出现"先随手 FetchContent 拉，后面又改 vendored"的反复，CMake 与 CI 双重返工。

四个候选策略：

- **`FetchContent`**：CMake 内置，按 git tag 拉取并集成。优点：版本由仓库锁定，无 submodule 状态分叉，CI 缓存自然。缺点：首次 configure 慢，依赖 git 与网络。
- **vendored submodule**：仓库内 `vendor/` 目录显式锁版本。优点：离线可构建，可自由 patch 上游。缺点：子模块状态管理成本高、首次 clone 体积大、上游 update 流程繁琐。
- **vcpkg / Conan**：包管理器统一接管。优点：跨平台一致，二进制缓存。缺点：要求开发者额外安装工具链，并维护 manifest 文件，对小型项目不划算。
- **系统 `find_package`**：依赖 Homebrew、apt、vcpkg 系统安装层。优点：最轻。缺点：版本碎片化，Linux 发行版与 macOS 版本可能不一致，CI 必须额外安装步骤。

本 ADR 明确主策略与例外，使 M2 实施可以直接套用而无需现场决策。

## 决策

主策略：**`FetchContent` 拉取 + `find_package(CONFIG)` 优先兜底**，沿用 `cmake/MRDependencies.cmake` 中 `mr_adm_core_find_or_fetch()` 已有的复用模式。

具体路径：

| 依赖 | 接入方式 | 版本锁定位置 | 备注 |
|---|---|---|---|
| `fmt` | FetchContent + find_package | `MRDependencies.cmake` | M1 已就位 |
| `spdlog` | FetchContent + find_package | `MRDependencies.cmake` | M1 已就位 |
| `CLI11` | FetchContent + find_package | `MRDependencies.cmake` | M1 已就位 |
| `libbw64` | FetchContent + find_package | `MRDependencies.cmake` | M2 接入 |
| `libadm` | FetchContent + find_package | `MRDependencies.cmake` | M2 接入 |
| `libear` | FetchContent + find_package | `MRDependencies.cmake` | M3 接入 |
| Spatial Audio Framework | vendored submodule（保留）| `vendor/saf/` 或 git submodule | M4 接入，原因见下 |
| `libebur128` | find_package 优先，FetchContent 兜底 | `MRDependencies.cmake` | M3/M4 视进度接入 |

## 例外：SAF 走 vendored submodule

SAF 与其他 EBU 库的关键区别：

- **许可证模块化复杂**。SAF core 是 permissive license（宽松许可证），但部分 optional module（例如 `SAF_USE_INTEL_IPP_FFT`、`SAF_USE_NETCDF`、特定 HRIR 数据集）带有 GPL/LGPL 或专利许可，必须在构建配置层面严格隔离。一旦走 FetchContent 直接拉上游主干，开发者可能默认开启某个 optional module 而无察觉，破坏许可证清单。
- **配置项极多**。SAF 的 CMake 选项（`SAF_PERFORMANCE_LIB`、`SAF_USE_*` 系列、各种 HRIR 包）需要根据本项目实际用途定制，FetchContent 模式不利于固定这套配置。
- **本项目已有 SAF 翻译运行时经验**。旧仓库 `vendor/saf_spreader_runtime/` 已经是 vendored 形式，并通过 `scripts/vbap/build_saf_spreader_bridge.py` 单独构建静态库。新仓库可沿用相似策略，只是改为标准 CMake submodule。

因此 SAF 在新仓库统一以 git submodule（或 vendored snapshot）形式存在，配置项在项目 CMake 中显式声明，避免上游配置漂移。

## 例外：`libear` 的构建脆弱性

`libear` 历史上依赖 `Boost`（数学算法）、`Eigen3`（线性代数）、`xsimd`（SIMD 抽象）、`adm`（自家 ADM 库）。FetchContent 拉它时会顺带把这些依赖一起带入。需要：

- 在 `MRDependencies.cmake` 中显式 `set(EAR_UNIT_TESTS OFF CACHE BOOL "" FORCE)` 等关闭 `libear` 的测试目标，避免污染我们自己的 ctest tree。
- 若 `libear` 上游的 `find_package(Boost)` 失败，提供清晰的错误提示，引导开发者通过系统包安装 boost-headers。
- 不强制将 `libear` 的内部依赖暴露给 `adm_render_ear` 之外的目标。

ADR 0003 已规定 `libear` 类型只能出现在 `adm_render_ear` 内部，本 ADR 在依赖管理层强化这一边界。

## 写法约束

- `MRDependencies.cmake` 的 `mr_adm_core_find_or_fetch(package_name target_name)` 函数继续作为统一入口；新加依赖应在该函数内添加 `elseif (package_name STREQUAL "<新依赖>")` 分支，而不是在 `CMakeLists.txt` 散落 `FetchContent_Declare`。
- 所有 git tag 必须是 release tag（语义化版本）或 commit hash，不允许追 branch（`master`/`main`）。
- 上游配置变更应封装为本仓库 `cmake/` 下的 `MRDeps<Name>.cmake`（仅在配置项超过 3 个时拆文件），保持 `MRDependencies.cmake` 可读。
- `MR_ADM_CORE_FETCH_DEPS=OFF` 模式必须仍可构建（前提是系统已 `find_package` 全部依赖），用于 Linux 发行版打包、CI 离线场景与 Homebrew formula。
- libFLAC 使用三档 provider：`MR_ADM_FLAC_PROVIDER=AUTO`（默认，按首次配置的构建类型选择：开发构建优先系统库，Release/多配置默认 vendored static）、`VENDORED`（正式分发，FetchContent + 静态链接）、`SYSTEM`/`MR_ADM_USE_SYSTEM_FLAC=ON`（包管理器或发行版打包）。
- libopus 使用同样的三档 provider：`MR_ADM_OPUS_PROVIDER=AUTO`（默认，按首次配置的构建类型选择：开发构建优先系统库，Release/多配置默认 vendored static）、`VENDORED`（正式分发，FetchContent + 静态链接）、`SYSTEM`/`MR_ADM_USE_SYSTEM_OPUS=ON`（包管理器或发行版打包）。
- 任何依赖的版本升级（修改 tag）需附带 CMake configure + build + ctest 验证，并在 commit message 注明升级原因（安全补丁/特性需求/上游 deprecation）。

## 三平台策略

- **macOS**：FetchContent + find_package 主路径；开发者本地可用 Homebrew 装 `boost`、`eigen` 缩短首次 configure 时间。
- **Linux**：同上；发行版打包者可走 `MR_ADM_CORE_FETCH_DEPS=OFF` 全 system 路径。
- **Windows**：同上；vcpkg 用户可手动 `find_package` 兜底（vcpkg 的 `CMAKE_TOOLCHAIN_FILE` 与 find_package CONFIG 模式兼容）。

不为单一平台分叉策略；统一 FetchContent 主路径保证三平台开发体验一致。

## 风险

- **首次 configure 时间**：四个 ADM 库 + 已有三个 utility 库 + libear 间接依赖（Boost/Eigen）首次拉取较慢。缓解：CI 配置 `FETCHCONTENT_BASE_DIR` 缓存 + `GIT_SHALLOW TRUE`（M1 已使用）。
- **离线场景**：开发者无网络时 first configure 失败。缓解：文档明确写明 `MR_ADM_CORE_FETCH_DEPS=OFF` + system find_package 路径；CI 至少一条 job 走该路径以防回归。
- **SAF submodule 状态分叉**：开发者 forget 跑 `git submodule update --init --recursive`。缓解：`CMakeLists.txt` 启动时检测 `vendor/saf/.git` 不存在则给明确报错，附带补救命令。
- **`libear` 上游变更频繁**：BBC 与 EBU 偶尔会破坏 CMake 公共接口。缓解：版本锁 release tag，不追 master；升级前手动跑 ctest。

## 后果

优点：

- 与 M1 已落地的 fmt/spdlog/CLI11 模式一致，无认知割裂。
- 不引入 vcpkg/Conan 等额外工具链要求，对个人开发者友好。
- SAF 单独走 vendored 给许可证审计留出操控空间。

代价：

- 首次 configure 比 vcpkg 二进制缓存慢，但 CI 缓存可以摊销。
- 需要维护一份"哪个库走哪条路径"的清单（即本 ADR 的依赖表）。
- 升级依赖需要人工跑 ctest 而非 dependabot 自动 PR。

## 参考资料

- 现行依赖封装：`cmake/MRDependencies.cmake`
- 边界约束：ADR 0003 第 "禁止事项" 段
- C++ 标准：ADR 0001
- SAF 链接与许可证策略（旧仓库经验，可作为新仓库 `docs/SAF_LINKING_AND_LICENSE.md` 的迁移基线）
- libear 文档：https://libear.readthedocs.io/
- libadm 项目：https://github.com/ebu/libadm
- libbw64 项目：https://github.com/ebu/libbw64
- Spatial_Audio_Framework：https://github.com/leomccormack/Spatial_Audio_Framework
