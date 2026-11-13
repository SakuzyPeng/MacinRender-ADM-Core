# C ABI 由 Rust 实现（六期 6.3）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)六期 6.3，家族清单与对照工具见 [6.1 记录](RUST_BOUNDARY_BASELINE.md)，
> 岛的窄 C 接口见 [6.2 记录](RUST_BOUNDARY_ISLANDS.md)。6.3 分 A（骨架）与 B–F（按家族迁移：版本、上下文与内存 7；
> render / inspect / room / 能力表 80；preview 4；Scene 38；Monitor / OSC 35）。

## 方向与用户决定

原计划由 rustc 产出 cdylib `mradm_capi`，A 批先把现有 C++ C ABI 整体链接进去。Linux 实测：cdylib 会导出依赖 crate 中所有
`#[no_mangle]` 函数，另加的 `--version-script` 也压不住。6.5 删除外壳之前，C++ 外壳与岛还要调用 `mradm-ffi` 的约 412 个 `mradm_*`
入口，所以这期间 cdylib 做不到只导出 164 个 `adm_*`。此外，要让 cdylib 导出仍由 C++ 实现的函数，还得写 164 个 Rust 转发桩并给 C++
定义改名。

用户决定（2026-11-13）：**6.3 不用 cdylib，6.6 再换。**
- 6.3 期间 `mradm_capi` 共享库照旧由 CMake 链接，沿用现有导出控制（ELF 版本脚本、Mach-O `-exported_symbols_list`、Windows `.def`），
  导出面、SONAME（`libmradm_capi.so.1`）、install_name、`.def` 与 GUI 打包都不变；
- Rust 实现的 `adm_*` 放在新 crate `mradm-capi`，它取代 `mradm-ffi` 成为 CMake 链接的唯一 Rust staticlib；
- 6.6 外壳已删、构建改以 Cargo 为主时，`mradm-capi` 改为 cdylib，那时核对 SONAME / install_name / `.def` 与 GUI 打包；
- A 批因此改为骨架，不迁移任何实现。

## A 批：骨架

- **crate。** `rust/crates/mradm-capi`（staticlib）依赖 `mradm-ffi`，并转发其 `sofa` / `diagnostics` / `room-cuda` 特性；`mradm-ffi` 的
  crate-type 改为 `rlib`。`use mradm_ffi as _;` 保证 staticlib 带上 `mradm-ffi` 的全部 `mradm_*` 符号（edition 2018 起未被引用的
  依赖不会链接）。`build.rs` 与 `mradm-ffi` 相同，只把 CMake 选定的 libFLAC 交给 Cargo 自己链接的测试。
- **CMake。** `MRRust.cmake` 经 Corrosion 导入 `mradm-capi`。库名定为 `mradm_rust`（不同于共享库的 `mradm_capi`：Corrosion
  把 staticlib 复制进 CMake 构建目录，Windows 上它与 DLL 的导入库都叫 `mradm_capi.lib`，Ninja 报 “multiple rules generate”；
  CI 43344955147 的 Windows debug 因此失败），目标随之为 `mradm_rust` / `mradm_rust-static` / `cargo-build_mradm_rust`，
  其余规则（PIC、FLAC 链接顺序、macOS SDK 路径、环境变量）照搬。6.6 改为 cdylib 时库名改回 `mradm_capi`。安装的 Rust 归档仍改名为
  `libmradm_ffi.a` / `mradm_ffi.lib`，`MacinRenderADMCoreConfig` 不变，6.5 随 SDK 删除。
- **入口表。** 共享库内没有代码调用 Rust 实现的导出，链接器不会从 staticlib 中取出它们。`check-capi-exports.py --write-references`
  从 `c_api.h` 生成 `generated/mradm_capi_entries.c`：一张引用全部 164 个声明函数的表，编进 `mradm_capi_bundle`。表名
  `mradm_capi_entries` 不在导出面内；它包含 `c_api.h`，所以 Rust 实现缺失时链接失败。未使用的 `bundle_stub.cpp` 删除。
- **panic 兜底。** `mradm-capi` `guard`：每个导出体经 `catch_unwind` 执行，panic 时返回与 C++ `catch (...)` 相同的回退值
  （`code` 为 `ADM_ERROR_INTERNAL`，`pointer` 为 NULL，`unit` 无返回值）。兜底不持有状态，回调线程与重入语义由各导出自己保持。
- **签名核对。** `check-ffi-headers.py` 另用 cbindgen 渲染 `mradm-capi`，与 `c_api.h` 单向核对：Rust 的每个 `adm_*` 都须在 `c_api.h`
  中以相同签名声明，结构体按字段比较；枚举 typedef 按 `int`（`c_api.h` 以 `static_assert` 保证）、回调 typedef 按签名、标量 typedef
  按所指类型比较；`ADM_API_NOEXCEPT` / `ADM_API_DEPRECATED` 先去掉。仍在 C++ 的函数不报错，由 `check-capi-exports.py` 在共享库上
  校验导出。`quality_scripts_test.py` 新增对应测试，并要求解析出的函数恰好是 `c_api.h` 声明的 164 个。

## 规模

| 区域 | 6.2 | 6.3 A |
| --- | ---: | ---: |
| production（C/C++） | 21,124 | 21,121 |
| `island-interfaces` | 988 | 988 |
| `rust/crates/mradm-capi`（Rust，不设棘轮） | — | 103 |

`src/adm_c_api` 3,545 → 3,542 行（`bundle_stub.cpp`）。

## 验证（本机 Linux x64，Release，`MR_ADM_BUILD_CAPI_BUNDLE=ON`）

参照为 main `66bfa9d` 的同配置构建（二进制另存），候选为本批源码：
- **CLI 记录**（`cli-transcript.py`，`--tool mradm=参照`）：102 条命令 0 差异，另有 3 条 help 文本只差可执行文件路径；
- **C ABI 采集**（`capi-capture.py`，`--library 参照`）：3,283 行、36 个文件 0 差异；consumer 测试换库后在两个库上都通过；
- **完整一致性采集**（`collect.py --tool mradm=参照`，`compare-collections.py`）：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- **入口表**：把 `adm_api_version_major` 临时移到 `mradm-capi`（C++ 定义删除）后，共享库恰好导出 164 个 `adm_*`，consumer 测试通过；
  去掉入口表后 `check-capi-exports.py` 报缺 1 个。实验未提交；
- ctest 85/85，`mr_adm_rust_quality`（fmt + clippy），FFI 头校验（412 个私有函数、171 个结构体；`c_api.h` 164 个函数中 0 个由 Rust
  实现），`check-capi-exports.py`（164 个），许可证校验（Cargo 清单登记 `mradm-capi`），`language-surface.py --check`。

待维护者：macOS / Windows 构建与导出校验（入口表在 MSVC 下以 C 编译，`#pragma warning(disable : 4996)` 消去弃用警告），
两平台完整 CTest 与新旧对照，GUI 包照旧打包（库名与导出不变）。

## 下一步

B 批迁移版本、上下文与内存（7 个导出）：在 `mradm-capi` 中实现并删去对应 C++，`c_api.h` 的类型在 Rust 中按名声明（不透明句柄、
`i32` 枚举），用同一套对照验收。
