# Scene 流式引擎迁入 Rust（五期 5.4）

> 五期 5.4 的记录（2026-11-08 至 2026-11-10）。路线见[审计五期一节](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)，
> 驱动面清单见 [5.1 记录](RUST_ORCHESTRATION_BASELINE.md)，文件流式见 [5.3 记录](RUST_ORCHESTRATION_STREAM.md)。
> 5.7（room 默认 ramp）按用户决定提前到 5.4 之前完成，见 [5.7 记录](RUST_ROOM_DEFAULT_RAMP.md)。

## 范围

5.4 把 Scene 流式渲染的编排移入 `rust/crates/mradm-engine` 的 `scene` 模块，包括：
- 渲染器的选择与创建；
- `SceneStreamEngine`：epoch、generation、输入队列、worker、时间线、输出环、EOS、fence、诊断与 `scene/*` 检查点；
- `SceneOutputSession` 中与设备无关的部分，以及 HpTF 的热切换运行时。

一致性工具 `mr_adm_phase2_scene` 换成 Rust，至此四个一致性工具都是 Rust。最后一批 E 修正了迁移中照原样保留下来的五个怪癖。

| 批次 | 原 C++ | Rust |
| --- | --- | --- |
| A | `create_live_renderer` 的分派，以及四个 `create_live_{vbap,triple_balance,binaural,room}_renderer`：请求映射、binaural 的 live HRTF 状态、room 的屏幕校验、房间资源、采样率与布局检查、拒绝 SAF spreader | `scene::renderer`：`Renderer` 分派与检查点后缀（`.30-vbap<N>` / `.30-triple` / `.30-binaural` / 无），`create`；FFI `mradm_engine_scene_renderer_create` |
| B | `SceneStreamEngine`（2,271 行）：epoch 与 generation、帧校验与深拷贝、输入预算与背压、worker 与待处理控制、语义策略、后端交叉淡化、preroll 与有理数时间线、重采样、输出环、尾声与 EOS、fence、诊断库、`scene/*` 检查点；Windows room worker 的 MMCSS 与高精度定时器 | `scene::stream`（`mod` / `worker` / `ring` / `trace`）；FFI `mradm_engine_scene_*`，Windows 部分在 `scene_worker.rs` |
| C | `SceneOutputSession` 的回调 pull、立体声耳机馈送的预取、峰值保护之前的 HpTF、音量、epoch 重置与状态优先级；`HptfProcessor` 的两个三槽 mailbox 与发布 | `scene::output::Session`；`mradm-dsp` `hptf::Live`；FFI `mradm_engine_scene_output_*`、`mradm_dsp_hptf_live_*` |
| D | `tests/tools/phase2_scene.cpp` 与 `phase2_io.h` | `mradm-ffi` example `phase2_scene` |
| E | —— | 五个怪癖的修正（见发布说明） |

### C++ 剩下的部分

- **`SceneStreamEngine`**（`src/adm_realtime/scene_stream_engine.{h,cpp}`，518 行）：
  - 是 `mradm_engine_scene_*` 的薄壳，接口不变，所以 C ABI、`SceneOutputSession`、测试与手动基准都没有改；
  - 负责描述、帧、状态、错误与诊断的转换；`pull` 直接调用 FFI；
  - 读诊断文字时，如果条目在两次读取之间被替换，就重读。
- **`SceneOutputSession`**（`src/adm_engine/scene_output_session.{h,cpp}`，357 行）：
  - 持有设备；`create()` 负责设备选择与布局检查；
  - 涉及设备的步骤拆开执行：先在 Rust 里 park 回调，再暂停并清空设备，最后在 Rust 里重置；
  - 回调先标记设备结束，再释放本次 pull。
- **设备岛**：miniaudio、Apple `AVSampleBufferAudioRenderer`、Windows `ISpatialAudioClient` 与测试用的捕获设备，`IAudioOutputDevice` 不变。
- **`HptfProcessor`**（`src/adm_render_common/hptf_eq.*`）：`hptf::Live` 的包装，供 MonitorEngine 使用。
- **归 5.5**：`ring_buffer.h` 与 `room_worker_wait.*`，MonitorEngine 仍在用。
- **请求组装**：`src/adm_render_room/scene_renderer_request.h` 组装渲染器请求，持有 UTF-8 文字。
- **测试专用 C++**：
  - `tests/support/live_scene`：`ILiveSceneRenderer` 接口、`create_live_renderer`、Rust 渲染器的适配、`live_scene_trace`；
  - `tests/support/dsp`：重采样器与 Scene 过渡的包装、`StereoPeakGuard`、`apply_output_volume`、`HptfCascade`；
  - 冻结参考的 include shim 放在 `tests/support/frozen_includes`，参考文件的哈希不变。

### 保持不变的规则

- **字节与文字**：A–D 的 PCM、检查点、错误码、文字、context、诊断与状态逐项相同。E 的变化都是有意的，见发布说明。
- **实时约束**：
  - 回调侧不等待、不分配：`Stream::pull` 与 `Session::pull` 都有分配计数测试；
  - 输出环是无锁 SPSC，槽存 f32 位模式；
  - `hptf::Live` 的每个槽按协议只归一侧所有，回调只用 `try_lock`，没有 unsafe。
- **时钟**：生产路径用单调时钟；重放改用手动时钟（`set_clock`），FFI 不传函数指针。
- **检查点与棘轮**：每个检查点名只归一方，`scene/*`、`device/*`、`hptf/r*` 都改由 Rust 写出；棘轮只降，每批在同一提交运行 `--update`。

### 用户决定

- **2026-11-06**：
  - 当期的问题当期处理；
  - 5.4 分 A–F 六批：A–D 是保持逐字节不变的结构迁移，E 修正迁移中发现的怪癖，F 写文档；
  - 四期遗留的 room 默认 ramp `×48` 提前做（5.7）；
  - BINAURAL-001 这次不动。
- **2026-11-09（E）**：
  - 尾声长度统一到 `end_input()`：双耳不再渲染尾声，与离线和文件流式的双耳一样，输出长度等于输入长度；
  - 诊断日志按 epoch 清空，并设上限。

## 发布说明

1. **A**：
   - 删除 `mradm_render_live_{vbap,triple_balance,binaural,room}_create` 及其请求记录，由 `mradm_engine_scene_renderer_create` 取代；
   - 删除四个 `live_*_renderer` 创建文件与 `room_internal::screens`；
   - `HrtfState::prepare_live` 与 `mradm_render_binaural_hrtf_live` 保留给 `sofa_fixture_test`。
2. **B**：
   - **重放时钟**：私有重放缝 `SceneStreamConfig::clock` 改为手动时钟（`manual_clock` 加 `set_clock`），`mr_adm_phase2_scene` 每次推进时间后调用它。
   - **修正**：结束补零的循环遇到 reset 或关闭时退出。原 C++ 在等环空间时收到 seek 或关闭会一直空转，`begin_epoch` 或析构因此不返回。带 seek 目标的变采样结束只补 1 帧，就能触发这种情况。
   - **内部错误**：按构造不会出现的错误（过渡尺寸不符、分配失败）由 terminate 或 `bad_alloc` 改为 worker 失败或 Rust 中止；worker 线程创建失败时，文字取 Rust 的 io 错误。
   - **删除**：`live_scene_state.h`、`live_policy_ffi.h` 与私有导出 `mradm_live_policy_*`。
   - **维护者修正**：`a601331` 恢复了 Scene 流适配壳的质量门禁（clang-tidy 与 cppcheck 的发现）。
3. **C**：
   - **HpTF 发布**：`hptf::Live` 在发布时拒绝处理器会拒收的目标（原 C++ 路径会在之后的回调里 terminate）；回调收到格式不对的缓冲区时，不再消耗待处理目标。这两点都是新测试发现的。
   - **删除**：`mradm_dsp_hptf_processor_*` 与 `MradmDspHptfUpdate`。
   - **维护者修正**：`64176db` 让 `set_hptf_prepared` 不再持有 `control_`，与原 C++ 的锁范围一致，并新增测试 `test_prepared_hptf_does_not_wait_for_output_control`。
4. **D**：
   - **与 C++ 工具的差别**：用例 JSON 有语法错误时，报错文字改为 mradm-json 的文字；数值字段照 nlohmann `get<>` 的规则转换。
   - **维护者修正**：`4adb6e5` 补齐 nlohmann 的隐式规则：null 的 `metadata` 与语义集合按空列表处理，布尔值转成 0 / 1 浮点；room 夹具路径接受重复的分隔符，拒绝 `.` 组件。
5. **E**：五项有意的行为变化。
   1. **状态只前进**：
      - 推入数据时只会从 buffering 进入 running，`signal_end` 之后的 draining 不再被改回 running；
      - 生产结束时先进入 draining，再标记生产完成，pull 报出的 EOS 不会再被改成 draining；
      - EOS 可从 buffering、running 或 draining 进入 ended；
      - reset 与失败照旧直接写入状态。
   2. **诊断日志按 epoch**：
      - worker 的 reset 屏障清空日志；
      - 每个 epoch 最多 4096 条，满了之后最新的 error 替换最后一条，其余条目不再记录；
      - 上一 epoch 迟到的条目被丢弃；
      - `semantic_degradations` 仍按整个会话累计；
      - `c_api.h` 的 Scene 线程约定写明这条规则。
   3. **旧 epoch 的 fence**：在 `begin_epoch` 之前发出的 fence 立即返回 “Scene replay fence was cancelled by a new epoch”，不再等到超时。每个 fence 自带完成标志，所以后面 epoch 的 fence 完成不会让它被误判为已完成。
   4. **尾声长度**：
      - 结束后渲染的尾声长度等于 `end_input()` 计入时间线的帧数：room 是房间尾声，双耳为 0；
      - 双耳的卷积尾声不再先渲染、再丢弃；
      - PCM 有变化：88 个 Scene 用例中有 36 个，都是变采样、且最后一个渲染器是双耳的用例：双耳 16 个，SOFA 8 个，会切到双耳的立体声 VBAP 8 个，设备 DSP 4 个；
      - 输出长度不变，只有每个 epoch 最后 111–231 帧不同。原因是重采样器收尾时看到的是补零，而不是原来被丢弃的尾声；第二个 epoch 只有 17–66 帧，整段都不同；
      - 同采样率的用例逐字节相同。门禁按跨平台比较，`phase2-gates.json` 不需要改。
   5. **尾声片段的检查点键**：尾声片段从结束样本开始计位置，不再和 s0 片段同名。只影响诊断构建。
   - **维护者修正**：
     - `376c5aa`：fence 改为每个 fence 自带完成标志；诊断日志改在 worker 的 reset 屏障清空；日志满时保留最新的 error；C++ 读诊断文字时，条目在两次读取之间被替换就重读；
     - `9084d1d`：让并发 fence 的回归测试同步运行，覆盖 32 个 epoch 中排队的 128 个旧 fence。
     - 我原来的写法用单调递增的 fence 编号判断完成：新 epoch 的 fence 完成后，旧 epoch 被丢弃的 fence 也会被当成已完成。

## 测试与植入缺陷

| 批次 | 新测试 | 植入并被抓到的缺陷 |
| --- | --- | --- |
| A | `mradm-engine/tests/scene_renderer.rs`（5 个） | 3 处：room 的输出检查放到资源准备之前、Triple Balance spread 映射、VBAP 后缀不带声道数 |
| B | `tests/scene_stream.rs`（13 个），pull 的分配计数测试 | 6 处，包括 preroll 边界不进位、retarget 漏掉 cleared 字段、pull 不计欠载、watermark 不看追踪 lookahead、释放预算时不比较 serial |
| C | `tests/scene_output.rs`（8 个），输出 pull 的分配计数测试，`hptf::live` 4 个 | 5 处：预取量、HpTF 放到峰值保护之后、reset 不重置 HpTF、状态优先级、mailbox 不置 dirty 位 |
| D | `phase2_tools_test.py` 的 Scene 用例改用 Rust 工具（69 个测试；`4adb6e5` 加了 4 个兼容回归） | 6 处：信号、分区边界、追踪时钟、设备控制帧、`events.json` 缩进、room 尾声 |
| E | `tests/scene_stream.rs` 3 个，`tests/scene_tail_trace.rs`（只在 `--features mradm-dsp/diagnostics` 下检查），C ABI 的 draining 用例；维护者另加 `scene/stream/tests.rs` 与 `scene_stream.rs` 中的并发 fence、在途策略诊断与日志饱和后的失败等回归 | 7 处：状态写回、清空日志、日志上限、fence 取消、尾声按 `tail()` 渲染、尾声起点为 0 等 |

D 中有一处时钟比例的变体观察不到：一个 epoch 内只有追踪过期的时刻会影响结果。

## 验证

本机对照都在 Linux x64、config A 的 Release 构建上进行，基线是每批开始时的 main。

| 批次 | 对照 | 结果 |
| --- | --- | --- |
| A（基线 `a1dd9e1`） | 88 个 Scene 用例，plain 与 diag | plain 704 个文件相同；diag 22,592 个文件中只有一个 VBAP 设备回放的 12 个 device trace 切分不同，按样本拼接后 36 条流相同 |
| B（基线 `bce2e50`） | 88 个 Scene 用例；C ABI 情景探针，覆盖 12 个情景：room、双耳尾声、变采样 preroll、延后的后端切换、策略更新、epoch 与全部校验错误 | plain 与 diag 结果同 A；探针在新旧构建上各跑两次，逐字节相同 |
| C（基线 `6f41f95`） | 88 个 Scene 用例；输出会话探针，覆盖 HpTF 档案、淡化、自动衰减、旁路与暂停、两个 epoch、有无设备音量、大音量双耳、全部错误文字与状态优先级 | plain 704、diag 22,598 个文件全部相同，其中 878 个 `device/*`、48 个 `hptf/*` 检查点；探针每份构建跑两次，逐字节相同 |
| D（基线 `8ff855a`） | 按 5.1 的切换规则：同一构建上分别用 C++ 工具和 `--tool` 换入的 Rust 工具采集，再 `compare-collections`；88 个 Scene 用例；切换后再采集一次 | 全部相同：294 个输出、220 个夹具、989 项比较、176 个重放状态、256 个 artifact、666 份日志；diag 只有两个碎片设备回放的 42 个 device trace 切分不同；切换后 outputs、comparisons、重放状态与 artifact 相同，日志规范化采集根路径后相同 |
| E（基线 `c181824`） | 88 个 Scene 用例，plain 与 diag | 只有上述 36 个用例有变化（92 个 pcmbits 文件），其余相同。diag 中另外只有两类差别：同采样率用例少了被丢弃尾声的 trace 文件；一个碎片设备回放的 trace 切分不同。36 条拼接后的设备流中，只有属于已变化用例的 18 条不同 |

- **常规检查**：
  - ctest 83/83，每批连跑 3 次，B 跑了 4 次；
  - B、C、E 把 Scene 相关测试重复 30 次，E 还包括 HpTF、realtime 与 Rust 单元测试；
  - clippy、fmt、FFI 头校验（A 后 339 个函数、143 个结构体，B 后 351 / 147，C 后 367 / 149，D、E 不变）、clang-format / tidy、许可、retention、version 与 manual 目标都通过。
- **仓库替换**：5.4 期间仓库换成了新的同名公开仓库，历史已重写，本记录的 SHA 都是新仓库的。A、B 合并后的 Consistency 运行在旧仓库，这里不列。
- **新仓库的 Consistency**：

  | 合并点 | 运行 | 结果 |
  | --- | --- | --- |
  | `6f41f95`（含 A、B） | 42590033479 | 通过 |
  | C（`8ff855a`） | 42629279543 | 通过 |
  | D（`c181824`） | 42650805546 | 通过；四份报告都是 294 个 PCM id 相同、门禁失败 0 |
  | E（`12c823c`） | 42737302477 | 写本记录时仍在运行 |

- **待你本地验证**：macOS 上的系统空间 Scene 输出（`AVSampleBufferAudioRenderer`）；Windows 上的 `ISpatialAudioClient`、miniaudio，以及 room worker 的 MMCSS 与定时器路径。

## 规模

C/C++（`language-surface.py`）：

| 合并点 | 生产 | 测试 | `src/adm_realtime` | `src/adm_engine` | `src/adm_render_common` | `tests/tools` | 说明 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 5.7（`a1dd9e1`） | 29,260 | 48,870 | 5,438 | 2,049 | 3,806 | 2,393 | 5.4 基线 |
| A（`bce2e50`） | 29,105 | 48,947 | 5,417 | 2,049 | 3,791 | 2,399 | 四个创建函数合为一个 Rust 入口 |
| B（`c590320`） | 26,672 | 49,537 | 3,415 | 2,049 | 3,499 | 2,395 | `SceneStreamEngine` 只剩薄壳；测试专用 C++ 移入 `tests/support` |
| C（`8ff855a`） | 26,363 | 49,885 | 3,314 | 1,967 | 3,379 | 2,395 | 输出会话与 HpTF 运行时移出 |
| D（`c181824`） | 26,363 | 49,238 | 3,314 | 1,967 | 3,379 | 1,748 | C++ 重放工具删除 |
| E（`12c823c`） | 26,363 | 49,297 | 3,314 | 1,967 | 3,379 | 1,748 | 只加测试 |

- **累计**：5.4 共减少生产 C/C++ 2,897 行。五期至今 32,551 → 26,363，减少 19.0%。
- **按后端**：A 中 `src/adm_render_vbap` 94 → 53，`src/adm_render_triple_balance` 101 → 54，`src/adm_render_binaural` 278 → 231，`src/adm_render_room` 305 → 321（新增请求组装头）。
- **Rust**：`mradm-engine` 10,432 → 16,275 行，`mradm-ffi` 21,011 → 22,586 行（含 example `phase2_scene`），`mradm-dsp` 40,297 → 40,617 行（`hptf::Live`）。
- **岛**与 CUDA 边界不变。
