# adm_windows:系统空间音频监听 sink（ISpatialAudioClient）

> 状态：已实现（Windows-only，实时监听路径）。本文记录 `adm_windows` 的定位、ISpatialAudioClient 静态床设计、布局映射、空间化器能力实测、切换空间化器的失效恢复，以及与 macOS 的对照与后续方向。
>
> 相关：ADR 0003（自有领域模型与后端边界）、ADR 0005（错误处理）、`docs/architecture/REALTIME_MONITORING.md`、`docs/architecture/ADM_APPLE_BACKEND.md`（macOS 平台后端）。

## 1. 定位与范围

`adm_windows` 提供 **Windows-only 的「系统空间音频」监听 sink**，是 macOS `AVSampleBufferAudioRenderer`（ASBR）sink 的对位实现。

「系统空间音频」监听模式 = 渲染多声道**床**（不下混）交给操作系统做 HRTF。这条路在 core 里早已解耦：`MonitorSession` 按 `options.monitor_system_spatial` 选一个 push sink（`realtime::IAudioOutputDevice`）；macOS 选 ASBR，Windows 选本模块的 `ISpatialAudioClient` sink。

**重要：`adm_windows` 不是渲染器（不实现 `IRenderer`）**，与 `adm_apple` 不同。它只是一个**音频输出设备 / sink**，被实时监听引擎调用；床由 EAR / VBAP 等真实渲染器产出。`adm_apple` 既有离线渲染器（AUSpatialMixer）又有监听 sink（ASBR），而 Windows 只补了监听 sink。

支持的监听布局：`7.1.4`（`4+7+0`）、`5.1.4`（`4+5+0`）、`5.1`（`0+5+0`）。

不在范围内：
- **OS 级头部追踪**：Windows 的空间化器（Windows Sonic / Dolby Atmos / DTS Headphone:X）不提供动态头追。床按朝向中性渲染，做静态空间化。源端头追（OpenTrack 等）是独立后续 feature。
- 离线渲染：`monitor_system_spatial` 只作用于实时监听，离线渲染路径忽略。

## 2. 模块边界

- 模块：`src/adm_windows/`，target `mr_adm_windows`，别名 `MacinRender::ADMRenderWindows`，CMake `if(WIN32)` 门控。
- 工厂：`realtime::make_spatialaudioclient_device(layout_id)`，声明在第三方无关的 `src/adm_realtime/audio_output_device.h`（`#if defined(_WIN32)`）。
- **ADR 0003 边界**：Windows COM / SpatialAudio 类型（`spatialaudioclient.h`、`mmdeviceapi.h`、WRL `ComPtr`）只允许出现在 `src/adm_windows/` 内部；工厂返回纯接口 `IAudioOutputDevice`，不泄漏任何 COM 类型。
- sink 选择点：`src/adm_engine/monitor_session.cpp::make_monitor_device()`，在 `#elif defined(_WIN32)` 分支下当 `monitor_system_spatial` 为真时返回本 sink，否则回落 miniaudio。
- 链接：只有 `mr_adm_engine` 在 `if(WIN32)` 下链接 `ADMRenderWindows`（sink 在 `monitor_session` 被调用）；renderer_factory / CLI 不需要——它不是渲染器。

## 3. 架构：事件驱动 pull + 静态声道床

ISpatialAudioClient 是**对象制**而非声道制:不存在「把 12 声道塞进 N 声道端点」。我们把床的每个声道作为一个**单声道静态 spatial audio object**（`AudioObjectType_FrontLeft` 等）提交，空间引擎再把这组对象 HRTF 渲染到端点实际格式（耳机 2ch）。这正是微软文档推荐的第一种集成法（“7.1.4 panner + 补高度声道 → 静态声道床”）。

与 ASBR 的关键差异决定了实现更简单：

| | macOS ASBR | Windows ISpatialAudioClient |
|---|---|---|
| 模型 | push / buffered（自带队列、独立时钟） | **event-driven pull**（自起渲染线程，等缓冲完成事件） |
| 需要 | prefill / stall 迟滞 / setRate / flush 一整套 | 无——`pull_is_realtime_playback()=true`，flush/pause/resume 用基类 no-op |

渲染线程循环（`SpatialAudioClientDevice::pump()`）：

1. `WaitForSingleObject(buffer_event_, 100)`：事件每 ~10ms 触发一次（系统要数据）；100ms 超时是安全网，使事件停发后仍能探测。
2. `BeginUpdatingAudioObjects(&dyn, &frame_count)`。
3. 从引擎 ring 调 `pull_()` 拉一个交织块（短读补静音）。
4. **去交织**写进每个静态对象的单声道 buffer（静态对象首轮惰性激活、之后复用 ComPtr）。
5. `EndUpdatingAudioObjects()`。

引擎暂停时 pull 返回静音、sink 照常喂静音，所以无需 sink 侧 pause/flush 逻辑。

### 3.1 对象格式

每个静态对象格式固定 `WAVE_FORMAT_IEEE_FLOAT / 1ch / 48000Hz / 32bit`，`IsAudioObjectFormatSupported` 校验后激活。实测三个空间化器**只接受这一种对象格式**（见 §6），监听本就跑 48k，因而总是匹配；若不匹配则 `start()` 返回 `unsupported`。

## 4. 布局 → AudioObjectType 映射（`windows_layouts.h`）

每个布局给出按 **project 交织声道序** 排列的 `AudioObjectType` 列表；声道序对齐 `src/adm_engine/layout_table.cpp` 的 CoreAudio Atmos 序（监听 sink 收到的就是这个序，与 CAF/APAC writer 一致）。

| 布局 | 声道序（= AudioObjectType 顺序） |
|---|---|
| `4+7+0`（7.1.4） | FL FR FC LFE **SL SR**(侧) **BL BR**(后) TpFL TpFR TpBL TpBR |
| `4+5+0`（5.1.4） | FL FR FC LFE **BL BR** TpFL TpFR TpBL TpBR |
| `0+5+0`（5.1） | FL FR FC LFE **BL BR** |

要点：7.1.4 里 project 的 `Ls/Rs` 是**侧环绕**（另有 `Rls/Rrs` 后环绕）→ 映射到 `SideLeft/SideRight` + `BackLeft/BackRight`；5.1/5.1.4 无独立后环绕，其唯一环绕对映射到后位（与 WAVEFORMATEXTENSIBLE 5.1 mask 约定一致）。映射用**具名常量**，不依赖手写位值。`static_object_mask()` 把列表按位或得到 `StaticObjectTypeMask`。

## 5. 跨平台能力暴露（`system_spatial_layouts`）

「系统空间音频」监听后端的**可用布局唯一权威源** = capabilities JSON 的 `system_spatial_layouts` 字段（`src/adm_engine/capability_json.cpp`）：macOS 取 `apple_capabilities()` 非-binaural 布局，Windows 取 `realtime::system_spatial_layouts()`（`adm_windows` 导出的第三方无关数据），其它平台为空。

GUI（`OutputModel.SystemSpatialLayouts`）读它，Windows 与 macOS 都解开「系统空间音频」监听后端；渲染床后端子下拉在 Windows 为 EAR（默认）/ VBAP（无 Apple）。**不在 GUI 硬编码白名单**。无新 C ABI signature（纯 JSON 内容追加）。

## 6. 空间化器能力实测

在 win-pc（设备 `You Kant Listen Stereo`）逐个切换 Windows 声音设置的空间格式，经 `ISpatialAudioClient` 实测，并与微软文档核对：

| 空间格式 | 静态床 | 动态对象（MaxDynamicObjectCount） | 对象格式 | MaxFrameCount |
|---|---|---|---|---|
| Windows Sonic（耳机） | 17 位（8.1.4.4） | 111（文档 112） | mono/48k/32f | 556（11.6ms） |
| Dolby Atmos（耳机） | 17 位（8.1.4.4） | 16 | mono/48k/32f | 556（11.6ms） |
| DTS Headphone:X | 17 位（8.1.4.4） | 32 | mono/48k/32f | 556（11.6ms） |

结论：

- 三者**静态床、对象格式、缓冲帧数完全相同**；本 sink 的 7.1.4 静态床三者通用。
- 原生静态床是 **8.1.4.4（17 位）**：环绕 8（含 BackCenter）+ LFE 1 + 顶 4（TpFL/FR/BL/BR）+ **底 4（BottomFL/FR/BL/BR）**。我们只用其中 12 位（7.1.4 子集）。
- 唯一差别是**动态对象容量**：Sonic > DTS > Dolby，但三者都 > 0（数字对上官方“较早 Windows 版本”资源表；新 servicing 把三者统一到 128；LFE 不计入额度）。
- **`MaxDynamicObjectCount` 只在该空间化器为当前默认端点活配置时才准**；查询非激活端点会得到基线值（常为 0）。空间格式按**端点**绑定，监听 sink 走 `GetDefaultAudioEndpoint(eRender,eConsole)`，所以监听设备需是带空间格式的默认输出。

## 7. 切换空间化器的失效恢复

运行中在声音设置切换空间格式（Dolby/Sonic/DTS），当前 `ISpatialAudioObjectRenderStream` 会被**作废**：缓冲事件停发，且 `BeginUpdatingAudioObjects` 返回 stream-lost 错误（实测 `0x88890100`，具体码因空间化器而异，也可能是 `SPTLAUDCLNT_E_RESOURCES_INVALIDATED` / `STREAM_NOT_AVAILABLE`）。

处理（`render_loop` + `pump`）：

- `pump()` 对 **Begin 的任何 `FAILED` 返回都视为失效**（不硬认某个码），返回 `PumpResult::Invalidated`。健康态事件每 ~10ms 必发、Begin 紧跟信号必 `S_OK`，故仅真失效才 `FAILED`；偶发误判也只重建一次自愈。
- 循环内 `ActivateSpatialAudioObject` / `GetBuffer` 失败同样返回 `Invalidated`（覆盖「失效发生在 Begin 之后」的情形），而非静默 `continue` 丢掉对应声道让监听看起来正常。床类型恒为 8.1.4.4 原生 mask 的子集，故激活不会因真配置问题失败、只会因流已死失败。
- `render_loop` 收到 `Invalidated` → `teardown()` 释放流 → `Sleep(100)` 让切换 settle → 重抓默认端点与当前空间化器 `setup_stream()`；切换瞬间暂不可用则退避重试到成功或 stop。
- `buffer_event_` 提升到**设备生命周期**（`start()` 建、`stop()` join 后关），跨重建复用、`setup` 时 `ResetEvent`，避免句柄 churn 及与 `SetEvent` 竞态。

效果：监听**无缝挺过切换**（仅一次短暂停顿=重建间隙），不再需要手动重启监听。官方概览文档未规定失效恢复写法，此为基于实测的自洽方案。

## 8. 与 macOS / 头追的关系

- macOS ASBR 同样喂 7.1.4 床（非对象），且系统含**动态头追**；Windows 静态空间化无 OS 头追。两边的布局白名单各自由 `apple_layouts` / `windows_layouts` 定义，经同一 `system_spatial_layouts` 暴露。
- `monitor_session` 在 system-spatial 下把 `plan.listener_orientation` 强制中性（床不在源端二次旋转），macOS/Windows 同此。

## 9. 局限

- 仅 `7.1.4 / 5.1.4 / 5.1`（静态床 12 位子集）。8.1.4.4 的底层/后中位虽被空间化器支持，但 ADM 的 wide / top-middle 等位无对应具名 AudioObjectType，故未开放更大布局（见 §11）。
- 无 OS 头追（Windows 平台限制）。
- Debug 构建的 capi bundle 为 `/MDd`，依赖 VS 调试 CRT，仅供本机验证，非分发件。

## 10. 验证

- 编译：CI `windows-debug` job + win-pc（规范配方）。
- 回归：win-pc 全量 ctest 23/23。
- 端到端：GUI `--selftest` 确认 `system_spatial_layouts` = `[7.1.4, 5.1.4, 5.1]` 及逐声道标签。
- 听验（真机）：听感正常、声道方位无严重错；切换空间化器自动恢复。
- 注：ISpatialAudioClient 需真硬件 + 启用空间格式，**CI 无法跑监听断言**（类比 APAC 在 Linux skip）；CI 只保证编译。

## 11. 后续方向（吃 ISpatialAudioClient 动态对象；按 `MaxDynamicObjectCount` 运行时 gating，实测三空间化器都 >0）

1. **真对象直通**：ADM audioObject 不经 EAR/VBAP 床，直接当**动态对象** `SetPosition(对象真实方位)` 喂系统，逐对象 HRTF，无床量化更忠实。需坐标映射、对象数 > 槽位时裁剪/编组、非对象内容（DirectSpeakers/HOA）仍走床兜底。对应微软文档第三种集成法。
2. **静态床 + 动态对象组合出扩展布局**：一个 render stream 可同时带 `StaticObjectTypeMask` + 动态对象。把布局每个扬声器映射：床里有的走静态槽，床外的（9.1.6 的 Lw/Rw/Ltm/Rtm、22.2 多余位）用动态对象钉死在固定方位当“虚拟扬声器”。槽位预算（静态 12-13 + 动态 16~128）足够 9.1.6 / 22.2，与 macOS（CoreAudio 22.2 tag）对齐。需 BS.2051 方位 → `SetPosition` 坐标映射 + 听验“固定位动态对象 vs 同位床声道”是否等价。
3. **源端头追**（OpenTrack 等）：在床渲染阶段按 `listener_orientation` 旋转，为 Windows 补头追。
