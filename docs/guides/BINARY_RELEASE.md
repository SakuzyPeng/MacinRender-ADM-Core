# 发行包

`.github/workflows/release.yml` 在推送 `v*` tag 或手动触发时构建可审计的发行包。tag 构建会把 CLI 与 GUI 包
上传到对应的 [GitHub Release](https://github.com/SakuzyPeng/MacinRender-ADM-Core/releases)。项目概览见
[README](../../README.md)。

## 产物

| 平台 | 产物 | 构建基线 | 自包含边界 |
|---|---|---|---|
| macOS arm64 CLI | `mradm-<version>-macos-arm64.tar.gz` | macOS 26 runner | 第三方库随包静态交付；外部依赖为 Apple 系统库与 framework |
| Windows x64 CLI | `mradm-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | 包含 `mradm.exe` 与所需 DLL；附带 `dumpbin /dependents` 清单 |
| macOS arm64 GUI | `MacinRender-Gui-<version>-macos-arm64.tar.gz` | macOS 26 runner | 自包含 `.app`；外部依赖为 Apple 系统库与 framework |
| Windows x64 GUI | `MacinRender-Gui-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | 包含 GUI NativeAOT 可执行文件、`mradm_capi.dll` 与所需 DLL；附带 `dumpbin /dependents` 清单 |
| Linux x86_64 CLI | `mradm-<version>-linux-x86_64.AppImage` | ubuntu-24.04 runner | 单文件 AppImage；仅在手动触发时构建，作为 workflow artifact 提供，不随 tag 上传到 GitHub Release |

macOS 使用 `.tar.gz`，Windows 使用 `.zip`。每个产物旁都有对应的 `.sha256`：

```bash
shasum -a 256 -c mradm-<version>-macos-arm64.tar.gz.sha256
```

## 包内容

CLI 发行包：

- `bin/mradm` 或 `bin/mradm.exe`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `BUILD_INFO.txt`
- `DEPENDENCIES.txt`

GUI 发行包包含 macOS `.app` 或 Windows `app/MacinRender.Gui.exe`，同样附带 license、build info 和依赖清单。

## 启动 GUI

解压后，macOS 打开 `MacinRender ADM.app`，Windows 运行 `MacinRender ADM.cmd` 或 `app/MacinRender.Gui.exe`。
发行包采用本地开发分发流程：macOS 启动时按 Gatekeeper 流程确认，Windows 启动时按 SmartScreen 流程确认。

## 发布前校验

- CLI job：构建后校验 C ABI 导出、运行 smoke，打包后再做包级 smoke（`scripts/release/smoke-*`）。
- macOS / Linux CLI job：另外运行许可证全量覆盖校验（`check-licenses.sh --require-full`）。
- GUI job：打包后运行包级 smoke。

第三方许可证与发行边界见 [THIRD_PARTY_LICENSES](../THIRD_PARTY_LICENSES.md)，CI 设计见 [CI 指南](CI.md)。
