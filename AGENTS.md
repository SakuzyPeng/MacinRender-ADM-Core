# Repository Guidelines

## Project Structure & Module Organization

This is a C++20 CMake project for ADM spatial-audio rendering. Public API headers live in `include/adm/`. Implementation is split by module under `src/adm_*`, for example `src/adm_audio/`, `src/adm_engine/`, `src/adm_render_ear/`, `src/adm_render_vbap/`, and `src/adm_c_api/`. The CLI is in `src/adm_cli/` and builds as `mradm`. Unit and fixture tests live in `tests/unit/`. CMake helpers are under `cmake/`, quality scripts under `scripts/quality/`, and design notes under `docs/adr/` and `docs/architecture/`.

## Build, Test, and Development Commands

- `cmake --preset debug`: configure a Debug build in `build/debug` and export `compile_commands.json`.
- `cmake --build --preset debug`: build libraries, tests, and the `mradm` CLI.
- `ctest --preset debug`: run all registered tests with failure output.
- `cmake --preset release && cmake --build --preset release`: produce an optimized build in `build/release`.
- `./build/debug/mradm backends`: smoke-check the CLI after a local build.
- `scripts/quality/check-changed.sh --build-dir build/debug`: run format, clang-tidy, and cppcheck on changed C/C++ files.
- `scripts/quality/check-all.sh build/debug`: run the full local quality gate.

## Coding Style & Naming Conventions

Follow `.clang-format`: LLVM base style, 4-space indentation, no tabs, C++20, 120-column limit, sorted includes. `.editorconfig` requires LF endings, UTF-8, final newlines, and trimmed trailing whitespace. Naming follows `.clang-tidy`: namespaces, functions, variables, and enum constants use `lower_case`; classes and structs use `CamelCase`. Keep public headers free of backend-specific third-party types unless that header belongs to the backend boundary.

## Testing Guidelines

Tests are standalone C++ executables registered with CTest from `CMakeLists.txt`; there is no separate test framework requirement. Add new tests under `tests/unit/` with names such as `module_smoke_test.cpp` or `module_fixture_test.cpp`, then register them with `add_test`. Prefer smoke tests for API/error-path coverage and fixture tests for real ADM/audio behavior. Run `ctest --preset debug` before submitting changes.

## Commit & Pull Request Guidelines

Recent history uses Conventional Commit style, for example `fix: ...`, `ci: ...`, and `refactor: ...`. Keep commits focused and use a lower-case type prefix. Pull requests should describe the behavioral change, list test/quality commands run, link related issues or ADRs when relevant, and note platform-specific impact such as macOS-only APAC or SOFA/FLAC/Opus provider changes.

## Agent-Specific Instructions

Use Chinese for user-facing discussion by default, unless the user explicitly asks for another language. Do not edit generated build directories under `build/`. Preserve existing uncommitted user changes. For public C ABI work, review `docs/adr/0007-c-abi-stability-policy.md`; for error handling, follow `docs/adr/0005-error-handling-model.md`.
