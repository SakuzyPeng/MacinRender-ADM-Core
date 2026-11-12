# 岛改为由 Rust 调用（六期 6.2）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)六期 6.2，清单与对照工具见 [6.1 记录](RUST_BOUNDARY_BASELINE.md)。
> 6.2 分三批：A 编码器、容器元数据与 IAMF MP4 打包（本记录）；B Apple 后端与岛流表；C 系统空间 sink 与 miniaudio 设备。
> A 批的 PCM、日志、进度事件、错误文字与输出文件都不变。

## 方向

五期里离线渲染的 Rust 状态机遇到岛请求就暂停，把请求交回 C++ `RenderService` 执行后再 `resume`。6.3 起 C ABI 与 CLI 改为 Rust，
没有 C++ 循环来执行岛请求，所以岛要先改成 Rust 能直接调用的窄 C 接口。A 批处理离线渲染中不依赖平台框架的岛请求：
1（MP4 打包器探测）、4（FLAC）、5（Opus MKA）、6（APAC）、7（IAMF）、8（IAMF MP4 打包）、9（FLAC / Matroska / MP4 元数据）。
2、3（Apple 后端的准备与渲染）仍交回 C++，留给 B 批。

## 设计

- **执行点。** `mradm-engine` `render` 新增 `Islands` trait 与 `Render::next_with(cancelled, host, islands)`。岛请求仍按原顺序排进事件队列，
  排到队首、此前排队的事件都已返回之后，才交给 `Islands::run`；它给出回复时由状态机内部 `resume`，否则照旧作为 `Event::Island` 返回。
  所以事件顺序、进度区间与取消点都与五期相同。`Render::next` 等同于 `next_with(…, None)`，Rust 一致性工具不变。
- **窄 C 接口。** `mradm-ffi` 新增 `MradmIslandRunner`（`run` 函数与调用方的 `MradmIslandContext`）和
  `mradm_engine_render_set_islands`；`run` 返回 0 表示已执行（失败写在回复里），非 0 表示交回调用方。请求与回复沿用
  `MradmEngineRenderEvent` / `MradmEngineIslandReply`，不新增字段。`MradmIslandContext` 只有两个回调：`cancelled` 查询调用方的取消，
  `progress` 接收已映射到渲染区间的进度事件。
- **岛入口。** `src/adm_audio/island.cpp` 的 `mradm_audio_island` 执行上述七种岛，代码由 `RenderService::Islands` 原样移来；
  它把 `cancelled` 回调桥接成编码器使用的 `std::stop_token`（开始前先查一次，运行中每 2 ms 轮询一次），把编码器的
  `ProgressSink` 事件原样交给引擎，由引擎按 `map_progress` 同样的规则映射后转给调用方。异常不能穿过 Rust 栈帧，入口内捕获后
  作为 `internal_error`（“render island failed”）回复；五期中这类异常会从 `RenderService::render` 抛出，再由 C ABI 转成 INTERNAL。
- **C++ 外壳。** `RenderService::render` 在开始渲染后注册 `mradm_audio_island` 与自己的取消、进度回调，`Islands` 类只剩 Apple 分支。
  6.3 的 Rust C ABI 注册同一个入口，以自己的取消标志与进度回调作为 `MradmIslandContext`。

## 规模

用户决定（2026-11-12）：岛供 Rust 调用的窄 C 接口单独计量。`language-surface.py` 新增 `island-interfaces` 区域，只收
`ISLAND_INTERFACES` 中逐个列出的文件（不按目录归类），新文件在加入它的同一提交由 `--update` 按实测登记，之后逐文件只减不增，
`--base-ref` 拒绝提高已登记的上限；production、islands、cuda 三个区域的规则不变。B、C 批的接口文件同样登记到这里。

| 区域 | 6.1 | 6.2 A |
| --- | ---: | ---: |
| production | 21,772 | 21,741（`render_service.cpp` −48，`engine_ffi.h` +17） |
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

待维护者在 macOS arm64 与 Windows x64 上验证：两平台的编译（`island.cpp` 在 macOS 调用 AudioToolbox 的 APAC 编码），
CLI 记录与 C ABI 采集的新旧对照（macOS 上 APAC 走成功路径），以及开启 `MR_ADM_ENABLE_IAMF` 的构建中 IAMF 与 MP4 打包的对照。
