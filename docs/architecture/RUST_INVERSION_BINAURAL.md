# 双耳渲染迁入 Rust（四期 4.7）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)四期 4.7。
> 本切片是结构迁移：锁定矩阵的 PCM、内核文件、诊断检查点、日志与错误消息都要求与迁移前逐字节相同。
> 与旧实现不同的几处行为见[发布说明](#发布说明)，都不改变锁定矩阵的 PCM。
> B 合并时维护者的审查与性能问题见 [B 合并审查](#b-合并审查2026-10-27)，其中 BINAURAL-001 仍开放。

## 范围

双耳后端的 HRTF 状态与缓存、声源计划、逐源 OLA、spreader、工作池、离线驱动、文件流式渲染与 Scene 流式渲染器迁入 `rust/crates/mradm-render`（`#![forbid(unsafe_code)]`）。与 4.5、4.6 相同，Rust 后端只读策略后的 Rust 渲染场景（`RenderPlan::scene_handle`），不读 C++ `AdmScene`。数值内核原本就在 `mradm-dsp`（OLA 卷积、diffuse 延迟、HRTF 滤波与查询、网格、cloud、OM spreader、Scene 双耳 session、文件流式的逐源归约与拓扑淡化、`scene_math::Rotation`），本切片迁走的是它们外面的计划构建、块时间线、系数组装、样本归约与渲染循环。

| 批次 | 内容 | 原 C++ | Rust |
| --- | --- | --- | --- |
| A | 内置 KEMAR 与 SOFA 数据集的读取（打开、为空、读不全三条错误，维度与采样率检查，`SOFA <name>` 名字）；状态构建（尺寸检查、FFT 大小、overlap、滤波器组）与 `binaural.01–05` 检查点；几何缓存（方向位的 FNV 键，16 MiB / 8 项，合并并发的冷构建）；Scene 双耳的 HRTF 状态缓存（canonical 路径、mtime、size 与采样率为键，64 MiB / 4 项，入缓存时丢掉冲激响应）与 HRIR 重采样 | `binaural_renderer.cpp` 的数据集与状态构建，`hrtf_grid.cpp/.h`，`live_binaural_renderer.cpp` 的缓存与重采样 | `binaural_hrtf` |
| B | 声源计划：每个对象 3×17 个 divergence × extent 槽（direct 与 diffuse 两个 bank）、cloud 展开与 `cloud.01/02`、DirectSpeakers 的 25 项标签表与 LFE 旁路、HOA 警告、拓扑缩放、spreader 轨道与 lane 分组（FNV 种子，预算 8）；逐源 OLA bank（HRTF 缓存、头部转动与对象平滑的 crossfade、分段与静音）与实例自有的 rayon 工作池；spreader 适配器（512 帧 carry、32768 帧输出环、prime）；分步离线驱动（预滚、延迟环、`out_skip`、窗口 emit、计量、尾声）与 `hrtf-grid-*`、`spreader/*` 检查点 | `build_sources` 及其辅助函数、`build_spreader_tracks` / `groups`、`render_source_ola_block`、`TrackWorkerPool`、`render_window` 的循环，`binaural_spreader.cpp/.h` | `binaural`、`binaural_render` |
| C | 文件流式：块 FIFO、按离线窗口预滚的分步 seek、按 `object\0label` 键的 20 ms gain ramp、100 ms 合并的拓扑重建与 crossfade、seek 时采用待定拓扑、头部追踪与 live head_locked；bank 与工作池随流常驻、原地重建 | `BinauralStream`、`SourceBank`、`ola_preroll_start`，以及 B 的过渡 FFI `mradm_render_binaural_bank_*` | `binaural_stream::FileStream` |
| D | Scene 流式双耳：generation 配置与 fallback 方向、帧的暂存（initial、PCM、events）、DirectSpeakers 方向的取法（状态位置、25 项标签表与 fallback 警告、descriptor 位置）、duration 规则（jump、ramp、smoothing）、4 种警告、`.30-binaural-*` 检查点、朝向与 tail；只有 session 渲染成功才提交 target 与警告掩码 | `LiveBinauralRenderer` | `live_binaural` |

C++ 侧只剩适配：
- `binaural_renderer.cpp`（381 行）：能力表、路由模式与 LFE 路由检查、48 kHz 检查、写出、取消、进度与 “wrote … frames” 日志，以及文件流式的 `IRenderStream` 适配壳（分步 seek 之间检查取消，文字 “双耳文件流式跳转已取消”）；
- `hrtf_state.cpp/.h`（138 行）：Rust HRTF 状态的视图，持有滤波器组句柄；SOFA 路径在这里转成 UTF-8（维护者 `819d2e0`）；
- `live_binaural_renderer.cpp`（35 行）：经 `HrtfState::prepare_live` 取 HRTF 状态、创建 Rust 渲染器，再交给 `render_common` 的 `live_rust_renderer`（4.6 C 起两个 Scene 扬声器渲染器与 room 共用）。

`src/adm_render_binaural` 由 3,488 行降到 596 行。只剩测试与冻结参考使用的 C++ 移到 `tests/support/`：
- `tests/support/binaural/`：A 之前的 C++ 准备（`binaural_internal.h`、`legacy_state.cpp`、插值探针 `binaural_test_probe.*`），供冻结的 Scene 双耳参考与 HRTF 插值测试使用；以及 D 移出的 `binaural_dsp.h`、`live_binaural_convolver.h`、`live_binaural_ffi.h`。后者声明的 `mradm_dsp_live_binaural_*` 导出保留，`mradm-ffi` 的分配与原子性测试仍在用。
- `tests/support/head_rotation.h`：`osc_head_tracking_test` 与 `scene_numeric_rust_test` 使用。生产的头部转动用 `mradm-dsp` 的 `scene_math::Rotation::new_compat`。

`tests/reference/` 的冻结参考文件内容没有改动，retention 哈希不变。

### 保持不变的规则

- **归约顺序。** 声源与 spreader 组各自写入自己的缓冲区，再固定按源索引、组索引归约，所以工作池的线程数不影响输出。
- **消息与检查顺序。** 错误码、context、文字与检查顺序逐字保留；日志行顺序照旧：C++ 先做路由、LFE 与采样率检查并记 “HRTF source”，再转发 Rust 返回的准备日志。
- **检查点。** `binaural.01–05`、`hrtf-grid-*`、`cloud.01/02`、`spreader/s*-g*-lane*.*`、`.30-binaural-commands` / `-directions` 改由 Rust 经 `diagnostics::process` 写出，每个名字只归一方。并行 worker 写同名的 `hrtf-grid-*` 时先写者生效，内容只取决于网格下标，与 C++ 相同。
- **时间线。** 离线窗口的预滚（`preroll_start`，spreader 模式不 seek）、spreader 尾声按 `total_latency` 分块排空；文件流式的 seek 用同一预滚，保留目标所在块的后缀；拓扑编辑在第 0 帧直接生效，此后每 100 ms 最多应用一次。
- **原子性。** 文件流式的拓扑重建先准备好新工作区，成功后才重置历史、提交新的源图，失败时保留原 bank、视图与音频历史（维护者 `0f49c1b` 建立的契约，C 原样保留）；Scene 双耳的帧只有在 session 渲染成功后才提交 target、initialized 与警告掩码。

### 维护者决定

2026-10-26：
- 4.7 分 A–E 五批，每批单独提交，维护者本机全量对照通过后再开始下一批。
- 离线与文件流式分开：B 迁离线时，C++ 文件流式暂时经过渡 FFI `mradm_render_binaural_bank_*` 驱动 Rust 的逐源 OLA bank（做法同 4.6 D），C 删除这层过渡。
- B 合并时记录两项性能问题后照常合并（见 [B 合并审查](#b-合并审查2026-10-27)）。

## 发布说明

都不改变锁定矩阵的 PCM：
1. **工作池线程数。** 线程数改由 `std::thread::available_parallelism` 取得（考虑 cgroup 限额），原来是 C++ `hardware_concurrency`；`MR_ADM_DIAGNOSTIC_WORKERS` 覆盖照旧。输出与线程数无关，只有诊断构建的 “diagnostic pools: …” 日志可能不同。
2. **worker 中止改为返回错误。** 原先在 worker 线程里抛出、导致进程中止的路径（HRTF 查询参数非法、Voronoi 单元、OM 协方差、非有限卷积增益）现在返回同样的错误码与文字。原来进程以 SIGABRT 退出，现在渲染失败并报错；A 对照中发现的方位偏移 1e10 用例即属此类，B 的离线 CLI 对照中有 10 例。
3. **黄金数据中的 NaN。** 声源计划的黄金数据把 NaN 方向写为 `nan`：同一段 Rust 算术内联到不同位置时 NaN 的符号可能不同，而这类方向会被 HRTF 查询拒绝，不会到达 PCM。
4. **文件流式检查输出缓冲。** `process()` 的输出缓冲短于请求时返回 invalid_argument “Invalid stream output buffer”，C++ 原来不检查。
5. **Scene 双耳 session 创建失败。** 只有参数非法时才会发生；原来从构造函数抛出 `runtime_error`，现在返回错误（context “Live binaural DSP”）。
6. **删除的私有导出与头。** `mradm_render_binaural_bank_*` 与 `RenderBinauralInfo`（C），stream_dsp 已无 C++ 调用者的 `mradm_dsp_stream_accumulate` / `_topology_crossfade`、`mradm_dsp_extent_spread_degrees`、`mradm_dsp_monitor_stereo_fold`（C；内核测量直接调用 `mradm-dsp`，不受影响）；`src/adm_render_binaural/hrtf_grid.*`、`binaural_spreader.*`，以及 `src/adm_dsp/live_binaural.h`（D）。这些只在 `mradm-ffi` 与 C++ 之间使用，C ABI bundle 本来就不导出；C ABI 没有变化。
7. **维护者补充的修正。**
   - `819d2e0`：SOFA 的显示路径与文件名改为 UTF-8，不再经 `path.string()` 的 ANSI 代码页；
   - `0f49c1b`：修复 BINAURAL-002，文件流式全程持有同一个 bank 与工作池，拓扑重建原地复用卷积工作区（见[内存修复](RUST_BINAURAL_MEMORY_FIX.md)）；
   - `949e14e`：Rust 测试在 CTest 之外预构建，不再受 CTest 超时影响；
   - `a0f231d`：重命名一处拓扑表的键，避免 cppcheck 2.22 的 shadowFunction；
   - `2a444bc`：HRTF 缓存键的时间戳测试显式设置修改时间，避免快速写入共用同一文件系统时间戳。

## 黄金数据

| 语料 | 生成方 | 规模 | 结果 |
| --- | --- | --- | --- |
| `mradm-render/tests/binaural-v1/`（`cases.jsonl`、`expected.jsonl`、`samples.jsonl`） | main `c967d84` 的 C++ `build_sources`、`build_spreader_tracks`、`build_spreader_groups`，由 scratchpad 里的临时生成器（不提交）把 `binaural_renderer.cpp` 编译进自身后一次性采集 | 27 个场景（`scenes/`：13 个双耳专用 AXML 与 14 个 policy-v1 / 矩阵夹具，其中 `bin-extent-edge` 的 extent 恰为 1° 阈值及其下一个 ulp），每个场景在三种 spread 下运行：81 例无策略、2,100 例套用 policy-v1 的策略、270 例 live 拓扑缩放、60 例 spreader 分组预算，共 2,511 例；记录导入或策略错误、警告、计数与计划文本的 FNV-1a 64，另有 14 份完整计划文本 | Rust 全部一致 |
| `binaural-v1/renders.jsonl` | 同一提交的 CLI（`--renderer saf-binaural --no-peak-limit --output-bit-depth f32`） | 54 个渲染（全部 spread、平滑、窗口与听者朝向的变体），记录帧数与浮点输出的 FNV-1a 64 | Rust 全部一致；维护者两平台对照也核对了这 54 个指纹 |

手工构造或改写场景的 C++ 用例移到 Rust：`binaural_render_fixture_test` 的同起点部分与原 `same_start_blocks_test` 的双耳 live label 移到 `tests/binaural.rs`；`hrtf_grid_test` 中检查缓存内部结构的部分移到 `binaural_hrtf` 的模块测试。Rust 测试：`binaural_hrtf` 模块内 8 个、`tests/binaural.rs` 13 个、`tests/binaural_stream.rs` 9 个、`tests/live_binaural.rs` 6 个，以及 `mradm-ffi` `render_binaural_test.rs` 的 bank 不分配测试。

每批都植入缺陷，确认测试会报错：

| 批次 | 植入的缺陷 | 结果 |
| --- | --- | --- |
| A | 4 处（缓存与重采样） | 都被抓到 |
| B | 槽位公式、extent 阈值的两处比较（`>=` 改为 `>`）、延迟环写入位置、预滚向下取整、组内 lane 顺序 | 都被抓到（extent 阈值是补了恰在 1° 阈值的 `bin-extent-edge` 场景后才抓到的；写本记录时在 `2a444bc` 上重跑这 6 处，结果相同） |
| C | 8 处，包括拓扑间隔计算、slot 键漏掉标签、seek 时不采用待定拓扑与 seek 时不重启 gain ramp | 都被抓到（ramp 重启一处是补了 seek 到 0 的用例后抓到的） |
| D | jump 优先级、标签表一项、不置 fallback 标记、忽略状态位置、警告掩码不提交、提前提交暂存 | 都被抓到（最后一处是补了“首帧被 session 拒绝”的用例后抓到的） |

## 验证

**逐批检查（Linux x64，提交前）。**
- ctest 全部通过（B、C、D 各连跑 3 次，观察线程池时序问题；D 为 80/80）；
- `mr_adm_rust_unit_tests`、fmt 与 clippy `-D warnings`；
- `mr_adm_ffi_header_check`（D 之后为 344 个函数、141 个结构体）；
- 变更 C++ 的 clang-format 与 clang-tidy；
- `check-reference-retention.py`、`version_metadata.py --check`、`language-surface.py`、`check-licenses.sh`；
- B–D 显式构建 `tests/manual` 下的目标。

本机没有 cppcheck，由 CI 运行（`a0f231d` 即 CI 发现的一处）。

**新旧对照（scratchpad 工具，不提交）。** 基线是各批开始时的 `main`。离线 CLI 比较退出码、`-v` 日志与输出字节（bext 的墙钟日期时间置零）；文件流式用 `mr_adm_phase2_stream` 比较退出码、stderr 与两遍重放的 PCM 字节；矩阵子集用一致性采集的 binaural 与 SOFA 行比较 PCM 与检查点。

| 批次 | 基线 | 用例 | 结果 |
| --- | --- | --- | --- |
| A | `5dbdc3a` | `--renderer saf-binaural` 948 例（551 例渲染，三种 spread、SOFA、窗口、听者朝向、响度目标与 policy-v1 的全部策略）；矩阵中 22 个 binaural / SOFA / room 离线行与 6 个文件流式重放，普通与诊断；Scene 的 binaural / SOFA 重放，普通与诊断 | 只有一例两版都中止的用例 stderr 不同（方位偏移 1e10）；矩阵诊断 4,478 个检查点文件相同；Scene 普通 352、诊断 11,269 个文件相同 |
| B | `c967d84` | 离线 CLI 948 例（551 例渲染）；矩阵 22 个离线行与 6 个文件流式重放（经过渡 FFI），普通与诊断 | 938 例相同；其余 10 例基线以 SIGABRT 中止、候选报错退出（发布说明第 2 项）；矩阵 0 个问题 |
| C | `a0f231d` | `mr_adm_phase2_stream` 574 例（368 例渲染：矩阵夹具与 binaural-v1 场景，三种 spread，三种拉取分块，seek 目标、按标签的 gain、拓扑比例 0 / 0.5 / 4 与快速编辑、朝向）；矩阵子集普通与诊断；离线 CLI 948 例重跑；Scene 普通回归；完整控制重放的峰值 RSS | 全部相同；矩阵诊断 4,478 + 1,942 个检查点文件相同；Scene 704 个文件相同；峰值 RSS 两版都是 44.8 MiB |
| D | `f6c697e` | 全部 88 个 Scene 重放，普通与诊断；C++ `live_binaural_rust_test`（生产实现对照冻结参考）与连续性测试；矩阵子集 binaural 普通 | 普通 704 个文件相同；诊断 22,592 个文件中只有一个 VBAP 设备回放的 24 个设备回调 trace 切分不同，按样本顺序拼接后 36 条流全部相同；`.30-binaural-*` 728 个检查点相同；矩阵 0 个问题 |

**维护者本机验证。** 无诊断插桩的 Release 构建：
- B：macOS arm64 83 例、Windows x64 77 例与 `c967d84` 逐位相同，77 个共用输出跨平台相同（[B 合并审查](#b-合并审查2026-10-27)）；
- `0f49c1b`：Windows 77 项、macOS 83 项 PCM 对照通过，77 个共用输出相同（[内存修复](RUST_BINAURAL_MEMORY_FIX.md)）；
- C：对照 `a0f231d`，macOS 83/83、Windows 77/77 逐位相同，77 个共用输出跨平台相同；6 个文件流式重放的耗时与峰值内存都在 1.05 / 1.10 调查阈值内；
- D：macOS arm64 58 例、Windows x64 44 例 Scene 重放与 `f6c697e` 逐位相同，44 个共用用例跨平台相同；双耳参考与 Scene C ABI 回归测试通过。SOFA 只在 macOS 对照，Windows 保持规范构建的 SOFA 关闭。

**规模。** 生产 C/C++ 由 36,931 行降到 33,732 行（−3,199）：

| 批次 | 行数 |
| --- | --- |
| A | 36,931 → 36,467 |
| B | 36,467 → 34,958 |
| `0f49c1b` | 34,958 → 34,953 |
| C | 34,953 → 34,513 |
| D | 34,513 → 33,732 |

**三平台。** 每次合入 `main` 后，Consistency 由 push 触发，在 macOS arm64、Linux x64、Windows x64 上采集 config A、B 的普通与诊断结果。

| 运行 | 提交 | 内容 | 结果 |
| --- | --- | --- | --- |
| [40300248787](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40300248787) | `c967d84` | A | 四份比较报告均为 283 个 PCM id 相同、0 个不同、平台集完整、门禁失败 0 |
| [40484741776](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40484741776) | `c34f937` | B | 同上 |
| [40653088266](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40653088266) | `0f49c1b` | BINAURAL-002 修复 | 同上 |
| [40808829856](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40808829856) | `f6c697e` | C | 同上 |
| [40931990370](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40931990370) | `31bb6dc` | D | 同上 |

## B 合并审查（2026-10-27）

以下为 B 合并时维护者的记录，原文保留；其中“文件流式渲染仍由 C++ `BinauralStream` 编排”已由 C 改变，BINAURAL-002 已修复。

2026-10-27 对照基线 `c967d840` 与 Claude 候选 `1c52d999`。两平台已测 PCM 全部逐位相同，
Windows 性能回退仍开放。维护者要求记录问题后合并、推送；本次合并不表示性能验收完成。

同日后续[性能调查](RUST_BINAURAL_PERFORMANCE_REVIEW.md)确认拓扑重建时新旧卷积 bank 重叠是内存回退的原因，
并修订了 cloud 的计时依据：旧采集器的 2 ms 休眠实际约 15.55 ms，改用直接等待进程退出后，两轮无插桩
Release 对照为 +3.21% / +1.39%。内部调度与输出归约仍有额外成本；归因实验未作为正式修复，两项仍开放。

2026-10-28：BINAURAL-002 已通过持久 bank / 工作池与卷积工作区复用修复，两平台 Release 内存和 PCM
对照通过；BINAURAL-001 继续开放。范围、源码身份和数据见[内存修复](RUST_BINAURAL_MEMORY_FIX.md)。

### 迁移范围

`mradm-render::binaural` 持有声源规划、divergence × extent 槽、DirectSpeakers 标签与 LFE 旁路、
spreader 轨道和分组；`binaural_render` 持有逐源 OLA、spreader 适配器、工作池、离线窗口、
延迟补偿、计量与尾声。C++ 保留路由与采样率检查、写出、取消、进度和日志。
文件流式渲染仍由 C++ `BinauralStream` 编排，通过过渡 FFI 驱动 Rust OLA bank。
本次没有执行 Scene 流式渲染对照或设备欠载验收。

### 正确性对照

全部音频对照使用关闭诊断插桩的 Release 构建，固定内置 KEMAR、48 kHz，复用原有构建目录及
Cargo / FetchContent 缓存。Windows 使用标准 MSVC/Ninja 配置，SOFA 关闭；macOS 另测六个 SOFA 用例。

| 平台 | 离线用例 | 冻结 PCM golden | 文件流式重放 | 结果 |
| --- | ---: | ---: | ---: | --- |
| macOS arm64 | 19 | 54 | 10 | 83/83 与基线逐位相同 |
| Windows x64 | 13 | 54 | 10 | 77/77 与基线逐位相同 |
| 两平台共用候选输出 | 13 | 54 | 10 | 77/77 跨平台逐位相同 |

离线用例包括点声源、cloud、spreader、姿态、同起点元数据、平滑、窗口、语义策略、整数 WAVE
和 CAF 后处理。54 个 golden 同时核对基线与原冻结 FNV-1a；文件流式重放覆盖固定／碎片拉取、
增益、静音、拓扑编辑、头追、seek、床声道标签和参数夹具，每例还检查进程内两次重放一致。
补齐原先遗漏的 DirectSpeakers 输入后续跑剩余四例，已完成的数值行保留，过程记录在 JSON 中。

此前审查还通过了 macOS Debug 双耳 C++ fixture、12 项 Rust 双耳测试（含 2,511 个规划样例），
以及 344 个函数、143 个结构体的私有 FFI 一致性检查。Windows 两版 Release 双耳 fixture 均通过。

### 开放问题

以下是正常桌面环境中的端到端进程测量，未调整日常应用、后台服务、亲和性或供电设置。
Windows 为 i7-11800H、16 个逻辑核心、交流供电。素材长 1 秒；耗时包括启动、准备、渲染、
计量与输出，文件流式工具每次运行在同一进程内重放两遍。峰值内存使用
`GetProcessMemoryInfo` 的 peak working set，处理期间每 2 ms 采样。

#### BINAURAL-001：Windows cloud 离线端到端耗时回退（P2，开放）

- 条件：`objects-extent`，`saf-binaural`、`cloud`、f32、关闭峰值限制。
- 预热后交替测量 5 轮，中位数由 132.54 ms 增至 149.04 ms（+12.45%）。
- 同条件交替复测 9 轮，中位数由 135.08 ms 增至 148.62 ms（+10.02%）；峰值内存约相同。
- PCM 在所有轮次保持一致。回退根因尚未确认；后续需区分准备、工作池调度和渲染成本，
  在 Windows 修复并复测后再继续适用的 macOS 性能矩阵。

#### BINAURAL-002：Windows 文件流式拓扑编辑峰值内存回退（P2，已修复）

当前状态：2026-10-28 已修复并通过两平台验证，以下为合并时的历史失败记录。

- 条件：`objects-extent-multi`，cloud、512 帧拉取，重放包含增益、extent／diffuse／divergence
  比例、头追和 seek 的 `stream-binaural-cloud-fixed` 事件。
- 初测 5 轮峰值中位数由 43.13 MiB 增至 52.23 MiB（+21.11%）。
- 复测 9 轮由 43.11 MiB 增至 52.18 MiB（+21.04%）；端到端耗时约相同（-1.18%）。
- 隔离测量 7 轮：静态重放为 42.98 → 42.45 MiB（-1.23%）；保留增益／头追／seek、移除
  拓扑编辑后为 43.02 → 43.15 MiB（+0.30%）。增长与拓扑重建相关，尚未确认具体分配来源。
- 同一隔离测量中，增益／头追／seek 重放耗时为 227.09 → 245.30 ms（+8.02%）；该耗时
  观察尚未单独复测，也继续保留。静态重放耗时变化为 +0.86%。

两项已复测回退分别超过仓库的耗时 1.05、RSS 1.10 调查阈值。阈值不是共享 CI 的硬性门禁；
维护者本次选择带问题记录合并，问题状态仍为开放。依照 Windows 优先的性能顺序，
本轮未继续 macOS 性能矩阵或长测；两平台 PCM 相同不代替这些未完成的验收。

### 证据

原始轮次、请求参数、PCM 与二进制 SHA-256、构建配置及工具链身份保存在：

- [汇总](evidence/binaural-render-20261027/summary.json)
- [Windows 初测](evidence/binaural-render-20261027/windows-short.json)
- [Windows 9 轮复测](evidence/binaural-render-20261027/windows-recheck.json)
- [Windows 文件流式隔离](evidence/binaural-render-20261027/windows-stream-isolation.json)
- [macOS PCM](evidence/binaural-render-20261027/macos-pcm.json)
- [Windows PCM](evidence/binaural-render-20261027/windows-pcm.json)
- [构建与夹具指纹](evidence/binaural-render-20261027/provenance.json)
