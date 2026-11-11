# 一致性工具与命名清理（五期 5.8）

> 五期 5.8 的记录（2026-11-11）。路线见[审计五期一节](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)；
> 用户 2026-11-04 决定：期次与实现语言不再出现在工具、目标与源码注释的命名中。

## 范围

5.8 只改名字与注释，不改行为：PCM、检查点名、错误文字、日志、CLI 输出与 C ABI 不变。已有记录、`RUST_PHASE2_*.md`
与 `evidence/` 保持原名；本文与此后的文档只写新名字。

### A：一致性采集（`eaef4e9`）

| 原名 | 新名 |
| --- | --- |
| `scripts/consistency/run-rust-phase2.py` | `scripts/consistency/collect.py` |
| `scripts/consistency/phase2_common.py` | `scripts/consistency/collection.py` |
| `scripts/consistency/compare-rust-phase2.py` | `scripts/consistency/compare-evidence.py` |
| `scripts/consistency/phase2-gates.json` | `scripts/consistency/gates.json` |
| `mr_adm_phase2_{kernels,render,stream,scene}`（CMake 目标与二进制） | `mr_adm_consistency_{kernels,render,stream,scene}` |
| `rust/crates/mradm-ffi/examples/phase2_*.rs`、`examples/phase2/` | `examples/consistency_*.rs`、`examples/consistency/` |
| `mr_adm_phase2_tools`（构建并写 stamp 的目标） | `mr_adm_consistency_tools` |
| `phase2-build-stamp.json` | `consistency-build-stamp.json` |
| `tests/unit/phase2_tools_test.py`、ctest `mr_adm_phase2_tool_tests` | `tests/unit/collection_tools_test.py`、`mr_adm_collection_tool_tests` |
| `tests/fixtures/room/phase2{,-96k}` | `tests/fixtures/room/synthetic{,-96k}`（字节不变，SHA-256 钉住值不变） |
| `tests/tools/make_room_phase2_fixtures.py` | `tests/tools/make_consistency_room.py` |
| manifest schema `mradm.phase2.v1` / `.gates.v1` / `.comparison.v1` | `mradm.consistency.collection.v1` / `mradm.consistency.gates.v1` / `mradm.consistency.comparison.v1` |

`collect.py --tool` 的名字随工具改为 `mr_adm_consistency_{render,stream,scene}=<路径>`。CI 步骤 “Collect Rust phase-2
baseline” 改为 “Collect consistency evidence”。`retention.json` 的退役条件 `phase2-cleared` 指历史上的二期确认，保留原名。

### B：C++ 包装与测试目标（`cb6d52b`）

| 原名 | 新名 |
| --- | --- |
| `RustWavReader` / `RustWavChunks` / `RustWavWriter` / `RustOutputWriter` / `RustCafReader`（`src/adm_audio/wav_backend.h`） | `WavFileReader` / `WavChunkReader` / `WavFileWriter` / `PcmOutputWriter` / `CafFileReader` |
| `src/adm_render_common/rust_error.h`、`RustError` | `ffi_error.h`、`FfiError` |
| `tests/support/live_scene/live_rust_renderer.{h,cpp}` | `live_renderer_adapter.{h,cpp}` |
| `{hoa,live_binaural,live_vbap,scene_numeric,triple_balance}_rust_test.cpp`、`mr_adm_*_rust_tests` | `*_parity_test.cpp`、`mr_adm_*_parity_tests`（与冻结 C++ 参考对照） |
| `wav_rust_test.cpp`、`mr_adm_wav_rust_tests` | `wav_io_test.cpp`、`mr_adm_wav_io_tests` |

`mr_adm_rust_unit_tests`、`mr_adm_rust_quality`、`MacinRender::RustDSP` 等指 Rust 工具链本身（`cargo test`、rustfmt /
clippy、Rust staticlib），按功能命名，不改。冻结参考 `tests/reference/libbw64/reference_test.cpp` 只改所对照的读取器类型名，
算法未改，`retention.json` 哈希同步更新。

### C：生产注释（`27f5749`）

`include/`、`src/`、`rust/crates/*/src`、`cmake/` 与 `CMakeLists.txt` 的注释不再引用切片编号（“PCM consistency 5.2 C1”、
“(4.6)”、“since 5.6 B”、“audit N02”），改为说明代码做什么；追溯留在迁移记录与 git 历史。删除 `PreviewSession` 上过时的
Phase 2a / 2b 说明与 GUI 头追踪注释中的 Phase 编号。测试代码与 `NOTICE.txt` / `PATCHES.md` 等来源记录不在此列。

## 验证

本机 Linux x64：
- **A**：同一个 `consistency-a` Release 构建，用新脚本采集一次；再把同一批二进制以旧名放进另一目录，用 `main`（`bb01510`）
  的旧脚本写 stamp 并采集一次（各 134 s）。按旧名 → 新名映射后，两份采集的 1,642 个文件逐字节相同：294 个 PCM 输出、
  256 个内核与检查点、220 个夹具、989 条比较、176 条回放状态、666 份日志（日志按各自的采集根目录归一）；二进制哈希相同，
  日志里没有工具名。manifest 中只有采集根目录、源码指纹、日志清单的原始哈希（日志含采集根目录）与这次手工旧名目录的构建记录不同。
  `collection_tools_test.py`（73 项，跳过 1 项）与 `compare-collections.py` 对新采集的完整性校验通过；
- **B、C**：debug 全量构建、ctest（不含 `mr_adm_rust_unit_tests`）、`check-reference-retention.py`、
  `language-surface.py --check`、`format.sh --check` 通过（`binaural_render_fixture_test.cpp` 维护者 clang-format 版本带来的
  既有差异除外）。

待维护者验证：macOS 与 Windows 规范构建（`src/adm_apple`、`src/adm_windows` 只改了注释；改名的类型在跨平台的
`adm_audio`、`adm_realtime`、`adm_engine` 与 `adm_render_room`）、Consistency CI（PCM 应全部不变）。维护者本机脚本中的旧脚本名、工具名与 `phase2-build-stamp.json` 需按上表更新。

## 规模

只删了注释行：生产 C/C++ 由 21,776 行降到 21,772 行。
