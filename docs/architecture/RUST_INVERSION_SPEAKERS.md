# 扬声器后端与公共渲染层迁入 Rust（四期 4.5）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)四期 4.5。
> 本切片是结构迁移：锁定矩阵的 PCM、内核文件、诊断检查点、日志与错误消息都要求与迁移前逐字节相同。
> 与旧实现不同的几处行为见[发布说明](#发布说明)，都不改变锁定矩阵的 PCM。

## 范围

扬声器后端（VBAP、HOA、EAR）、Scene 流式的 VBAP 渲染器，以及它们共用的 DirectSpeakers / 标签 / LFE 路由、对象块预处理、离线驱动与文件流式渲染迁入新 crate `rust/crates/mradm-render`（`#![forbid(unsafe_code)]`）。Rust 后端只读取策略后的 Rust 渲染场景（`RenderPlan::scene_handle`），不读 C++ `AdmScene`。

| 内容 | 原 C++ | Rust（`mradm-render`） |
| --- | --- | --- |
| DirectSpeakers 标签规范化与别名、目标索引与位置回退、DS 矩阵按布局解析与按块路由、LFE 判定与 22.2 LFE 路由、实时覆盖的增益与 head-locked 解析 | `src/adm_render_common/render_common.cpp` | `routing` |
| 生效块（同起点只留最后一块）、positionOffset、channelLock、divergence、LFE 路由检查 | `render_common.cpp` 的对象预处理与各后端的同类代码 | `scene` |
| 离线渲染循环：读入、混音、预滚、`speaker/s*.10-input` / `.40-mix` 检查点、后台计量 | `src/adm_render_common/speaker_pcm.cpp` | `offline`（分步驱动） |
| 文件流式渲染与实时覆盖 | VBAP / HOA / EAR 各自的 `IRenderStream` | `stream` |
| VBAP 增益表：VBAP / MDAP、2D 高度警告、label / position / matrix 路由、`vbap.01–03` 检查点 | `src/adm_render_vbap/vbap_renderer.cpp` | `vbap` |
| HOA 编码计划：块分组、标签方向回退、逐声道检查与重绑定、`hoa.01–04` 检查点；HOA 计量（7.1.4 AllRAD 解码，LFE 另计 True Peak） | `src/adm_render_hoa/hoa_renderer.cpp` | `hoa` |
| EAR 布局（标准、4+5+4 与 9.1.6、wav71、Apple 几何）、Objects / DirectSpeakers / HOA pack 的 f64 增益、diffuse 总线与去相关、`ear.01–03` 与 `ear.hoa-decode-row-*` 检查点 | `src/adm_render_ear/ear_renderer.cpp` | `ear` |
| Scene 流式 VBAP：元素生成、逐帧暂存与原子提交、Objects / DS / LFE 增益、warn_once、`.30-vbap*` 检查点 | `src/adm_render_vbap/live_vbap_renderer.cpp` | `live_vbap` |

数值内核原本就在 Rust（`mradm-dsp` 的 `vbap::Panner`、`pcm_mix`、`hoa`、`ear_post`、`live_vbap::Mixer`、计量与 `scene_math`，`mradm-ear`），本切片迁走的是它们外面的计划构建、块时间线、系数组装、渲染循环与流式胶水。

离线驱动是分步的（`mradm_render_offline_open` / `next` / `commit` / `finish`）：C++ 驱动循环，负责写出、取消、进度、日志以及两步之间的有状态处理器（Triple Balance）。没有用回调，原因有两个：FFI 头校验不支持函数指针，C++ 回调抛出的异常也不能穿过 Rust 栈帧。内核按枚举分派（`offline::Kernel::{Mix, Hoa, Ear}`），预滚与日志、报错文字按内核区分，与原来各后端的循环一致。

C++ 侧只剩适配：
- 各后端的 `IRenderer` / `ILiveSceneRenderer` 适配壳与能力表：`vbap_renderer.cpp`（216 行，含自定义布局注册）、`hoa_renderer.cpp`（111 行）、`ear_renderer.cpp`（115 行）、`live_vbap_renderer.cpp`（193 行）；
- `speaker_pcm.cpp`：驱动循环、写出、取消、进度与日志，以及共用的 `RustPrepared` / `FileStream` 包装；
- `render_common` 中仍有调用方的薄包装，调用方是 Apple 岛、Triple Balance、room 与双耳（4.6、4.7）；
- 新的私有 FFI 头 `src/adm_render_common/routing_ffi.h`（384 行），已加入 `check-ffi-headers.py`。

B 让 Rust 场景句柄贯通所有渲染入口：`RenderService::prepare_preview_scene` 返回带句柄的 `PreparedScene`，`PreviewSession`、监听与一致性重放工具构建的 `RenderPlan` 都带 `scene_handle`，句柄的 C++ 定义移到 `src/adm_render_common/scene_handle.h`。此前预导入场景的句柄为空。

只剩测试使用的私有头移到 `tests/unit/`：`hoa.h`、`hoa_ffi.h`（`hoa_rust_test`）、`ear.h`、`ear_ffi.h`、`ear_post.h`（`ear_post_test` 与 libear 参考测试）、`live_vbap.h`、`live_vbap_ffi.h`（`live_vbap_rust_test`）。对应的 `mradm_dsp_*` 导出保留。`src/adm_dsp/dsp.h` 的 `Panner` / `panner_for` 在生产中已无调用者，但冻结的 `tests/reference/live_vbap/legacy.h` 与 `saf_reference_test` 经 `dsp.h` 使用它们，改参考会改变登记的哈希，所以保留到参考退役。

### 保持不变的规则

- **消息。** 检查顺序、错误码、context（`layout=`、`input=`、`Live VBAP DSP` 等）与文字逐字保留；警告的条数、顺序与“每次渲染一次”的规则不变。
- **数值。** 增益乘积、求和顺序与精度不变：EAR 全程 f64，divergence 各源按源顺序相加，HOA 的 LFE 只进 W。
- **时间线。** 同起点块只保留文档顺序中的最后一块；VBAP 离线窗口按块对齐，HOA 与 EAR 在窗口起点之前多渲染一块（只有 HOA 在起点重置编码器）；计量按块顺序累加。
- **检查点。** 迁入 Rust 的代码经 `mradm-dsp` `diagnostics::process` 写出同名同格式的检查点：`speaker/s*.10-input` / `.40-mix`、`vbap.01–03`、`hoa.01–04`、`ear.01–03`、`ear.hoa-decode-row-*`、`.30-vbap*-commands` / `-levels` / `-coefficients`。C++ 不再写这些名字，每个名字只归一方。

### 维护者决定

2026-10-23：
- A–D（路由、场景句柄、离线驱动、VBAP）合为一批提交，使棘轮在同一提交下降（新私有 FFI 头本身占行数）。
- Rust 后端只读场景句柄。手工构造或改写 `AdmScene` 的 C++ 测试，迁移对应后端时改为导入取句柄，或移到 Rust 测试。
- 离线驱动用分步模式，不用回调。
- 每批推送后由维护者本机全量对照，通过后才开始下一批。

## 发布说明

都不改变锁定矩阵的 PCM：
1. **EAR 的超长窗口。** 窗口长度接近 u64 上限时，旧 EAR 计算窗口终点会整数溢出，结果什么都不渲染；现在与 VBAP、HOA 一样截到文件末尾。CLI 的窗口参数碰不到这种情况。维护者另补了 `5efc517`：完成日志里的窗口终点同样先钳到剩余帧数。
2. **EAR 文件流式的内部错误。** 旧 C++ 的异常会从 `process` 抛出，没有被捕获；现在作为错误返回。
3. **Scene VBAP 的 panner 缓存。** 原来是 C++ 进程级缓存（最多 32 项），现在每个渲染器第一次需要时创建并保留。数值相同。
4. **Scene VBAP 的缓冲区检查（`a992c13`）。** 帧记录与 PCM 缓冲区重叠时拒绝该帧，错误文字与 context 与原来的 invalid-argument 相同；失败帧不留下上一帧的诊断。
5. **实时覆盖（`e13a01a`）。** 覆盖查询借用记录、不分配内存，双耳文件流式一并改用（`routing::try_live_override`、C++ `LiveOverrideView`）。
6. **测试构建。** `direct_speakers_matrix_render_test` 只剩 Apple 部分，改为只在 macOS 构建，Linux ctest 少一项。

## 黄金数据

| 语料 | 生成方 | 规模 | 结果 |
| --- | --- | --- | --- |
| `mradm-render/tests/routing-v1/` | 4.5 A 之前的 C++ `render_common` 规则，经 scratchpad 临时程序生成（不提交） | 4,550 条：标签键、标准布局与合成表上的目标索引、标签位置、LFE 目标、DS 矩阵与按块路由、实时覆盖、22.2 LFE 路由及其日志 | Rust 与改为薄包装后的 C++ 都逐字节一致；植入的 3 处缺陷被抓到 |

后端本身没有另做黄金数据，由锁定矩阵、诊断检查点和下面的新旧 CLI / Scene 对照验收。手工构造场景的 C++ 用例移到 Rust 后，`tests/vbap.rs`（5 个）、`tests/hoa.rs`（8 个）、`tests/ear.rs`（10 个）、`tests/live_vbap.rs`（13 个，含 `a992c13` 补的回归）覆盖导入走不到的情形，例如没有位置的 DS 块、提前结束的输入、重叠缓冲区。每批都植入 2–3 处缺陷，确认测试会报错：

| 批次 | 植入的缺陷 | 结果 |
| --- | --- | --- |
| A–D | 路由规则 3 处；VBAP 2 处 | 都被抓到 |
| E | binding 不重置、预滚向下取整 | 都被抓到 |
| F | 预滚向下取整、DS 不乘对象增益 | 都被抓到；LFE2 标签误映射为 LFE1 对输出没有影响（22.2 上的 LFE 块总先走 22.2 路由，其他布局只有一个 LFE），无法观测 |
| G | 警告键带上元素、LFE 不落到第一个、ramp 优先于 jump | 都被抓到 |

## 验证

**逐批检查（Linux x64，提交前）。**
- Release ctest 全部通过（83 → 82 个测试，见发布说明第 6 项）；
- `mr_adm_rust_unit_tests`、fmt 与 clippy `-D warnings`；
- `mr_adm_ffi_header_check`（G 之后为 322 个函数、136 个结构体）；
- 变更 C++ 的 clang-format 与 clang-tidy；
- `check-reference-retention.py`、`version_metadata.py --check`、`language-surface.py`；F 新增 `mradm-ear` 依赖，另跑了 `check-licenses.sh`。

`format.sh --check` 唯一的报错是 main 上原有的 `binaural_render_fixture_test.cpp`。本机没有 cppcheck，由 CI 运行。

**新旧对照（scratchpad 工具，不提交）。** 基线是各批开始时的 `main`，比较退出码、`-v` 日志与输出字节（只把 bext 的墙钟日期时间置零）：

| 批次 | 基线 | 用例 | 结果 |
| --- | --- | --- | --- |
| A–D | `340ef59` | `--renderer saf` 1,148 例：policy-v1 的 14 个场景与全部策略，轮换布局、DS 路由、几何、spread、LFE、平滑与窗口 | 全部相同 |
| E | `16e14dc` | `--renderer hoa` 886 例（689 例渲染成功），含 label / position 路由与其他布局的拒绝用例；VBAP 的 1,148 例重跑 | 全部相同 |
| F | `08350d9` | `--renderer ear` 2,096 例（1,128 例渲染成功），布局含 wav71、4+5+4、9.1.6、22.2，两种几何，auto / matrix / 被拒绝的路由，加 5.1、7.1.4、22.2 声道床输入；HOA 与 VBAP 重跑 | 全部相同；未渲染的都是两边一致的预期拒绝 |
| G | `dd3f82b` | 全部 88 个 phase2 Scene 重放（`mr_adm_phase2_scene`），普通构建与诊断构建各一次 | 普通构建 704 个文件（PCM、回放状态、事件清单）相同；诊断构建按采集器的方式拼接设备 trace（`canonicalize_device_trace`）后 21,756 个检查点文件相同，其中 534 个 `.30-vbap*` |

G 的诊断构建在拼接之前有 30 个文件不同，都在 3 个 fragmented 设备回放的 `device/` trace 下（其中一个是双耳用例）：这类 trace 按设备回调分段，采集器本来就要先拼接再比较。

**本机全量对照（维护者，每批合入前）。** 每批推送后，由维护者在本机对照候选与基线（普通与诊断各一次），通过后合入：
- A–D（`16e14dc`）：macOS Debug 与 Windows 规范 Release 构建 ctest 83/83，Rust clippy、FFI 头校验、变更文件的 C++ 分析与分配回归探针；
- E（`08350d9`）、F（`dd3f82b`）：本机全量对照通过；
- G（`970b706`）：Release Scene 重放 macOS arm64 88 例、Windows x64 74 例，含诊断与重复性；PCM、状态、事件与检查点都相同，两平台共有的 74 例跨平台也相同。

**维护者补充的修正。**
- `e13a01a`：实时覆盖查询借用记录、不分配内存；
- `aff8ec8`：cppcheck 识别跨编译单元的语义报告字段；
- `5efc517`：窗口完成日志防溢出，`render_speaker_pcm` 改为接收 `const SpeakerMix&`；
- `a992c13`：Scene VBAP 拒绝重叠缓冲区，失败帧清空诊断；
- `763d637`：清理测试中失效的 Backend 元数据，Scene VBAP 的帧记录改用 `ranges::transform`。

**规模。** 生产 C/C++ 由 42,406 行降到 39,747 行（−2,659）：

| 批次 | 行数 |
| --- | --- |
| A–D | 42,406 → 42,108，`e13a01a` 后为 42,107 |
| E | 42,107 → 41,306，`5efc517` 后为 41,305 |
| F | 41,305 → 40,109 |
| G | 40,109 → 39,748，`763d637` 后为 39,747 |

`src/adm_render_vbap` / `ear` / `hoa` 由 1,236 / 1,039 / 770 行降到 421 / 115 / 111 行，`src/adm_dsp` 由 3,475 行降到 2,844 行。`src/adm_render_common` 为 2,979 行，比 4.1 基线的 2,777 行多：逻辑已迁出，但新增了 384 行的私有 FFI 头，Apple、Triple Balance、room 与双耳用到的薄包装也要留到 4.6、4.7。

**三平台。** 每次合入 `main` 后，Consistency 由 push 触发，在 macOS arm64、Linux x64、Windows x64 上采集 config A、B 的普通与诊断结果。

| 运行 | 提交 | 内容 | 结果 |
| --- | --- | --- | --- |
| [39933972829](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/39933972829) | `16e14dc` | A–D | 四份比较报告均为 283 个 PCM id 相同、0 个不同、平台集完整、门禁失败 0 |
| [39959813760](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/39959813760) | `08350d9` | E | 同上 |
| [39987342040](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/39987342040) | `dd3f82b` | F | 同上 |
| [40004502957](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/40004502957) | `763d637` | G 与维护者修正 | 同上 |

`970b706` 的运行 40001144880 被后续提交取消，由 40004502957 覆盖。
