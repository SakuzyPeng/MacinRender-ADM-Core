# 构建以 Cargo 为主（六期 6.6）

> 状态：已完成（2026-11-19，本机 Linux x64）。macOS arm64 / Windows x64 的原生构建、发布与 GUI 打包待维护者复核。
> 依据：[六期一节](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)的 6.6 与用户决定 2026-11-12 第 2 条（以 Cargo 为入口，岛与 libFLAC /
> libopus 由 `-sys` crate 的 `build.rs` 调用只含岛的 CMake 工程）。

6.6 之前，产品由 CMake 构建：
- Corrosion 导入 `mradm-capi` staticlib；
- `mradm_capi` 共享库由 CMake 链接，导出靠版本脚本、`-exported_symbols_list` 或 `.def` 控制；
- `mradm` 是一个只调用 `mradm_cli_main` 的 C++ `main`。

6.6 之后，`cargo build -p mradm-capi` 直接产出 `libmradm_capi`（rustc 链接的 cdylib）与 `mradm`。岛与第三方 C 库由新 crate
`mradm-islands-sys` 的构建脚本经 `cmake` crate 构建，Corrosion 移出构建。

## 用户决定（2026-11-19）

1. **岛按名调用的 36 个 Rust 入口改为函数表。** rustc 的 cdylib 导出依赖图中所有 `#[no_mangle]` 函数，私有模块里的也一样
   （固定工具链 1.98 上实测），版本脚本压不住。若保留按名调用，`libmradm_capi` 会多导出 36 个 `mradm_*`。
2. **顶层 CMake 保留为测试与一致性构建树。** 产品构建、CLI 发布与 macOS GUI 打包直接用 Cargo；C++ 测试、ctest、一致性采集
   （`collect.py` 读 CMakeCache 与构建戳）和 clang-tidy 继续用顶层树，presets 保留。

## A 函数表

- `mradm-ffi` `services::RustServices`（`#[repr(C)]`）的 33 个字段就是原来的 33 个生产入口，去掉了 `mradm_` 前缀：
  - WAV / FLAC 读取、浮点写出端与容器元数据（11 个）；
  - 增益组与计量（10 个）、Scene 数学（5 个）；
  - 布局表、场景视图与销毁、实时覆盖与 LFE 增益（5 个）；
  - Apple 总线计划（2 个）。

  另有一个指向 `RustTestServices` 的指针，装着只供岛测试的 3 个入口：场景导入、DirectSpeakers 矩阵解析与销毁。
  两张表都是 `static`，`mradm-ffi` 不再有任何 `#[no_mangle]` 函数。
- C++ 侧的表定义在 `src/adm_core/rust_services_ffi.h`（测试表在 `tests/support/ffi/island_test_ffi.h`）：
  - `mradm_islands_bind` 把表存进原子指针；
  - `mradm::rust()` 取表，未绑定时打印消息后 abort（`src/adm_core/rust_services.cpp`，静态库 `mr_adm_rust_services`）；
  - 包装（`wav_backend.h`、`gain.h`、`meter.h`、`scene_math.h`、`scene_handle.h` 等）改为 `mradm::rust().<入口>(...)`；
  - `dsp_ffi.h`、`scene_math_ffi.h` 删除，其余私有 FFI 头只留记录。
- 绑定发生在第一次调用岛之前，只执行一次（`Once`）：`mradm-capi` 取岛表的函数（平台、设备、Apple）和每个导出的 panic 兜底
  `guard` 都会绑定。
  - C++ 岛测试直接调用岛的包装，它们链接 C ABI，在 `main` 开头做一次 C ABI 调用（`tests/support/rust_services_test.h`）；
  - 房间设备探针在取岛表前自己绑定。
- `check-ffi-headers.py` 按 `ISLAND_RECORDS` 配对两张表，逐字段核对，函数指针按签名比较；`mradm_islands_bind` 视为 Rust 的导入。
  结果是 0 个函数、58 个结构体。

## B 岛工程、`-sys` crate 与 cdylib

**岛工程 `src/CMakeLists.txt`**：
- 包含 C/C++ 岛（音频、Apple、Windows、平台与设备岛、渲染公共层）、函数表、C ABI 进度回调跳板、房间探针的采集垫片，以及
  libFLAC / libopus / dr_flac / miniaudio（和可选的 IAMF bridge）；
- 岛的库一律为静态库，C ABI 是唯一的共享库；
- 与顶层树共用的选项和编译设置在 `cmake/MRProject.cmake`（PIC 现在总是开启，因为 cdylib 要链接这些归档）；
- 写出 `mradm_native_link-<config>.txt`。清单按链接顺序列出归档（`file:`）、编解码库（`link:`）与平台库
  （`framework:` / `system:`），并记录编解码库在当前配置下的传递链接依赖（`target:`）。
- `mr_adm_native` 以同样顺序把它们交给顶层树的 C++ 测试。

**`rust/crates/mradm-islands-sys`**（`links = "mradm_islands"`），构建脚本有两种工作方式：
- 设置了 `MRADM_NATIVE_LINK_FILE`（顶层树为它的每个 Cargo 命令设置）时，直接读该树的清单，不重复构建；
- 否则经 `cmake` crate 在 `OUT_DIR` 配置并构建 `src/`：
  - 始终是 Release，MSVC 上固定动态 CRT（`MultiThreadedDLL`），与 Rust 一致；
  - 从环境读取 `MR_ADM_FLAC_PROVIDER` / `MR_ADM_OPUS_PROVIDER`；
  - 设置了 `MR_ADM_IAMF_AOM_ROOT` 时开启 IAMF；
  - `MRADM_NATIVE_CMAKE_ARGS` 可再传入以 `;` 分隔的 `NAME=VALUE` 缓存项。

清单里的归档以 `static:-bundle` 链接：不打进 rlib，所以 `mradm-capi` 的 staticlib 不含岛，岛由 C++ 测试自己链接。`mradm-ffi`
依赖这个 crate（岛与 libFLAC 都由它按名调用），并用 `extern crate` 强制链接：一个没有被代码引用的 crate，rustc 不会链接它。
`mradm-ffi` 原来的 FLAC 与岛链接文件处理和 feature `islands` 一并删除，房间探针与其他 `mradm-ffi` 目标的链接方式相同。

**`mradm-capi`**：
- `crate-type = ["cdylib", "staticlib", "rlib"]`，另有 bin `mradm`（`src/main.rs`）；
- C++ 的 `src/adm_cli/main.cpp` 与 `cli_ffi.h` 删除，`mradm_cli_main` 改为 Rust 函数 `cli_main`；
- cdylib 恰好导出 164 个 `adm_*`。版本脚本、导出列表、`.def` 与入口表的生成全部删除，`check-capi-exports.py` 只做检查。

构建脚本设置共享库的名字，与 6.6 之前 CMake 给出的一致：

| 平台 | 共享库名 |
|---|---|
| Linux | SONAME `libmradm_capi.so.1` |
| macOS | install name `@rpath/libmradm_capi.1.dylib`；current version 取 `CMakeLists.txt` 的 `project(VERSION)`，compatibility version 为 `1.0.0` |
| Windows | `mradm_capi.dll`，Cargo 产出的导入库名是 `mradm_capi.dll.lib` |

**顶层树**（`CMakeLists.txt` 与 `cmake/MRRust.cmake`）：
- `add_subdirectory(src)` 构建岛；
- 经 rustup 找到固定工具链，缓存名沿用 `Rust_*`，供 `build_info.py` 读取；
- 目标 `mr_adm_rust` 以 Cargo 构建 `mradm-capi`，把产物复制到构建目录：
  - `libmradm_capi` 带版本号的文件名与符号链接；
  - Windows 的导入库改名为 `mradm_capi.lib`；
  - `rust-static/` 下的 staticlib；
  - 启用 `MR_ADM_CORE_BUILD_CLI` 时构建 `mradm`，macOS 上补完整的 ad-hoc 签名；
- 以 IMPORTED 目标（`mradm_capi`、`mradm_capi_bundle`、`mradm_exe`、`MacinRender::ADMCAPI`）供测试链接与引用；
- Rust staticlib 需要的系统库由 rustc 对空 crate 的 `--print native-static-libs` 得出；
- 每个 Cargo 命令（工具、探针、测试、clippy）都带 `MRADM_NATIVE_LINK_FILE`，并依赖 `mradm_native`；
- `cmake --install` 安装 `c_api.h`、共享库（及其链接、Windows 导入库）与 `mradm`；
- 新的 Cargo 依赖 `cmake` 0.1.58、`cc` 1.11.2、`find-msvc-tools` 0.1.15、`shlex` 2.0.1 只在构建时使用，已登记许可证清单、SBOM 与原文；
  Corrosion 已从清单删除。

## C 工作流与打包

- `scripts/release/build-cargo-release.sh` 是 CLI 发布与 macOS GUI 打包共用的构建：
  - 运行 `cargo build --release --locked -p mradm-capi`，libFLAC / libopus 用 vendored 版本，不用已安装的包；
  - FetchContent 缓存放在 `$FC_CACHE_DIR/cargo`；
  - 检查导出并创建 SONAME 或 install name 对应的版本化符号链接，macOS 上给 `mradm` 补签名。
- `release.yml`：
  - macOS 与 Linux 产物改用这个脚本构建；
  - 顶层树只配置不构建，供 `check-licenses.sh --require-full` 读取全部依赖与 Cargo 元数据；
  - Windows 仍构建顶层树，因为发布前要跑 FLAC / Opus / CLI fixture。被打包的 `mradm.exe` 与 `mradm_capi.dll` 就是 `mr_adm_rust`
    用 Cargo 构建、fixture 实际测过的那一份。
- Windows GUI 打包（`package-windows-gui-release.ps1`）保留 vcpkg 工具链、可选的 ROOM CUDA 插件与 CMakeCache 检查。它继续用顶层树，
  目标改为 `mr_adm_rust`（以及 `mr_adm_room_cuda`）。
- macOS GUI 打包与 `gui/` 的本地脚本：
  - 打包改为 Cargo 构建；
  - `copy-native.sh` 与 `package-macos-gui-dev-app.sh` 的说明改为构建 `mr_adm_rust`。
- CI：
  - 去掉 `MR_ADM_BUILD_CAPI_BUNDLE`；
  - consistency 与 windows-bringup 的构建目标 `mradm_capi_bundle` / `mradm_exe` 改为 `mr_adm_rust`；
  - Linux ARM64 Release job 的 `cargo test --workspace` 另外排除 `mradm-islands-sys`，因为该 job 不构建岛。
- consistency-a preset 删除 `MR_ADM_BUILD_CAPI_BUNDLE` 与已无意义的 `CMAKE_DISABLE_FIND_PACKAGE_fmt`。`consistency_build_test.py`
  仍要求它与 release 的配置一致。

## 发布说明

- `MR_ADM_BUILD_CAPI_BUNDLE` 仍被接受，但已不起作用：共享 `libmradm_capi` 总会构建。
- 直接 `cargo build` 得到的 macOS `mradm` 只带链接器的临时签名，APAC 编码需要先执行 `codesign --force --sign - mradm`。
  顶层树与发布脚本会自动签名。
- Cargo 直接产出的 Windows 导入库是 `mradm_capi.dll.lib`。顶层树、`cmake --install` 与发布包仍提供 `mradm_capi.lib`。
- 岛的库总是静态的，`BUILD_SHARED_LIBS` 不再把它们变成共享库。
- 只使用 `c_api.h` 的消费者不受影响：导出集、SONAME / install name 与文件名都与 6.6 之前相同。

## 验证（本机 Linux x64）

| 项目 | 结果 |
|---|---|
| ctest | A 25/25；B 26/26（`mr_adm_room_96k_tests` 现在总有共享库可用） |
| Rust 质量 | `mr_adm_rust_quality`（fmt 与 clippy）通过；`x86_64-pc-windows-msvc` 与 `aarch64-apple-darwin` 的交叉 clippy 通过 |
| FFI 头校验 | 通过，0 个函数、58 个结构体；`mradm-capi` 私有入口 0 个 |
| 其他检查 | 许可证检查、规模棘轮、`consistency_build_test.py`、`quality_scripts_test.py` 通过 |
| 共享库 | `libmradm_capi.so.1.9.1` 的 SONAME 是 `libmradm_capi.so.1`，恰好导出 164 个 `adm_*`，没有其他动态符号 |

**纯 Cargo 路径**：在全新的 target 目录中运行 `cargo build --release --locked -p mradm-capi`，不经顶层树，经 `cmake` crate 构建岛。
- 产物导出同样的 164 个 `adm_*`；
- 它的 `mradm` 经岛写出 FLAC 与 Opus MKA，FLAC 与顶层树 debug 构建的输出逐字节相同；
- `build-cargo-release.sh` 在本机跑通。

**对照 main `2029e31` 的参照构建**（consistency-a，方法同 R3），A 与 B 各做一次，全部 0 差异：
- C ABI 采集 3,283 行、36 个文件；CLI 记录 113 条命令；
- 消费者测试对两个库都通过；
- 完整 PCM 采集相同：294 个输出、220 个 fixture、989 项比较、176 个重放、256 个产物、666 份日志。

A 的两次采集之间我提交了 A，记录里的提交号与 dirty 标记因此不同，源码指纹相同。比较前把这两个字段对齐。

**故障注入**：
- 测试去掉绑定调用：岛在绑定前运行，进程 abort 并给出消息；
- 交换表的两个字段：头校验报 2 处不一致；
- 在 `mradm-ffi` 加一个 `#[no_mangle]` 函数：导出检查报多出 1 个符号，以 1 退出。

**规模**：
- 生产 C/C++ 8,704 → 8,684 行，岛 3,028 → 3,021 行，测试 C/C++ 11,319 → 11,361 行（岛测试的绑定调用与头）；
- `mradm-ffi` 的导出函数 36 → 0；
- `cmake/MRRustLink.cmake`、`cmake/mradm-capi.map` / `.exports` 删除。

## 待维护者在 macOS / Windows 复核

- 纯 Cargo 构建：
  - macOS 上 `cmake` crate 构建 Apple 岛（框架、`-lc++` / `-lobjc`），`install_name` 与 current / compatibility version，`mradm` 签名后 APAC 编码；
  - Windows 上用 MSVC 构建岛（动态 CRT），检查 `mradm_capi.dll` 的导出与 `mradm_capi.dll.lib`。
- 顶层树：debug / release 构建与 ctest，Windows debug 下 Rust（`msvcrt`）与 C++ 岛（`/MDd`）的混合链接。
- 发布工作流 `release.yml`：三平台 CLI 包与两个 GUI 包的 P/Invoke smoke。
- IAMF bridge 与 ROOM CUDA 插件的可选构建。
