# Triple Balance 与 room 迁入 Rust（四期 4.6）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)四期 4.6。
> 本切片是结构迁移：锁定矩阵的 PCM、内核文件、诊断检查点、日志、报告与错误消息都要求与迁移前逐字节相同。
> 与旧实现不同的几处行为见[发布说明](#发布说明)，都不改变锁定矩阵的 PCM。

## 范围

Triple Balance 后端的场景改写、床、离线与文件流式渲染、Scene 流式渲染器，以及 room 后端的资源准备、事件表、准备线程状态机、文件流式与 Scene 流式渲染器迁入 `rust/crates/mradm-render`（`#![forbid(unsafe_code)]`），room 的资源与 HRIR 加载迁入 `mradm-dsp`。与 4.5 相同，Rust 后端只读策略后的 Rust 渲染场景（`RenderPlan::scene_handle`），不读 C++ `AdmScene`。

| 批次 | 内容 | 原 C++ | Rust |
| --- | --- | --- | --- |
| A | Triple Balance 输入与 RC 7.1.2 床的检查、私有兼容副本的规范化（rtime 时间轴、512 帧控制块、中性化 gain / mute / start / end）、`triple-balance-objects-v1` / room-222 / D 模式报告、rows / events / 床系数；离线驱动的 TB 内核（size 或 D 模式处理器在增益混音之后） | `triple_balance_renderer.cpp`、`semantics.cpp`、`bed.cpp`，`render_common` 的 `SpeakerBlockProcessor` 与 `prepare_speaker_mix` | `mradm-render` `triple_balance`，`offline::Kernel::TripleBalance` |
| B | 文件流式：1024 帧块与 FIFO、size 为 0 的 canonical point 路径、20 ms 实时增益 ramp（`GainBank`）、32 MiB 检查点 LRU 与 seek 锚点、分步 seek、覆盖校验 | `triple_balance_stream.cpp`、`render_state.cpp` 的 `SizeMixer`、`InterleavedLiveGainSmoother` | `triple_balance_stream` |
| C | Scene 流式 Triple Balance：创建检查、generation 的床标签与 lane、逐帧暂存与原子提交、spread=none 警告、`.30-triple-*` 检查点；两个 Scene 扬声器渲染器共用的元素与状态记录 | `live_triple_balance_renderer.cpp`，`live_vbap` 中的共用记录 | `live_triple_balance`、`live` |
| D | room 资源与 HRIR 加载（有界读取、profile 与数组、原生 SOFA、HRTF collection 与 prepared bank、弱引用缓存、极坐标）；文件渲染的声源描述与事件表（生效块、positionOffset、DS 位置回退与 LFE 角色、screenRef 的参考屏幕、每块三条事件与稳定排序） | `mradm-ffi/src/room.rs` 的准备逻辑，`RoomRenderer::prepare`，`include/adm/screen.h` 的 `resolve_reference_screen` | `mradm-dsp` `room::loading`，`mradm-render` `room` |
| E | room 运行时：worker 侧 session、准备线程、request / epoch / 候选 / 回收规则、pump 与 publish、状态与声源位置快照、控制侧请求；文件流式（2048 帧块、尾声、分步 seek、实时覆盖）；Scene 流式 room 渲染器 | `room.cpp` 的 `room_internal::Runtime`，`room_renderer.cpp` 的 `RoomStream`，`live_room_renderer.cpp` | `room_runtime`、`room_stream`（`FileStream`、`Live`） |

数值内核原本就在 Rust（`mradm-dsp` 的 `triple_balance` 计划、size 混音、D 模式与 live mixer，`room` 的 session、实例与卷积），本切片迁走的是它们外面的场景改写、计划构建、块时间线、状态机与流式胶水。

room 的边界有两处特别处理：
- **浮点环境。** 迁移前每个 C++→Rust 的 room 调用都在 FFI 边界内进入规范的浮点环境（RNE、无 FTZ/DAZ），并捕获 panic。迁移后由 `mradm-ffi` 把这一边界作为作用域（`room_runtime::Scope`）交给 Rust 运行时，原来经过边界的每个调用仍在其中执行，原来在 C++ 侧的算术（如 extent 比例乘积）仍在调用方环境中。`checked()` 的默认文字 “房间 DSP 操作失败” 与 context `room-binaural` 照原样保留。D 的极坐标换算也按这一方式补回了边界（维护者 `1b8463c`）。
- **房间切换的准备。** 新房间的准备（含 CUDA 的选择与实时绑定）留在 `mradm-ffi`，作为闭包交给运行时，切换走的路径与原来相同。

C++ 侧只剩适配：
- `triple_balance_renderer.cpp`（132 行）与 `triple_balance_stream.cpp`（159 行）：能力表、模式检查、报告发布、`IRenderStream` 适配壳与 seek 的取消；
- `room.cpp`（173 行）：`RoomProfile`、`inspect_room`、资源准备、屏幕记录，以及 `IRoomControl` 薄壳 `RoomControl`；
- `room_renderer.cpp`（306 行）：屏幕校验、资源准备、`IRenderStream` 适配壳（格式检查与分步 seek 的取消）和 `render_window` 的写出、计量、进度与取消循环；
- `live_triple_balance_renderer.cpp`、`live_room_renderer.cpp`：创建 Rust 渲染器后交给 `render_common` 的 `live_rust_renderer`。后者转换帧记录、转发警告，并为 room 转发 `end_input`、尾声长度、听者朝向、`room_control` 与 configure 阶段的警告。

`src/adm_render_triple_balance` 由 2,055 行降到 355 行，`src/adm_render_room` 由 1,343 行降到 568 行（CUDA 子目录另计）。只剩测试使用的 Triple Balance C++ 移到 `tests/support/triple_balance/`：`panner`、`size_panner`、`size_processor`、`layout_222`、`bed`、`size_mixer`，以及 `src/adm_dsp` 的 `triple_balance.h` / `triple_balance_ffi.h`；`mradm_dsp_tb_*` 导出保留给它们。`tests/reference/triple_balance/` 的冻结参考不变。

### 保持不变的规则

- **消息与报告。** 检查顺序、错误码、context 与文字逐字保留；Triple Balance 报告由 `mradm-json` 写出，与 nlohmann `dump` 逐字节相同，被拒绝时同样写出报告。
- **时间线。** Triple Balance 首个事件低于一个 512 帧控制块时吸附到 0，否则在其前插入默认点；文件流式 seek 恢复检查点后逐块重放，锚点不被逐出；room 的 seek 先按事件锚点 prime，再按 2048 帧网格重渲有限的声学历史。
- **取消点。** 两个后端的 seek 都拆成 begin / step（Triple Balance 另有 finish）。C++ 在每一步之前检查取消，文字与位置不变（“Triple Balance seek cancelled”、“room-binaural 预热已取消”）。
- **room 运行时。** 请求必须保持档位、HRIR 与坐标比例，HRIR 路径按 `std::filesystem::path` 的元素比较（`.` 与结尾分隔符都是元素）。节目结束后拒绝切换。reset 与 end_input 递增 epoch，使准备中的候选过期。待回收实例最多 8 个，满时 pump 提前返回，实例只在准备线程上析构。状态映射与 preparing / failed 覆盖照旧。
- **无诊断 sink 时。** Scene room 遇到未知扬声器标签时，C++ 只在有 sink 时回退到正前方并发警告，否则保留元素自身位置。这一行为照原样保留（`mradm_render_live_room_create` 的 `sink` 参数）。
- **检查点。** `.30-triple-lane-kinds` / `-lane-gains` / `-commands` / `-values` 改由 Rust 经 `diagnostics::process` 写出，C++ 只拼 trace 前缀；每个名字只归一方。

### 维护者决定

2026-10-24、2026-10-25：
- 4.6 分 A–F 六批，每批单独提交，维护者本机全量对照通过后再开始下一批。
- room 拆为两批：D 只迁资源准备与事件表（C++ `RoomStream` 暂经过渡视图读 Rust 事件表），E 迁运行时状态机、文件流式与 Scene room，并删除过渡视图。
- Scene 流式 Triple Balance 的 spread=none 警告按 generation 的元素顺序发出（发布说明第 1 项）。
- room 离线默认 ramp `default_interp_ms × 48` 照原样迁移，记为已知问题（发布说明第 2 项）。

## 发布说明

都不改变锁定矩阵的 PCM：
1. **Scene Triple Balance 的警告顺序。** 同一帧有多个元素首次触发 spread=none 警告时，现在按 generation 中的元素顺序发出；C++ 按 `unordered_set` 的遍历顺序，与平台有关。警告的内容、条数与“每个 generation 每元素一次”不变。锁定矩阵中观测不到：Scene 重放不记录警告，矩阵里都是 spread=auto。
2. **已知问题：room 离线默认 ramp。** 对象块没有 `interpolationLength` 时，room 的离线与文件流式渲染用 `default_interp_ms × 48` 帧作 ramp，96 kHz 下只有应有长度的一半。本切片照原样迁移，另开批次修复；修复会改变 96 kHz room 的 PCM。
3. **删除 `resolve_reference_screen`。** 它随 room 事件表迁入 Rust 后已无调用者，从 `include/adm/screen.h` 与 `src/adm_core/screen.cpp` 删除（`include/adm` 除 `c_api.h` 外已不是对外接口，ADR 0021 §4）。C ABI 不受影响。
4. **删除的私有导出。** `mradm_dsp_live_tb_*`（C）、`mradm_room_session_*`、`mradm_room_instance_*` 与 `mradm_room_validate_overrides`（E），以及 D 的过渡视图；它们只在 `mradm-ffi` 与 C++ 之间使用，C ABI bundle 本来就不导出。
5. **维护者补充的修正。**
   - `5d69eeb`：手动基准 `triple_balance_monitor_benchmark` 改为导入场景（A 漏改的 `EXCLUDE_FROM_ALL` 目标）；
   - `65a9621`、`ae3d8b6`：Triple Balance 文件流式的覆盖校验与 worker 状态隔离，满足控制线程校验与 worker 处理并发的契约；
   - `89f828d`：两个 Scene 扬声器渲染器直接借用已校验的 PCM 记录，不再逐块分配；
   - `1b8463c`：room 事件表的极坐标换算补回 room 浮点环境（迁移后在非默认舍入模式下有 1 ulp 差别）；
   - `158fc3e`：room 覆盖校验不再借用整个流，状态查询只借用 message 字段，待回收队列改为 8 槽固定数组，worker 上不再分配。

## 黄金数据

| 语料 | 生成方 | 规模 | 结果 |
| --- | --- | --- | --- |
| `mradm-render/tests/triple-balance-v1/` | main `8c406e9` 的 C++ `triple_balance_renderer` / `semantics` / `bed`，经 CLI（`mradm render --renderer triple-balance --write-semantic-report -v`）一次性采集，生成器在 scratchpad（不提交） | 58 个场景（56 个 AXML，2 个普通 WAVE）、1,300 例：三种布局、两种模式、两种 spread、两种 22.2 LFE 路由、六种策略，加后端拒绝的选项；记录退出码、错误、准备日志与规范化报告的 FNV-1a，另有 12 份完整报告 | Rust 重放进入后端的 1,294 例全部一致（470 例准备成功；另 6 例在 CLI 层被拒） |

room 没有另做黄金数据，由锁定矩阵、诊断检查点和下面的新旧对照验收。手工构造或改写场景的 C++ 用例移到 Rust：
- Triple Balance：`tests/triple_balance.rs`（5 个，含截获参考的床增益与点运动）、`triple_balance_stream.rs`（8 个）、`live_triple_balance.rs`（8 个）；
- room：`tests/room.rs`（8 个，同起点块与参考屏幕）、`room_runtime.rs`（6 个）与模块内 2 个，以及维护者补的 `room_runtime_allocations.rs`（1 个）。

每批都植入缺陷，确认测试会报错：

| 批次 | 植入的缺陷 | 结果 |
| --- | --- | --- |
| A | 512 网格、床标签大小写、D 模式采样率检查、首事件的 512 边界 | 都被抓到（后两处是补测试后抓到的） |
| B | 检查点锚点保护、canonical 判断、scale 为 0 时的 point 路径 | 都被抓到（canonical 一处是补了 point 过渡测试后抓到的） |
| C | cleared 字段不还原默认值、空帧也提交、警告倒序 | 都被抓到（空帧一处是补强测试后抓到的） |
| D | ramp 起点、DS 回退顺序、事件优先级 | 前两处被抓到；同一声源的事件本就按时间顺序插入，优先级键无法观测 |
| E | 回收列表上限、reset 不递增 epoch、去掉 ended 守卫、去掉 sink 判断 | 都被抓到；HRIR 路径比较的 `.` 元素差别是新测试在实现阶段抓到的 |

## 验证

**逐批检查（Linux x64，提交前）。**
- Release ctest 全部通过（E 为 81/81，连跑 3 次，`mr_adm_room_tests` 重复 20 次）；
- `mr_adm_rust_unit_tests`、fmt 与 clippy `-D warnings`；
- `mr_adm_ffi_header_check`（E 之后为 331 个函数、135 个结构体）；
- 变更 C++ 的 clang-format 与 clang-tidy；
- `check-reference-retention.py`、`version_metadata.py --check`、`language-surface.py`、`check-licenses.sh`；
- 各批显式构建 `tests/manual` 下的目标（`mr_adm_triple_balance_*`、`mr_adm_room_*`、`mr_adm_scene_stream_stress` 等）。E 另以 stream 模式跑一次 `mr_adm_room_scene_benchmark`，完成了一次房间切换（`applied_revision` 1，无欠载）。

本机没有 cppcheck，由 CI 运行。

**新旧对照（scratchpad 工具，不提交）。** 基线是各批开始时的 `main`。离线 CLI 比较退出码、`-v` 日志与输出字节（bext 的墙钟日期时间置零）：

| 批次 | 基线 | 用例 | 结果 |
| --- | --- | --- | --- |
| A | `8c406e9` | `--renderer triple-balance` 992 例（343 例渲染），比较输出字节与语义报告字节；VBAP 1,148 例、HOA 886 例、EAR 2,096 例重跑；6 个 `stream-triple-*` 重放 | 全部相同 |
| B | `6940df9` | `mr_adm_phase2_stream` 650 例（195 例渲染，含 6 个矩阵重放及其他夹具、seek 目标、extent 比例、静音与碎片分块的变体）；离线 992 例重跑 | 全部相同 |
| C | `a072146` | 全部 88 个 Scene 重放，普通与诊断构建 | 普通 704 个文件相同；诊断 22,588 个文件相同（396 个 `.30-triple`、534 个 `.30-vbap`），唯一差别是一个设备回放的最后一次回调把相同样本合进一个文件（基线为两个） |
| D | `305159f` | `--renderer room-binaural` 1,014 例（632 例渲染，含 96 kHz 包副本、SOFA 与屏幕配置）；16 个 room 文件流式重放；4 个 Scene room 用例；3 个离线 room 矩阵用例 | 全部相同 |
| E | `72c188b` | room CLI 1,014 例（632 例渲染）；16 个 room 文件流式重放；3 个离线 room 矩阵用例；全部 88 个 Scene 重放（普通与诊断） | 全部相同；诊断构建拼接设备 trace（`canonicalize_device_trace`）后 21,756 个文件相同，其中 room 880 个 |

**维护者本机验证。** 逐批验证的范围以各自记录为准。D、E 的两平台 Release 对照均关闭一致性诊断插桩；
以下记录列出其素材、用例与输出覆盖。合入后 config A/B 的普通与诊断三平台结果见本节末的 CI 记录。

- [ROOM 准备迁移审查与对照](RUST_ROOM_PREPARATION_REVIEW.md)：`1b8463c` 的修正，以及 macOS arm64 与 Windows x64 上 57 份最终 PCM 与 `305159f` 逐位相同（离线 9、文件流式 16、Scene 4 例的 8 个 epoch、非默认浮点环境 24），两平台之间也相同；
- [ROOM 运行时迁移审查与对照](RUST_ROOM_RUNTIME_REVIEW.md)：`158fc3e` 的修正，以及两平台各 197 份最终 PCM 与 `72c188b` 逐位相同（离线 9、文件流式 16、Scene 74 例的 148 个 epoch、非默认浮点环境 24），两平台之间也相同；
- C 的 `89f828d`：macOS arm64 上全部 88 个 Scene 用例、704 个输出文件与 `a072146` 相同，重复性与分块检查记录在 evidence JSON。

**规模。** 生产 C/C++ 由 39,747 行降到 36,931 行（−2,816）：

| 批次 | 行数 |
| --- | --- |
| A | 39,747 → 38,755 |
| B | 38,755 → 38,392 |
| C | 38,392 → 37,683（`89f828d` 未改变总行数） |
| D | 37,683 → 37,520 |
| E | 37,520 → 36,932，`158fc3e` 后为 36,931 |

`src/adm_render_common` 由 2,979 行增到 3,306 行：两个 Scene 扬声器渲染器的共用适配 `live_rust_renderer` 移入这里（`src/adm_render_vbap` 随之由 421 行降到 257 行），私有 FFI 头 `routing_ffi.h` 增加了 Triple Balance 与 room 的声明（609 行）。`src/adm_dsp` 由 2,844 行降到 2,399 行。

**三平台。** 每次合入 `main` 后，Consistency 由 push 触发，在 macOS arm64、Linux x64、Windows x64 上采集 config A、B 的普通与诊断结果。

| 运行 | 提交 | 内容 | 结果 |
| --- | --- | --- | --- |
| [40099269643](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40099269643) | `6940df9` | A | 四份比较报告均为 283 个 PCM id 相同、0 个不同、平台集完整、门禁失败 0 |
| [40122405263](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40122405263) | `a072146` | B | 同上 |
| [40166924313](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40166924313) | `305159f` | C | 同上 |
| [40176869018](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40176869018) | `72c188b` | D | 同上 |
| [40186174718](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40186174718) | `ea07aa9` | E | 同上 |
