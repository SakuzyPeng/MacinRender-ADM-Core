# Windows 系统空间音频监听（ISpatialAudioClient）

> 7.5 起由 Rust 调用 Windows 空间音频接口。C++ 参考实现为 `d5e3a323`。迁移的原型、实时分配计数与两平台验收见 [7.5 记录](RUST_ISLAND_WINDOWS.md)。

## 1. 定位与范围

文件流式渲染监听和 Scene 流式渲染均可将 EAR 或 VBAP 等后端输出的多声道床交给 Windows 空间化器，由操作系统做 HRTF。文件监听通过 `monitor_system_spatial` 选择设备，Scene 输出通过 `ADM_SCENE_OUTPUT_SYSTEM_SPATIAL` 选择。

支持的监听布局：`22.2`（`9+10+3`）、`9.1.6`、`7.1.4`（`4+7+0`）、`5.1.4`（`4+5+0`）、`5.1`（`0+5+0`）。

`22.2`（`9+10+3`）在 Windows 下按 **7.1.4 静态床 + 其余位置动态对象** 提交：`M±030 / M+000 / LFE / M±090 / M±135 / U±045 / U±135` 走 7.1.4 static bed；`M±060 / M+180 / U+000 / T+000 / U±090 / U+180 / B+000 / B±045` 走 `AudioObjectType_Dynamic` 并固定到 BS.2051 方位。它不是把 22.2 折叠成 7.1.4 输出，24 路输入仍逐路提交；只有两个 LFE 共享一个 Windows LFE 对象。

不在范围内：
- **OS 级头部追踪**：Windows 的空间化器（Windows Sonic / Dolby Atmos / DTS Headphone:X）不提供动态头追。床按朝向中性渲染，做静态空间化。源端头追（OpenTrack 等）是独立后续 feature。
- 离线渲染：`monitor_system_spatial` 只作用于实时监听，离线渲染路径忽略。

## 2. 模块边界

实现位于 `rust/crates/mradm-ffi/src/windows_spatial/`。`mod.rs` 提供引擎使用的设备表并管理音频线程，`bed.rs` 定义布局与对象路由，`stream.rs` 负责 MMDevice、COM 流和设备缓冲。Cargo 仅在 Windows 引入 `windows` 绑定。公开 C ABI 与布局 JSON 只使用项目类型。

## 3. 架构：事件驱动 pull + 静态/动态对象床

ISpatialAudioClient 是**对象制**而非声道制:不存在「把 24 声道塞进 N 声道端点」。我们把监听床拆成一组**单声道 spatial audio object** 提交，空间引擎再把这组对象 HRTF 渲染到端点实际格式（耳机 2ch）。

对象分两类：

- **静态对象**：Windows `AudioObjectType_*` 已命名的位置走 `StaticObjectTypeMask`。当前发布布局最多使用 7.1.4 静态床（Front/Side/Back、LFE、TopFront/TopBack）。
- **动态对象**：Windows 静态枚举没有的位置（9.1.6 的 wide/top-middle，以及 22.2 超出 7.1.4 床的其余位置）走 `AudioObjectType_Dynamic`，并在每个 `BeginUpdatingAudioObjects` / `EndUpdatingAudioObjects` 周期用 `SetPosition(x,y,z)` 重申对应 BS.2051 固定方位。位置本身不移动；逐周期重申是因为部分实测端点会丢失只在激活周期设置的坐标并把对象渲染到监听者原点，即使首次 `SetPosition` 返回 `S_OK`。

22.2 有两个 LFE 输入声道，但 Windows 只有一个非空间化 `AudioObjectType_LowFrequency` 槽位。实现上不把第二个 LFE 当动态对象，而是让 `LFE1` / `LFE2` 两个输入声道共享同一个 LFE 对象：当前 render block 只有一路有能量时 unity 通过，两路都有能量时对合成值做约 -3 dB（`0.70710678`）补偿，避免双 LFE 监听低频抬升。

与 ASBR 的关键差异决定了实现更简单：

| | macOS ASBR | Windows ISpatialAudioClient |
|---|---|---|
| 模型 | push / buffered（自带队列、独立时钟） | **event-driven pull**（自起渲染线程，等缓冲完成事件） |
| 需要 | prefill / stall 迟滞 / setRate / flush 一整套 | 无——`pull_is_realtime_playback()=true`，flush/pause/resume 用基类 no-op |

渲染线程运行 `windows_spatial::pump` 和 `Stream::update`。

1. `WaitForSingleObject(buffer_event_, 100)`：事件每 ~10ms 触发一次（系统要数据）；100ms 超时是安全网，使事件停发后仍能探测。
2. `BeginUpdatingAudioObjects(&dyn, &frame_count)`。
3. 从引擎环形缓冲拉取一个交织块（短读补静音）。
4. 对象首轮激活，后续复用 COM 引用；每轮先为动态固定坐标对象重申 `SetPosition`，再**去交织 / 合并**写进每个对象的单声道 buffer（22.2 双 LFE 合并到同一个 LFE 对象）。
5. `EndUpdatingAudioObjects()`。

引擎暂停时 pull 返回静音、sink 照常喂静音，所以无需 sink 侧 pause/flush 逻辑。

### 3.1 对象格式

每个 spatial object 格式固定 `WAVE_FORMAT_IEEE_FLOAT / 1ch / 48000Hz / 32bit`，`IsAudioObjectFormatSupported` 校验后激活。实测三个空间化器**只接受这一种对象格式**（见 §6），监听本就跑 48k，因而总是匹配；若不匹配则 `start()` 返回 `unsupported`。

## 4. 布局与 AudioObjectType 映射

路由表按引擎交织声道序列出每个输入要送往的 Windows 对象。动态对象的坐标来自 `mradm_scene::layouts` 中本次渲染所选的扬声器几何。

| 布局 | 声道序 / 路由 |
|---|---|
| `4+7+0`（7.1.4） | FL FR FC LFE **SL SR**(侧) **BL BR**(后) TpFL TpFR TpBL TpBR |
| `4+5+0`（5.1.4） | FL FR FC LFE **BL BR** TpFL TpFR TpBL TpBR |
| `0+5+0`（5.1） | FL FR FC LFE **BL BR** |
| `9.1.6` | L R C LFE Ls Rs Rls Rrs **Lw Rw** Vhl Vhr **Ltm Rtm** Ltr Rtr；加粗 4 个位置为动态对象 |
| `9+10+3`（22.2） | 原生序为 M+060 M-060 M+000 LFE1 M+135 M-135 M+030 M-030 M+180 LFE2 M+090 M-090 U+045 U-045 U+000 T+000 U+135 U-135 U+090 U-090 U+180 B+000 B+045 B-045；M+000/LFE/M±030/M±090/M±135/U±045/U±135 走 7.1.4 static bed，其余 11 个非 LFE 位置走动态对象；LFE1/LFE2 合并到单个 LFE 静态对象 |

要点：7.1.4 里 project 的 `Ls/Rs` 是**侧环绕**（另有 `Rls/Rrs` 后环绕）→ 映射到 `SideLeft/SideRight` + `BackLeft/BackRight`；5.1/5.1.4 无独立后环绕，其唯一环绕对映射到后位（与 WAVEFORMATEXTENSIBLE 5.1 mask 约定一致）。映射用**具名常量**，不依赖手写位值。`Bed::new` 汇总静态掩码与动态对象数，申请流时把动态对象的最小和最大配额都设为该数。22.2 不做声道折叠降级；它以 7.1.4 static bed 加动态对象表达完整 24ch 监听床。

## 5. 跨平台能力暴露（`system_spatial_layouts`）

GUI 从 capabilities JSON 的 `system_spatial_layouts` 读取支持的布局。Windows 使用与输出设备相同的 `bed.rs` 路由表，macOS 使用 Apple 岛提供的扬声器布局，其他平台返回空数组。

GUI（`OutputModel.SystemSpatialLayouts`）读它，Windows 与 macOS 都解开「系统空间音频」监听后端；渲染床后端子下拉在 Windows 为 EAR（默认）/ VBAP（无 Apple）。**不在 GUI 硬编码白名单**。无新 C ABI signature（纯 JSON 内容追加）。

## 6. 空间化器能力实测

在 Windows 测试机上逐个切换 Windows 声音设置的空间格式，经 `ISpatialAudioClient` 实测，并与微软文档核对：

| 空间格式 | 静态床 | 动态对象（MaxDynamicObjectCount） | 对象格式 | MaxFrameCount |
|---|---|---|---|---|
| Windows Sonic（耳机） | 17 位（8.1.4.4） | 111（文档 112） | mono/48k/32f | 556（11.6ms） |
| Dolby Atmos（耳机） | 17 位（8.1.4.4） | 16 | mono/48k/32f | 556（11.6ms） |
| DTS Headphone:X | 17 位（8.1.4.4） | 32 | mono/48k/32f | 556（11.6ms） |

结论：

- 三者**静态床、对象格式、缓冲帧数完全相同**；本 sink 的静态/动态组合床三者通用。
- 原生静态床是 **8.1.4.4（17 位）**：环绕 8（含 BackCenter）+ LFE 1 + 顶 4（TpFL/FR/BL/BR）+ **底 4（BottomFL/FR/BL/BR）**。当前实现主动限制到 7.1.4 静态床子集，22.2 超出的 BackCenter/Bottom 等位置改由动态对象表达。
- 唯一差别是**动态对象容量**：Sonic > DTS > Dolby，但三者都足够当前发布布局（9.1.6 需要 4 个，22.2 需要 11 个；数字对上官方“较早 Windows 版本”资源表；新 servicing 把三者统一到 128；LFE 不计入额度）。
- **`MaxDynamicObjectCount` 只在该空间化器为当前默认端点活配置时才准**；查询非激活端点会得到基线值（常为 0）。空间格式按**端点**绑定，监听 sink 走 `GetDefaultAudioEndpoint(eRender,eConsole)`，所以监听设备需是带空间格式的默认输出。

## 7. 切换空间化器的失效恢复

运行中在声音设置切换空间格式（Dolby/Sonic/DTS），当前 `ISpatialAudioObjectRenderStream` 会被**作废**：缓冲事件停发，且 `BeginUpdatingAudioObjects` 返回 stream-lost 错误（实测 `0x88890100`，具体码因空间化器而异，也可能是 `SPTLAUDCLNT_E_RESOURCES_INVALIDATED` / `STREAM_NOT_AVAILABLE`）。

每次成功的 `BeginUpdatingAudioObjects` 都配对一次 `EndUpdatingAudioObjects`。开始、写对象、设置坐标、取得缓冲或结束更新失败时，线程发布 `recovering`，停止并释放旧流，等待 100 ms 后重新打开默认端点。若端点尚未就绪，每隔 150 ms 重试。停止设备会唤醒事件等待和重试等待，并等待音频线程退出。

自动重置事件跨流重建复用。新流按 `GetMaxFrameCount` 预分配交织缓冲。一次更新中遇到超过协商上限的帧数会结束该次更新并进入恢复。

## 8. 与 macOS / 头追的关系

- macOS ASBR 同样喂 7.1.4 床（非对象），且系统含**动态头追**；Windows 静态空间化无 OS 头追。两边的布局分别由 Apple 能力表与 Windows 的 `bed.rs` 定义，经同一 `system_spatial_layouts` 暴露。
- `monitor_session` 在 system-spatial 下把 `plan.listener_orientation` 强制中性（床不在源端二次旋转），macOS/Windows 同此。

## 9. 局限

- 9.1.6 / 22.2 中静态床没有覆盖的位置依赖动态对象；若某个未来空间化器动态对象预算不足，`start()` 会在激活阶段返回 `unsupported`，而不是播放中静默丢声道。
- 22.2 当前不是纯 OS static bed，而是 7.1.4 static bed + dynamic extras；方位准确性取决于空间化器对 dynamic objects 的 HRTF 实现。
- 无 OS 头追（Windows 平台限制）。
- Debug 构建的 capi bundle 为 `/MDd`，依赖 VS 调试 CRT，仅供本机验证，非分发件。

## 10. 验证

以下记录来自 C++ 实现。Rust 实现的原生结果与待听验项见 [7.5 记录](RUST_ISLAND_WINDOWS.md)。

- 编译：CI `windows-debug` job + Windows 测试机（规范配方）。
- 回归：Windows 测试机全量 ctest 23/23。
- 端到端：GUI `--selftest` 确认 `system_spatial_layouts` = `[7.1.4, 5.1.4, 5.1, 9.1.6, 22.2]` 及逐声道标签。
- 听验（真机）：听感正常、声道方位无严重错；切换空间化器自动恢复。
- 注：ISpatialAudioClient 需真硬件 + 启用空间格式，**CI 无法跑监听断言**（类比 APAC 在 Linux skip）；CI 只保证编译。

## 11. 后续方向

1. **真对象直通**：ADM audioObject 不经 EAR/VBAP 床，直接当**动态对象** `SetPosition(对象真实方位)` 喂系统，逐对象 HRTF，无床量化更忠实。需坐标映射、对象数 > 槽位时裁剪/编组、非对象内容（DirectSpeakers/HOA）仍走床兜底。对应微软文档第三种集成法。
2. **源端头追**（OpenTrack 等）：在床渲染阶段按 `listener_orientation` 旋转，为 Windows 补头追。
3. **真机听验矩阵**：继续覆盖 9.1.6 / 22.2 的动态对象方位、22.2 双 LFE 合并响度，以及 Dolby/Sonic/DTS 三空间化器切换恢复。
