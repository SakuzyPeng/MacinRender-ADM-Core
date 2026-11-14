# Rust 直接调用 Rust（R2）

> 状态：已完成（2026-11-14，本机 Linux x64）；macOS arm64／Windows x64 原生复核待维护者。
> 依据：[用户决定 2026-11-14](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)，排在 R1（冻结参考退役）之后、R3（C++ 测试移植）之前。

6.3 和 6.4 把 C ABI 与 CLI 迁进 Rust 时，`mradm-capi` 仍照原来 C++ 外壳的顺序调用 `mradm-ffi` 的 `extern "C"` 入口：
它先把自己的数据转成 `#[repr(C)]` 记录和固定大小的消息缓冲，入口再把它们转回 Rust 类型。为了能按 Rust 路径调用这些入口，
`mradm-ffi` 的 18 个模块都以 `#[doc(hidden)] pub mod` 公开。R2 去掉这层往返：`mradm-capi`（以及经它实现 `Service` 的 `mradm-cli`）
直接调用 `mradm-engine`、`mradm-scene`、`mradm-render`、`mradm-dsp`、`mradm-osc` 与共享的 `macin-adm` / `macin-bwf`。
`mradm-ffi` 只保留确实需要 C 表、回调地址或平台服务的部分，并通过一个有文档的模块 `mradm_ffi::api` 提供给 Rust 调用方。

按用户的排序决定（「先 R2，入口留到 R3 删」），`mradm-ffi` 的 `extern "C"` 入口没有删除。它们改为调用同一组 Rust 方法的薄包装，
只剩 C++ 岛与 C++ 测试使用；R3 把测试移植为 Rust 后删除这些入口。

## 分批

| 批 | 提交 | 内容 |
|---|---|---|
| A | `eb4e6cf` | probe、inspect JSON / 文本、策略模板与 export 直接用 `mradm-scene`、`mradm-engine` `scene_json` 与 `mradm-render` `postprocess`；AXML / CHNA 与 WAVE 信息直接读 `mradm-wav`。房间加载、准备与屏幕校验改为 `mradm-ffi` 的安全函数（`RoomProfileHandle::load`、`RoomPreparedHandle::prepare`、`room::validate_screens`、`room::in_environment`），它们保留房间的浮点环境。场景导入经 `SceneHandle::import`，屏幕 JSON 用 `mradm_scene::screen::parse`。 |
| B | `756c508`、`7c2ac9f` | 离线渲染、预览与 export：`mradm-ffi` 新增 `RenderDriver`，它与 `extern` 入口共用同一个 `EngineRender`（创建、执行已登记岛的步进、取出准备结果）。选项与 CLI 直接构造 `mradm_engine::render::Request`，`drive()` 读 `Event` / `Outcome`；export / 预览策略用 `SceneHandle::apply_policy`；五份发现文档直接调用 `mradm_engine::tables`。维护者在 `7c2ac9f` 把 `RenderDriver::new` 改为 `unsafe fn`，写明回调与 Apple 准备的生命周期，并加了 `compile_fail` 回归。 |
| C | `8c82b00`、`4c4a97c` | Scene 流持有引擎流（`EngineSceneStream`，`create_stream` 在 FFI host 上创建），直接调用 `mradm_engine::scene::stream::Stream`：渲染器配置、元素描述、帧平面、初始状态与更新都直接从 C ABI 结构构造，pull 直接写调用方缓冲、不分配。Scene 输出的设备岛会话留在 `mradm-ffi`（`EngineSceneOutput` 的 `unsafe` `open`、play / pause / epoch / 音量 / HpTF / 状态）。房间控制持有 `mradm_render::room_runtime::Control`。`4c4a97c` 修正审查意见：设备回调保存了输出对象的地址，而 `open` 返回普通 `Box`，安全代码可以把对象移走。输出改为 `!Unpin`，`start` / `open` 返回 `Pin<Box<_>>`，并用 `compile_fail,E0277` 守护。 |
| D | `fd76097`、`e2a7658` | Monitor：`adm_monitor_t` 持有 `Box<EngineMonitorSession>`，直接调用引擎的 `monitor::session::Session`。请求用 `engine_request`，覆盖直接构造 `ObjectOverride`，状态、电平、日志与房间控制直接读 Rust 值；`EngineMonitorSession::new` 为 `unsafe fn`。HpTF 文本解析直接用 `mradm_dsp::hptf::try_parse_parametric_eq` 与 `profile_error`。OSC：`adm_osc_head_tracking_t` 持有 `mradm_osc::receiver::Receiver`，由 `mradm-ffi` 按平台绑定（Windows `SO_EXCLUSIVEADDRUSE`）。删除旧的 `EngineRenderRequest` 构造、`Texts` 缓冲与 `RenderRoomControl::take`。 |
| E | `4e499b2` | `mradm-ffi` 只经 `api` 模块提供 Rust 接口，其余模块改为私有，`doc(hidden)` 标记删除。`hptf_infos` 改读 `mradm_dsp` 的 `StereoSnapshot`，HpTF 的 `#[repr(C)]` 记录不再出现在 Rust 接口里。去掉 crate 级的 `allow(clippy::missing_safety_doc)`，5 个仍可从外部调用的 `unsafe fn` 补上 `# Safety`。 |

维护者的 `961370d`（显式链接 Rust C ABI 目标）与 R2 同期合并，R2 本身没有改动 CMake。

## 留在 `mradm-ffi` 的部分

`mradm_ffi::api` 按留下的原因分为 8 组：

- **`islands`**：C++ 岛表（`PlatformIslands` 与 `platform`、`DeviceIslands`、`AppleIslands`）和岛回调传递的 `Text`。
- **`render`**：`RenderDriver` 与岛运行器的记录，`RenderPrepared`，以及 `FfiHost`。`FfiHost` 是引擎的平台服务：房间浮点环境、CUDA、房间切换、
  Windows Scene worker 的定时器。
- **`scene`**：`SceneHandle`，由渲染、流与监听共享的导入场景。
- **`scene_stream`**：`EngineSceneStream`、`create_stream` 与 `EngineSceneOutput`（固定地址）。
- **`monitor`**：`EngineMonitorSession`、`choice` 与 `hptf_design`。
- **`room`**：房间包与准备（浮点环境）、`switch_config`。
- **`hptf`**：从 C ABI 记录读取 HpTF 请求的 `Settings`，以及 `profile_error`。
- **`osc`**：平台绑定的回环接收器。

`extern "C"` 入口放在私有模块里照样导出；`check-ffi-headers.py` 仍核对全部 391 个入口与 163 个结构体。

## 规则

- **`unsafe` 构造**：保存调用方 C 表或裸指针的构造是 `unsafe fn`，带 `# Safety` 一节，调用处写 `SAFETY:` 注释（`RenderDriver::new`、
  `EngineMonitorSession::new`、`EngineSceneOutput::open`）。前两个有 `compile_fail,E0133` 文档测试。
- **固定地址**：设备回调保存地址的对象返回 `Pin<Box<_>>`，类型为 `!Unpin`（`EngineSceneOutput`，`compile_fail,E0277` 守护）。
  E 批把三个 `compile_fail` 测试改用门面路径，并确认每个只因预期的错误码失败。
- **Monitor 会话不需要 `Pin`**：回调的 `Puller` 指向会话里 `Box<Monitor>` 的内容和设备自己的分配（`Arc<dyn Device>`），不指向会话本身；
  会话也总是先停设备，再替换或释放 `Monitor`。移动会话只移动这些指针。这一点写在 `EngineMonitorSession` 的文档里：以后如果把 `Monitor`
  改成内联字段，就必须改用 `Pin`。
- **分配失败**：C ABI 中原本可恢复的分配失败仍返回 `ADM_ERROR_INTERNAL`。ParametricEQ 解析为此新增 `try_parse`，它复用一个 token 缓冲，
  token 与频段都用 `try_reserve` 增长；`mradm-capi` 的分配失败测试现在覆盖解析器本身。
- **门面**：`mradm-capi` 只从 `mradm_ffi::api` 导入，不调用 `mradm_*` 入口。新的 Rust 调用方也一样；需要新的平台部分时加进门面，并在对应分组里写明原因。

## 指标

下表在各提交上用同一方法测量：

- **私有入口**：`mradm-ffi` 中定义的 `extern "C" fn mradm_*`，同时在 `mradm-capi/src` 里被引用的个数。
- **`unsafe` 行**：`mradm-capi/src` 中含 `unsafe` 的行数。

各批提交说明里的入口数用的是另一种计数，以本表为准。

| 提交 | 私有入口 | `unsafe` 行 | `mradm-capi` 行数 | `mradm-ffi` 公开模块 |
|---|---:|---:|---:|---:|
| `b89687c`（基线） | 97 | 984 | 9,448 | 18 |
| `eb4e6cf`（A） | 75 | 954 | 9,149 | 18 |
| `756c508`（B） | 63 | 945 | 9,213 | 18 |
| `8c82b00`（C） | 34 | 914 | 9,009 | 18 |
| `fd76097`（D1） | 12 | 887 | 8,747 | 18 |
| `e2a7658`（D） | 0 | 869 | 8,610 | 18 |
| `4e499b2`（E） | 0 | 868 | 8,602 | 1（`api`，8 组） |

`mradm-ffi` 由 21,311 行增至 21,790 行，增加的是入口与 Rust 调用方共用的方法、`# Safety` 文档与门面。R3 删除入口后这部分会下降。
R2 只改 Rust，生产 C/C++ 行数不变。

## 发布说明

- **C ABI 与 CLI**：二进制接口、返回码、输出与退出码不变；门禁矩阵逐位不变。
- **错误消息不再截断**：原先截在 256 / 512 / 1024 字节或 64 KiB 的固定缓冲区，现在保留 Rust 错误的完整文本，涉及查询、房间、渲染、Scene、
  Monitor、HpTF 请求与 ParametricEQ 解析。只有 Monitor 的 HpTF 设计错误仍按原样截到 511 字节。采集的固定输入中没有超过原缓冲区的消息，所以对照为 0 差异。
- **Rust 接口**（只影响本仓库的 Rust 调用方）：
  - `mradm_ffi` 只公开 `api`。
  - `mradm_dsp::hptf::parse_parametric_eq` 在分配失败时 panic（原来是中止进程），需要可恢复错误的调用方改用 `try_parse_parametric_eq`。
  - `EngineSceneOutput::hptf_snapshot` 返回 `mradm_dsp::hptf::StereoSnapshot`。
- **暂留**：`mradm-ffi` 中只供 C++ 测试使用的 `extern "C"` 入口留到 R3 删除。审计里 R2 一项写的「测试专用导出删除」按上述决定移到 R3。

## 验证

每批都在同一构建（`build/consistency-a`，Release，Linux x64）上与参照 worktree `b89687c` 对照：

- **C ABI 采集**（`capi-capture.py`）：3,283 行、36 个文件，0 差异，含 Scene、Monitor、HpTF 与 OSC 家族。
- **CLI 记录**（`cli-transcript.py`）：113 条命令，0 差异，帮助与解析错误文本也为 0 差异。
- **C ABI consumer**：`mr_adm_c_api_consumer_tests` 与 `mr_adm_scene_stream_c_api_consumer_tests` 通过。
- **PCM 采集**（`collect.py --config a --tool mradm=候选`，再用 `compare-collections.py` 比较）：完全相同，即 220 个 fixture、989 项比较、
  176 个重放、256 个产物、666 份日志。
- **ctest**：debug 构建 74 个全部通过。
- **E 批另行检查**：
  - FFI 头校验（cbindgen 0.29.2）通过。
  - 共享库恰好导出 164 个 `adm_*`。
  - workspace clippy `-D warnings` 与 rustfmt 通过。
  - 这一批建在 main `66ce410` 之上，已接入共享的 ADM / BWF 读写库（[通用 ADM/BWF 读写库接入](SHARED_ADM_FORMAT.md)），对照结果同样为 0 差异。

## 下一步

R3：把 C++ 测试覆盖的行为移植为各 crate 的 Rust 集成测试，C ABI 只保留一个精简的 C 消费者测试。随后删除 `tests/support` 的外壳、SDK、
新旧对照工具，以及 `mradm-ffi` 中只供测试的 `extern "C"` 入口。之后进入 6.6（构建以 Cargo 为主、cdylib 只导出 `adm_*`）与 6.7。
