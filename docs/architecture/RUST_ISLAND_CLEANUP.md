# 七期 7.6：收尾

> A 批于 2026-11-21 完成，macOS arm64 的 Apple 岛复核见末节。
> 切片与用户决定见[审计七期一节](PCM_CONSISTENCY_AUDIT.md#七期平台岛收缩)，7.5 之前的清单见 [7.1 记录](RUST_ISLAND_BASELINE.md)。

7.3–7.5 之后，生产 C/C++ 只剩 Apple 岛与它的共用头，以及几个窄接口：
- Rust 服务函数表；
- miniaudio 接口；
- 平台事实岛；
- 系统空间 sink 表；
- 进度回调跳板。

7.6 删掉随调用方一起失去用处的部分，并决定进度回调跳板与 Windows 打包路径的去留。

## A：没有生产调用者的共用代码

逐个查共用头、私有 FFI 记录与 Rust 服务表的每一项，看它在 C/C++ 里还有没有使用者（`src`、`tests`，含 Apple 源码），再顺着服务表查到 Rust 侧的实现。删除如下：

| 删除 | 原调用方 | 现在的覆盖 |
| --- | --- | --- |
| `speaker_layouts.{h,cpp}`、`layout_ffi.h`、服务表的 `scene_layouts` 与 Rust `mradm_scene_layouts` / `LayoutEntry` | Windows sink（7.5 迁入 Rust） | `mradm-scene` `tests/layouts.rs` 以 `layouts-v1.txt` 逐位锁定两套布局表 |
| `scene_handle.h` 的 `scene_info`、服务表的 `scene_view`、`adm_ffi.h` 中除 `MradmAdmText` 外的 16 个结构体、`scene_ffi.h` 的 `MradmSceneImportInfo`，以及 Rust 的 `View`、`SceneImportInfo` 与 `SceneHandle` 的视图文本缓存 | 无（`scene_info` 没有调用者） | — |
| Rust `SceneHandle` 为对象和 DirectSpeakers 块维护的两组 C 记录及其重建逻辑 | `mradm_scene_overlay`（R3 G3 已删除） | 语义策略直接更新 Rust `RenderScene`，由预览和渲染测试覆盖 |
| `room_ffi.h`（`MradmRoomScreenGeometry`） | 无 | 记录只由 `check-ffi-headers.py` 核对，不在任何签名中 |
| `render_common.h` 的 `mix_lfe_pair` / `k_lfe_split_power_gain` | Windows sink | `mradm-ffi` `windows_spatial` 的 LFE 对重组与测试 |
| `RenderPlan` 的 `sofa_path`、`default_interp_ms`、`object_smoothing_frames`、`speaker_geometry`、`triple_balance_mode`、`meter_window`（及 `MeterWindow`）、`renderer_semantics_sink`、`room`、`screens` | 无（Apple 岛只读其余字段） | — |
| `options.h` 的 `RenderOptions`、`RenderRequest`、`OutputBitDepth`、`RendererSelection`、`TripleBalanceMode`、`SpeakerGeometry`、`HptfPreampMode`；`room.h`、`screen.h` | 无（选项在 Rust） | — |
| `NullLogSink`、`NullProgressSink`，以及没有定义的 `parse_direct_speakers_matrix` 声明 | 无 | — |
| `system_spatial_island.h` 里可选方法的判断 | 7.5 之后只剩方法齐全的 AVSampleBufferAudioRenderer sink | 模板直接接上全部控制 |

`options.h` 只保留 Apple 岛读取的六个类型（`SpeakerSpreadMode`、`DirectSpeakersRoutingMode`、`BinauralSpreadMode`、`LfeRoutingMode`、`AppleSpatialPreset`、`ListenerOrientation`）。原先经它间接带进来的标准头全部保留，因为 Apple 源码依赖这些间接包含，而这里编译不了 Apple 岛。

### 测试的去向

- `speaker_layouts_test` 改名为 `live_override_test`（ctest `mr_adm_live_override_tests`），保留 Apple 岛使用的实时增益、head-lock 解析与去拉链斜坡测试。布局表断言由 Rust 的逐位基准覆盖。
- `lfe_routing_test` 去掉 Windows LFE 对一项后只剩 Apple 后端，目标改为只在 macOS 构建，文件内的 `__APPLE__` 守卫随之去掉。这个文件不依赖 Apple 框架头，在 Linux 上也能做语法检查。

### 保留的待办

`FloatWavReader` 已没有生产调用者，只有 `wav_io_test` 与 Apple 测试驱动 `apple_island_driver.h` 用它读渲染结果。要删它，得先给 Apple 测试换一个读取方式，而 Apple 测试在这台机器上跑不了，所以留给能跑 macOS 测试的批次。

## 进度回调跳板：保留

`src/adm_c_api/adm_c_api.cpp`（27 行）在 C++ 里调用调用方的进度回调，回调抛出的 C++ 异常在这里被接住，渲染以 `ADM_ERROR_INTERNAL` 结束。

7.6 要求保留这个行为，而 Rust 做不到：
- C ABI 回调是 `extern "C"`，异常越过 Rust 帧是未定义行为；
- 改成 `extern "C-unwind"` 后，`catch_unwind` 也不保证能接住外来（C++）异常，可能直接中止进程。

GUI（C#）与 C 调用方不受影响，但 C++ 调用方会。因此保留这 27 行，它也是生产区域里除 `c_api.h` 外唯一不属于岛的 C++。

## Windows 打包：保留顶层 CMake 树

Windows 的 CLI 发布与 GUI 打包仍经顶层 CMake 树，DLL 与 exe 本身由 Cargo 产出（6.6）。六期结项给出的两个原因都还在：
- 发布前要跑 fixture 测试；
- GUI 包要检查可选的 CUDA 插件与 vcpkg 工具链。

改动发布工作流只能在 tag 或手动触发的 Windows 发布作业上验证，本批不改，结论记在这里。

## 验证（Linux x64）

- **测试**：CTest 21 项全部通过（`mr_adm_lfe_routing_tests` 不再在 Linux 构建，`mr_adm_speaker_layouts_tests` 改名为 `mr_adm_live_override_tests`），Rust 单元测试包含在内。
- **FFI 与导出**：`check-ffi-headers.py` 通过（37 个结构体），`check-capi-exports.py` 确认仍恰好导出 164 个 `adm_*`。
- **Rust 与 C/C++ 质量**：`cargo clippy --workspace --all-targets -D warnings` 与 rustfmt 通过；`check-changed.sh` 的 clang-format 与 clang-tidy 没有新错误。
- **cppcheck**：只报一处 `tests/support/adm_fixture.h` 的 `duplInheritedMember`。没改过的 `c_api_consumer_test.cpp` 上同样出现，属于本机 cppcheck 2.13 的旧提示。
- **Apple 岛的间接核对**：
  - 用全部删除的名字扫描 `src` 与 `tests`，只命中 `c_api.h` 结构的同名字段；
  - `system_spatial_island.cpp` 以 `-D__APPLE__` 做语法检查；
  - 简化后的 sink 模板用一个方法集与 AVSampleBufferAudioRenderer sink 相同的模拟类，在项目警告选项与 ASan / UBSan 下实例化并运行；
  - `lfe_routing_test.cpp` 在 Linux 上做语法检查。

## 规模

| 区域 | 7.6 之前 | A 之后 |
| --- | ---: | ---: |
| 生产 C/C++ | 4,882 | 4,147 |
| 岛（`adm_apple`） | 2,311 | 2,311 |
| 岛接口 | 1,029 | 1,007 |
| 测试 C/C++ | 8,289 | 8,128 |
| Rust（自有） | 150,165 | 149,879 |

## 发布说明

无公开行为变化：C ABI、CLI 与渲染结果不变；删除的都是内部头、记录与服务表项。

## 合并复核（macOS arm64）

- Debug 和 Release 均编译 Apple 岛及相关目标。Apple 渲染、长文件窗口、22.2 LFE、实时覆盖、DirectSpeakers 矩阵、APAC 和 C ABI 的 7 项 Release CTest 全部通过，动态库导出 164 个 `adm_*`。
- 相对主线 `b7c17394`，Release 的四组 Apple 离线渲染 PCM 逐字节相同。用例覆盖 7.1.4 点声源及其语义增益策略、22.2 LFE 直通与等功率拆分。语义报告一致。C ABI 预览分别检查连续两次带静音策略和无策略的窗口渲染，并确认拒绝错误 JSON。
- 系统空间监听通过文件流式渲染的打开、播放、暂停、定位和恢复检查。Scene 流式渲染通过默认设备的系统空间输出、短尾声、定位和音量检查。
