# Release Packages

`.github/workflows/release.yml` builds auditable release packages when a `v*` tag is pushed or the workflow is run
manually. Tag builds upload the CLI and GUI packages to the matching
[GitHub Release](https://github.com/SakuzyPeng/MacinRender-ADM-Core/releases). See the
[README](../../README.en.md) for a project overview.

## Artifacts

| Platform | Artifact | Build baseline | Self-contained boundary |
|---|---|---|---|
| macOS arm64 CLI | `mradm-<version>-macos-arm64.tar.gz` | macOS 26 runner | Third-party libraries ship statically; external dependencies are Apple system libraries and frameworks |
| Windows x64 CLI | `mradm-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | Includes `mradm.exe` and required DLLs, plus a `dumpbin /dependents` manifest |
| macOS arm64 GUI | `MacinRender-Gui-<version>-macos-arm64.tar.gz` | macOS 26 runner | Self-contained `.app`; external dependencies are Apple system libraries and frameworks |
| Windows x64 GUI | `MacinRender-Gui-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | Includes the NativeAOT GUI, `mradm_capi.dll`, required DLLs, and a `dumpbin /dependents` manifest |
| Linux x86_64 CLI | `mradm-<version>-linux-x86_64.AppImage` | ubuntu-24.04 runner | Single-file AppImage; built only on manual runs and provided as a workflow artifact, not uploaded to the GitHub Release on tags |

macOS packages use `.tar.gz` and Windows packages use `.zip`. Every artifact has a matching `.sha256`:

```bash
shasum -a 256 -c mradm-<version>-macos-arm64.tar.gz.sha256
```

## Package Contents

CLI packages:

- `bin/mradm` or `bin/mradm.exe`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `BUILD_INFO.txt`
- `DEPENDENCIES.txt`

GUI packages contain a macOS `.app` or Windows `app/MacinRender.Gui.exe`, together with license, build-information,
and dependency files.

## Launching the GUI

After extracting, open `MacinRender ADM.app` on macOS. On Windows, run `MacinRender ADM.cmd` or
`app/MacinRender.Gui.exe`. Packages use local developer distribution: macOS follows the Gatekeeper confirmation flow
and Windows follows the SmartScreen confirmation flow.

## Pre-release Checks

- CLI jobs: after building, check C ABI exports and run a smoke test; after packaging, run a package-level smoke test (`scripts/release/smoke-*`).
- macOS / Linux CLI jobs: additionally run the full license-coverage check (`check-licenses.sh --require-full`).
- GUI jobs: run a package-level smoke test after packaging.

See [third-party licenses](../THIRD_PARTY_LICENSES.md) for the release boundary and the [CI guide](CI.md) for CI design.
