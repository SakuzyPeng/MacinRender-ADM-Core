# 七期 7.7 验收与结项

> 2026-11-22 自动化验收通过。维护者听感与设备格式切换待确认。实机验证使用 macOS arm64 和 Windows x64，GitHub Actions 的一致性矩阵及发布构建继续包含 Linux。

## 自动化验收

| 工作流 | 提交 | 结果 |
| --- | --- | --- |
| Consistency | `c87e18b5` | A、B 两种配置的普通和诊断四份报告均为 294 个 PCM id 一致、0 个不同、0 个门禁失败，平台集完整。[报告](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/45100509870) |
| CI | `c87e18b5` | Debug 矩阵、Linux arm64 Rust Release 测试与版本元数据通过。[CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/45100509851) |
| Quality | `c87e18b5` | 全量质量检查通过。[Quality](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/45100509865) |
| Release Build | `e796cce0` | Windows CLI、Linux AppImage 及两个 GUI 包通过，两个 GUI 包均完成 P/Invoke 冒烟。macOS CLI 打包拒绝了系统库 `/usr/lib/libiconv.2.dylib`。[首次运行](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/45099869204) |
| Release Build | `c87e18b5` | 修复 macOS CLI 打包的系统库白名单。本地打包、归档校验和、解包冒烟通过，注入 Homebrew 同名依赖时仍按预期拒绝。五个打包作业及其冒烟均通过，两个 GUI 包完成 P/Invoke 冒烟。下载重跑生成的 macOS CLI 归档后，实机校验和、构建提交和冒烟均通过。[重跑](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/45100535392) |

## 实机检查

以下原生构建和设备检查使用 `e796cce0`。`c87e18b5` 仅修改 CLI 打包脚本。

- macOS arm64 顶层 Release 构建的 29 项 CTest 全部通过。Debug 的 WAVE / CAF、C ABI、实时覆盖、Apple 岛四项定向 CTest 全部通过。
- Windows x64 规范 MSVC / Ninja Release 构建的 24 项 CTest 全部通过，使用 vendored FLAC / Opus，IAMF、SOFA 和 CUDA 关闭。
- 两个 GUI 发布包在实机分别通过发布冒烟脚本。用同一份合成 ADM 运行包内 `--selftest`，两端均验证语义预览、文件流式监听和双耳 FLAC 导出，进度回调到达 100%。
- 两端实测各导出 164 个 `adm_*`。启用真实系统空间输出的 Scene 流式渲染 C ABI 测试在两端都通过，覆盖 33 帧短尾声、seek 和音量。
- 同一份 12 秒静音 ADM 经文件流式渲染监听，播放、暂停和 seek 后继续均通过。macOS 7.1.4 无欠载。Windows 的 5.1、7.1.4、9.1.6 和 22.2 无欠载，5.1.4 在首次暂停前记录 1 次欠载，恢复播放后计数仍为 1。

仍待维护者按 [Apple 7.4](RUST_ISLAND_APPLE.md#原生行为验证) 确认 AirPods 头追踪听感，按 [Windows 7.5](RUST_ISLAND_WINDOWS.md#原生行为验证) 确认逐声道方位、22.2 双 LFE 听感和系统空间格式切换。验收完成后移除七期临时 CLI 记录工具。

## 规模

数据来自 7.1 和 7.6 B 的 `language-surface-baseline.json`。岛接口行数由脚本按文件另外统计。

| 区域 | 7.1 | 7.6 B | 变化 |
| --- | ---: | ---: | ---: |
| 生产 C/C++ | 8,684 | 3,838 | −4,846（−55.8%） |
| Apple / Windows 岛 | 3,021 | 2,311 | −710 |
| 岛接口 | 1,057 | 1,007 | −50 |
| CUDA SDK 边界 | 512 | 512 | 0 |
| 测试 C/C++ | 9,927 | 7,922 | −2,005 |
| 自有 Rust | 144,619 | 149,879 | +5,260 |

保留 AUSpatialMixer 与 AVSampleBufferAudioRenderer 接口、第三方 C 库的窄接口和 27 行 C++ 进度回调异常跳板。Windows 的顶层 CMake 树继续负责打包和 fixture 测试，产品由 Cargo 产出。[7.6 收尾](RUST_ISLAND_CLEANUP.md)
