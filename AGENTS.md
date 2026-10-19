# Repository Guidelines

## Project Structure & Module Organization

This is a C++20 CMake project for ADM spatial-audio rendering. Public API headers live in `include/adm/`. Implementation is split by module under `src/adm_*`, for example `src/adm_audio/`, `src/adm_engine/`, `src/adm_render_ear/`, `src/adm_render_vbap/`, and `src/adm_c_api/`. The CLI is in `src/adm_cli/` and builds as `mradm`. Unit and fixture tests live in `tests/unit/`. CMake helpers are under `cmake/`, quality scripts under `scripts/quality/`, and design notes under `docs/adr/` and `docs/architecture/`.

## Build, Test, and Development Commands

- `cmake --preset debug`: configure a Debug build in `build/debug` and export `compile_commands.json`.
- `cmake --build --preset debug`: build libraries, tests, and the `mradm` CLI.
- `ctest --preset debug --parallel "$(sysctl -n hw.ncpu)" --output-on-failure`: run all registered tests in parallel.
- `cmake --preset release && cmake --build --preset release`: produce an optimized build in `build/release`.
- `./build/debug/mradm backends`: smoke-check the CLI after a local build.
- `scripts/quality/check-changed.sh --build-dir build/debug`: run format, clang-tidy, and cppcheck on changed C/C++ files.
- `scripts/quality/check-all.sh build/debug`: run the full local quality gate.

Use Debug builds for correctness checks, fixture tests, and local quality gates. Use Release builds for performance,
runtime, and RSS comparisons; Debug timing or memory data is only directional and should not be reported as final
benchmark evidence.

Important: never use Debug builds to create rendered audio for listening, A/B comparison, loudness/band analysis,
or codec/container output validation. This applies to full-program renders and short excerpts alike, including APAC,
CAF, WAV, FLAC, and any `mradm render` output handed to a user for audition or comparison. Use `build/release/mradm`
for all such renders, rebuilding Release after any temporary source change used to compare legacy/fixed behavior.
Reserve Debug `mradm` only for correctness smoke checks and narrow code-path validation whose output will not be used
as listening or runtime evidence.

### Windows Build Notes

Windows validation runs on the maintainer's Windows test box via a canonical MSVC/Ninja recipe;
the machine-specific script paths live in private local notes (`local/`, not tracked). The
canonical build tree is `build\win-canon`, configured with `cl`, vendored FLAC/Opus, IAMF/SOFA
off, plus the existing FetchContent cache. Production builds no longer depend on OpenBLAS or
LAPACKE (the used SAF subset now runs in Rust). For a clean
Windows full build, delete only `build\win-canon` and re-run the canonical recipe.

Do not use the alternate `win-debug` / `win-msvc` build trees as the default validation path for
this repo: they can hit unrelated MSVC complex-header issues.

## Coding Style & Naming Conventions

Follow `.clang-format`: LLVM base style, 4-space indentation, no tabs, C++20, 120-column limit, sorted includes. `.editorconfig` requires LF endings, UTF-8, final newlines, and trimmed trailing whitespace. Naming follows `.clang-tidy`: namespaces, functions, variables, and enum constants use `lower_case`; classes and structs use `CamelCase`. Keep public headers free of backend-specific third-party types unless that header belongs to the backend boundary.

## Testing Guidelines

维护者今后的主机验证矩阵为 **macOS arm64 + Windows x64**。Linux 测试机已永久下线，
不再安排 Linux 主机验证，也不把缺少 Linux 结果列为验收阻塞项。已有 Linux 支持和历史证据继续保留。

开发、代码修改和常规正确性检查默认在本地工作区完成。**Windows 优先仅指性能筛选与验收顺序**，
不要求优先在 Windows 开发或先把全部正确性检查搬过去。
性能候选先做 **Windows x64** 无插桩 Release 短测；计时前只补齐必要的 Windows 原生构建与烟测。
Windows 短测未达到该候选的验收门槛，就停止该候选，不继续 macOS 性能矩阵或长测；修改形成新候选后，
仍从 Windows 短测开始。ROOM 零超时验收的短测固定 8 workers、96 kHz、512 帧、32 源，
位置运动和距离运动各 1024 次调用；任一超时即不进入六轮长测。短测通过只是进入后续验收的条件，
不等于长测通过。隔离链路的收益不能代替完整会话结果；加载优化按加载收益和正确性独立判断，
不能用来抵消运行时失败。Windows 通过后仍须完成适用的 macOS 验证。

性能验收使用有代表性的正常桌面环境，不要求完全空闲，也不设置后台 CPU 百分比的通过门槛。
保留日常应用和后台服务，记录负载与供电条件；正常后台负载下的超时照常计入，不能据此作废失败结果。
避免额外并行构建或其他基准干扰对照；人为施加的压力负载单独标注。

Tests are standalone C++ executables registered with CTest from `CMakeLists.txt`; there is no separate test framework requirement. Add new tests under `tests/unit/` with names such as `module_smoke_test.cpp` or `module_fixture_test.cpp`, then register them with `add_test`. Prefer smoke tests for API/error-path coverage and fixture tests for real ADM/audio behavior.

Run the narrowest relevant test set by default instead of reflexively running the full suite. For example, use `ctest --preset debug -R "(c_api|cli|binaural)" --parallel "$(sysctl -n hw.ncpu)" --output-on-failure` for focused C ABI / CLI / backend work. When a full suite is warranted by broad engine, layout, ABI, or shared-rendering changes, run it with `--parallel "$(sysctl -n hw.ncpu)" --output-on-failure` so CTest uses the available cores.

## Commit & Pull Request Guidelines

Recent history uses Conventional Commit style, for example `fix: ...`, `ci: ...`, and `refactor: ...`. Keep commits focused and use a lower-case type prefix. Pull requests should describe the behavioral change, list test/quality commands run, link related issues or ADRs when relevant, and note platform-specific impact such as macOS-only APAC or SOFA/FLAC/Opus provider changes.

## Agent-Specific Instructions

Use Chinese for user-facing discussion by default, unless the user explicitly asks for another language. Do not edit generated build directories under `build/`. Preserve existing uncommitted user changes. For public C ABI work, review `docs/adr/0007-c-abi-stability-policy.md`; for error handling, follow `docs/adr/0005-error-handling-model.md`.

Name rendering flows by function: **离线渲染 / offline rendering** (`IRenderer::render_window`),
**文件流式渲染 / file streaming** (`IRenderer::open_stream` → `IRenderStream`, including `BinauralStream`),
and **Scene 流式渲染 / Scene streaming** (`SceneStreamEngine` / `ILiveSceneRenderer`). Use these names in
discussion, documentation, comments, and diagnostics; do not label these supported flows by age or imply
that Scene replaces file rendering. Realtime monitoring describes a use of either streaming flow.
For implementation comparisons, identify the reference implementation, language, version, or commit.
See [rendering terminology](docs/README.md#渲染流程命名).

When validating ADM semantic behavior, prefer the built-in semantic policy path (`--semantic-policy` and
`--write-semantic-report`) over editing source ADM BWF/WAV files. For example, to force direct extent/spreader coverage
on material whose extent blocks are fully diffuse, use a temporary policy that disables diffuse while preserving extent,
then verify the effective metadata in the semantic report.
