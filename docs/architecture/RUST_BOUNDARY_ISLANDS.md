# 岛改为由 Rust 调用（六期 6.2）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)六期 6.2，清单与对照工具见 [6.1 记录](RUST_BOUNDARY_BASELINE.md)。
> 6.2 分三批：A 编码器、容器元数据与 IAMF MP4 打包；B Apple 后端与岛流表（见 [B 批](#b-批apple-后端与岛流表)）；
> C 系统空间 sink 与 miniaudio 设备。A、B 两批的 PCM、日志、进度事件、错误文字与输出文件都不变。

## 方向

五期里离线渲染的 Rust 状态机遇到岛请求就暂停，把请求交回 C++ `RenderService` 执行后再 `resume`。6.3 起 C ABI 与 CLI 改为 Rust，
没有 C++ 循环来执行岛请求，所以岛要先改成 Rust 能直接调用的窄 C 接口。A 批处理离线渲染中不依赖平台框架的岛请求：
1（MP4 打包器探测）、4（FLAC）、5（Opus MKA）、6（APAC）、7（IAMF）、8（IAMF MP4 打包）、9（FLAC / Matroska / MP4 元数据）。
2、3（Apple 后端的准备与渲染）仍交回 C++，留给 B 批。

## 设计

- **执行点。** `mradm-engine` `render` 新增 `Islands` trait 与 `Render::next_with(cancelled, host, islands)`。岛请求仍按原顺序排进事件队列，
  排到队首、此前排队的事件都已返回之后，才交给 `Islands::run`；它给出回复时由状态机内部 `resume`，否则照旧作为 `Event::Island` 返回。
  回复后返回 `Event::Continue`，调用方在推进下一步前重新读取取消状态，保留编码结束到输出安装之间的取消点。
  事件顺序与进度区间保持。`Render::next` 等同于 `next_with(…, None)`。
- **窄 C 接口。** `mradm-ffi` 新增 `MradmIslandRunner`（`run` 函数与调用方的 `MradmIslandContext`）和
  `mradm_engine_render_set_islands`；`run` 返回 0 表示已执行（失败写在回复里），非 0 表示交回调用方。请求与回复沿用
  `MradmEngineRenderEvent` / `MradmEngineIslandReply`，不新增字段。`MradmIslandContext` 只有两个回调：`cancelled` 查询调用方的取消，
  `progress` 接收已映射到渲染区间的进度事件。
- **岛入口。** `src/adm_audio/island.cpp` 的 `mradm_audio_island` 执行上述七种岛，代码由 `RenderService::Islands` 原样移来；
  它把 `cancelled` 回调桥接成编码器使用的 `std::stop_token`（开始前先查一次，运行中每 2 ms 轮询一次），把编码器的
  `ProgressSink` 事件原样交给引擎，由引擎按 `map_progress` 同样的规则映射后转给调用方。异常不能穿过 Rust 栈帧，入口内捕获后
  作为 `internal_error`（“render island failed”）回复；五期中这类异常会从 `RenderService::render` 抛出，再由 C ABI 转成 INTERNAL。
- **C++ 外壳。** `RenderService::render` 在开始渲染后注册 `mradm_audio_island` 与自己的取消、进度回调，`Islands` 类只剩 Apple 分支。
  进度桥为 `noexcept`，在 C++ 内捕获并保存调用方异常，同时发布原子停止标志供编码器的取消线程读取。
  Rust 调用返回后再重抛给 C++ 调用方，公开 C ABI 沿用 `INTERNAL` 与空结果句柄的异常路径。
  6.3 的 Rust C ABI 注册同一个入口，以自己的取消标志与进度回调作为 `MradmIslandContext`。

## 规模

用户决定（2026-11-12）：岛供 Rust 调用的窄 C 接口单独计量。`language-surface.py` 新增 `island-interfaces` 区域，只收
`ISLAND_INTERFACES` 中逐个列出的文件（不按目录归类），新文件在加入它的同一提交由 `--update` 按实测登记，之后逐文件只减不增，
`--base-ref` 拒绝提高已登记的上限；production、islands、cuda 三个区域的规则不变。B、C 批的接口文件同样登记到这里。

| 区域 | 6.1 | 6.2 A |
| --- | ---: | ---: |
| production | 21,772 | 21,761（`render_service.cpp` −28，`engine_ffi.h` +17） |
| island-interfaces | — | 205（`island.cpp` 188、`island_ffi.h` 17） |
| islands / cuda | 3,045 / 512 | 不变 |

## 验证

本机 Linux x64，参照为 main `95a38b9` 的 `build/consistency-a`（另存的 `mradm`、`libmradm_capi.so.1`、采集工具与 consumer 测试），
候选为本批的同配置构建：
- **CLI 记录**（`cli-transcript.py`，`--tool mradm=候选`）：102 条命令 0 差异。另有 3 条 help 文本差异，是 CLI11 用法行打印了可执行文件
  自身的路径（参照与候选在不同目录），与本批无关；contract 类命令没有这种输出。
- **C ABI 采集**（`capi-capture.py`，`--library 候选`）：3,248 行、36 个文件 0 差异，其中 FLAC、Opus MKA 渲染的结果字段、输出文件与
  编码期间映射后的进度事件（`encoding FLAC` 的区间分数与帧数）逐位相同；换库运行两个 consumer 测试通过。
- **完整一致性采集**（`collect.py --tool mradm=参照`，`compare-collections.py`）：产物 256 项、日志 666 份相同。这次比较在 rustfmt
  调整排版之前完成；排版不改变行为，最终源码上的 CLI 记录与 C ABI 采集已重新对照。
- **单元测试**：`mradm-engine` 渲染测试 13 项，新增的 `islands_run_in_order_inside_next_with` 证明 `next_with` 与手动 `resume`
  的事件序列、映射后的岛进度与结果逐项相同，被拒绝的岛照旧返回；新增 `mr_adm_audio_island_tests` 覆盖原始进度、开始前取消、
  编码进行中取消、Apple 与未知岛交回、空参数；`quality_scripts_test.py` 新增岛接口的登记、增长、收紧、删除与 `--base-ref` 用例。
- 完整 ctest 85/85 通过；`mr_adm_rust_quality`（rustfmt + clippy）、`mr_adm_ffi_header_check`（416 个函数、166 个结构体）通过；
  `language-surface.py --check --base-ref origin/main` 通过；clang-tidy 对新增与改动的 C++ 文件无新增提示。

2026-11-12 维护者复核，测试源码为 `bb08c86c`，参照为候选构建前保存的主线 Release 二进制。
macOS arm64 使用 `build/release`（SOFA on），Windows x64 使用规范 MSVC/Ninja Release 构建（SOFA off）。

| 平台 | 完整 CTest | CLI 命令 | C ABI 采集 | 换库 consumer |
| --- | ---: | ---: | ---: | ---: |
| macOS arm64 | 90/90 | 102 | 3,400 行、36 个输出文件 | 2/2 |
| Windows x64 | 88/88 | 102 | 3,144 行、34 个输出文件 | 2/2 |

两平台的 CLI 契约与 C ABI 新旧对照均通过，macOS APAC 覆盖成功路径。
两平台各有三个 help 输出只因可执行文件路径不同而产生信息性差异，退出码与输出文件保持一致。
Rust 渲染测试增至 14 项，新增内联编码结束后的取消用例。
正式 C ABI 回归覆盖 FLAC 编码首尾回调异常、结束时取消，以及有无既有输出的六种组合。
回调异常保持 `INTERNAL` 与空结果，取消保持 `CANCELLED`，两者均保留既有输出并清理临时文件。
macOS 改动文件的 clang-format、clang-tidy、cppcheck、Rust fmt/clippy、FFI 头校验（416 个函数、166 个结构体）
和相对 `origin/main` 的规模棘轮通过。原生音频输出与 CLI/C ABI 采集均使用 Release 构建。

本轮两平台均为 `MR_ADM_ENABLE_IAMF=OFF`。启用 IAMF 的构建及真实 IAMF/MP4 打包对照仍待单独验证。

## B 批：Apple 后端与岛流表

### 方向

A 批之后 Apple 后端仍由 C++ 执行两处：离线渲染的岛请求 2（准备）、3（渲染）由 `RenderService::Islands` 用 C++ `RenderOptions`
拼 `RenderPlan` 执行，准备状态放在 C++ 的 `slot`（`PreviewSession` 跨窗口复用，Rust 只拿到 `apple_cached` 标志）；监听经宿主回调
`open_apple` 由 `MonitorSession` 打开 Apple 流，再经 `src/adm_realtime/island_stream.cpp` 包成 `MradmEngineIslandStream`。
6.3 起没有 C++ `RenderOptions`、`LogSink` 与 `slot`，所以 B 批把 Apple 后端改为 Rust 直接调用的窄 C 接口：Rust 用自己的请求与场景
构造参数、持有准备状态，C++ 外壳删去执行 Apple 的代码。

用户决定（2026-11-12）：Apple 岛内部继续使用现有的 `AppleRenderer` / `AppleStream`（`IRenderer` / `IRenderStream`），
`spatial_mixer_renderer.cpp` 不改，Apple 测试照旧调用 `create_apple_renderer()`；Apple 内部停用这两个接口推到 6.5，与删除
`include/adm/render.h` 一起做。平台事实（`catalog.cpp` 的 Apple 能力记录、LFE 自检、系统空间布局）随 C 批。

### 设计

- **窄 C 接口**（类型在 `engine_ffi.h`，cbindgen 校验）。`MradmAppleSettings` 是 Apple 实际读取的 `RenderPlan` 字段：输入路径、
  布局、DirectSpeakers 路由与矩阵、22.2 LFE 拆分、扬声器与双耳 spread、空间预设、扬声器渲染标志与听者朝向，取值与请求记录相同。
  `MradmAppleIslands` 是四个入口：`prepare`（接管一份场景副本，成功时给出不透明的 `MradmApplePrepared`）、`render`（输出路径与窗口，
  回复带 LUFS / True Peak）、`open_stream`（返回岛流表，流自己持有准备状态）、`destroy`。`MradmIslandContext` 增加 `log`，
  Apple 的日志行经它按原顺序交给调用方；音频岛不写日志。
- **注册。** `MradmIslandRunner` 增加 `apple`，`MradmEngineMonitorSessionHost` 的 `open_apple` 回调换成 `apple`，两处都由外壳在
  macOS 构建中填 `mradm_apple_islands()`，其他平台为 NULL。`mradm_engine_render_begin` 删去 `apple_cached` 参数，
  `mradm_engine_render_scene` 删除。
- **Apple 岛入口。** `src/adm_apple/apple_island.cpp` 由设置构造 `RenderPlan`（代码从 `RenderService::Islands` 与
  `MonitorSession::open_apple` 移来），每次调用新建 `create_apple_renderer()`（与五期每个岛新建一次相同）。取消桥、原始进度与日志
  适配从 `island.cpp` 移到两个岛共用的 `src/adm_audio/island_driver.h`。岛流表的 trampoline 由 `island_stream.cpp` 原样移入，
  保活对象仍持有 renderer 与准备状态。异常不进 Rust 栈帧：`prepare` / `render` 捕获后回复 `internal_error`（“render island failed”，
  同 A 批），`open_stream` 捕获后以 100 与 `what()` 返回，会话外壳照旧重抛（五期的约定）。
- **Rust。** `render::ApplePlan` 增加输入路径与 `AppleOptions`（由请求填写），`Islands::run` 增加渲染场景参数。`mradm-ffi` 的
  `Runner` 把音频 runner 与 Apple 表组合成一个 `Islands`：准备与渲染经 A 批的 `Bridge` 映射进度、转发取消与日志；准备状态放在
  `RenderPrepared::apple`，`take_prepared` 交给调用方缓存，下一次 `begin` 据此跳过准备。`PreviewSession` 的缓存因此统一是
  `EnginePrepared`，外壳不再区分 Apple。监听的 `SessionHost::open_apple` 改为接收 `AppleOpen`（请求、布局、矩阵与已定的朝向）
  和写入会话日志的闭包；system spatial 时朝向为中性的规则从 C++ 移到 Rust。场景每次准备复制一份：离线与五期相同；监听原来借用会话
  场景，现在也复制一份。
- **岛流表。** 生产中的 `island_stream.{h,cpp}` 只剩测试调用（`MonitorEngine` 测试外壳把 C++ 流交给 Rust Monitor），移到
  `tests/support/realtime/`。

### 规模

| 区域 | 6.2 A | 6.2 B |
| --- | ---: | ---: |
| production | 21,761 | 21,403（`src/adm_engine` −182，`src/adm_realtime` −213，`engine_ffi.h` +37） |
| island-interfaces | 205 | 614（新登记 `apple_island.cpp` 353、`apple_island.h` 14、`island_driver.h` 113；`island.cpp` 188 → 117） |
| islands / cuda | 3,045 / 512 | 不变 |

### 验证

本机 Linux x64（Apple 后端不编译），参照为 main `3bdeb17` 的 `build/consistency-a`（另存的 `mradm`、`libmradm_capi.so.1`、采集工具与
consumer 测试），候选为本批的同配置构建：
- **CLI 记录**：102 条命令 0 差异；另有与 A 批相同的 3 条 help 文本差异（可执行文件路径）。
- **C ABI 采集**：3,283 行、36 个文件 0 差异；采集工具新增 `preview.apple`（Apple 预览两个窗口，非 macOS 记录 `unsupported`），
  参照与候选用同一工具；换库运行两个 consumer 测试通过。
- **完整一致性采集**（`collect.py --tool mradm=参照`，`compare-collections.py`）：fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同。
- **单元测试**：`mradm-engine` 新增 Apple 岛经 `next_with` 收到请求字段与场景、缓存准备时跳过准备的用例，以及监听 Apple 流的朝向
  规则与日志写入会话的用例；`mradm-ffi` 以假的 `MradmAppleIslands` 表验证设置逐字段、日志与映射后的进度、准备状态的保存与恰好一次
  的 `destroy`、准备失败与缺少准备的回复。完整 ctest 85/85 通过；`mr_adm_rust_quality`、`mr_adm_ffi_header_check`（415 个函数、
  168 个结构体）与 `language-surface.py --check --base-ref origin/main` 通过。
- Linux 上用 macOS 构建的预处理分支（`-D__APPLE__ -fsyntax-only`）检查了 `apple_island.cpp`、`render_service.cpp`、
  `monitor_session.cpp`、`apple_island_test.cpp` 与 `c_api_fixture_test.cpp`；clang-tidy 对新增与改动的 C++ 文件无新增提示。

待 macOS 验证：
- 新增 `mr_adm_apple_island_tests`（仅 macOS 注册）：真实表的准备、渲染、窗口、开始前与渲染中取消、监听流在准备句柄释放后仍可渲染、
  不支持的布局与 NULL 参数；
- `c_api_fixture_test` 的 macOS 分支：Apple 渲染中进度回调抛异常 → `INTERNAL` 与空结果、WAV 清理直写输出、FLAC 保留既有输出并清理临时文件，预览两个窗口；
- 完整 CTest、CLI / C ABI 新旧对照（含 `render-apple*` 与 `preview.apple`）、换库 consumer，以及实机 Apple 监听（含 system spatial
  与后端切换）。
