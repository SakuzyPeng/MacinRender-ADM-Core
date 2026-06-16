# 实时空间监听引擎设计

> 状态：草案
> 日期：2026-06-16
> 目标：记录把当前批处理（offline）渲染核心扩展为「实时空间监听引擎」的方向、接口边界与落地顺序。监听引擎让用户在**播放过程中**实时改听感语义参数、并在不同渲染后端 / 监听格式之间热切换、立即听到差异——类似 Logic Pro 的 Atmos 监听段。GUI 是它的遥控器，能力在 core。

## 1. 背景与诉求

语义编辑 GUI（见 `SEMANTIC_EDITOR_GUI.md`）一期落地后，验证手段曾用「应用 policy 渲极短窗口取语义报告 → 数值 diff」。实测下来该 diff **对用户无意义**：用户要的不是「数值变了」的确认，而是**实时监听**——边调边听。

进一步澄清后，诉求并非「接个音频播放库播一段离线渲染结果」，而是一个**实时能力**：

- 播放进行中，拖动 gain / diffuse / extent / divergence，立即（或松手即）听到差异；
- 播放进行中，在渲染后端之间热切换（EAR / VBAP / HOA / binaural / Apple），听不同渲染器的差异；
- 切换监听格式（双耳 / 5.1 / 7.1.4 …）对比。

这是 core 的实时引擎工程，不是 GUI 功能。本文档审计现状、定接口、拆切片。

### 1.1 已确认的范围决策

- **后端范围（最终）**：五个后端（EAR / VBAP / HOA / binaural / Apple）都要可热切换。但执行走垂直切片，不一次铺开（见 §9）。
- **听感维度（一期）**：gain / diffuse / extent / divergence，沿用语义编辑 GUI 一期范围；不做 position / channelLock / interpolation。
- **监听输出模型**：**未决**，但**不是后端重构的前置**，仅 §9 切片 6（热切换 + 监听 / 下混层）依赖它（见 §8）。

## 2. 现状审计

### 2.1 `IRenderer` 已经做了对的拆分

接口（`include/adm/render.h:124`）当前是两段：

- `prepare(plan) → IPreparedRender`（`render.h:133`）：**一次性**算好可复用、不可变的重料——HRTF 表 + VBAP 栅格、源列表 / bus 列表 / 增益矩阵、解码器。明确约定「不得携带每输出（per-output）或每窗口（per-window）可变状态」（`render.h:104-108`）。
- `render_window(prepared, plan) → RenderMetrics`（`render.h:138`）：用 `prepared` 渲一个输出窗口。

`PreviewSession`（`src/adm_engine/render_service.cpp:1291`）已经在跨窗口复用同一个 `prepared`（scrubbing）。也就是说「可复用 vs 每输出」这个最难的概念分离**已经做完了**。

### 2.2 跨块连续所需的状态对象已经存在

实时流式渲染的核心难点是「块 N+1 能接上块 N 的 DSP 尾巴」。binaural 里这些状态类型**已经存在**，只是今天活在一次 `render_window` 调用内、每次重建：

- `OLAState`（`binaural_renderer.cpp:640`）——overlap-add 卷积的重叠尾；
- `DiffuseDelayState`（`binaural_renderer.cpp:654`）——去相关延迟线；
- `BinauralSpreaderAdapter`（per-output STFT 状态，`render_window` 内 `prime()` 后逐块 `process_chunk`，`binaural_renderer.cpp:1404-1419`）。

`render_window` 的主体本身就是一个块循环（`binaural_renderer.cpp:1576`）：

```
while (frames_done < num_frames && out_abs < win_end) {
    reader->read(in_block, frames_now);     // 从文件拉一块输入
    …按 source / spreader group 混音，OLA 卷积…
    emit(l_buf, r_buf, fn);                 // writer.write 写文件
}
```

**循环体就等价于 `process(out, n_frames)`**。把它抽出来、把上面的状态对象从「窗口局部」提升为「会话持久」，就是流式化的主要工作——是**重构搬迁**，不是从零写 DSP。

### 2.3 Apple 后端已经是逐块、参数可活改

`src/adm_apple/spatial_mixer_renderer.cpp` 包的是 `kAudioUnitSubType_SpatialMixer`（AUSpatialMixer，`:128`），`render_window` 主循环按 ~10 ms slice（`k_render_block`）逐 bus `AudioUnitSetParameter`（azimuth/elevation/gain，`:454-463`）后 `AudioUnitRender` 拉输出（`:660`）。

AUSpatialMixer 本就是**实时 AudioUnit**，参数本就该活改。Apple 后端因此是**最省力的实时切入点**：渲染输出从 file writer 换成调用方 PCM/ring，输入仍可由 worker 从 file reader 读到 AU staging buffer（需要时再加缓存），参数从烤死的 `buses[].events` 换成活覆盖层即可。

### 2.4 现状的两个根本限制

1. **输出是文件**：binaural（`binaural_renderer.cpp:1424`）、apple（`:638`）都 `audio::WriterHandle::open(...)` 写文件，并内联 ebur128 计量。实时要写进调用方提供的 PCM 缓冲 / 环形缓冲，实时路径不计量。
2. **语义参数在 `prepare()`（及更早的 `apply_semantic_policy`）阶段就烤死**：`PreviewSession::create` → `prepare_preview_scene`（`render_service.cpp:1271`）在 import 后一次性 `apply_semantic_policy` 改写 `AdmScene` 块值；随后 `prepare()` 把这些值进一步展平成源 / bus / 增益矩阵。改一个参数当前必须销毁会话重建（重 import + 重 apply + 重 prepare）。

### 2.5 各后端规模（流式化工作量参考）

| 后端 | 行数 | 循环结构 | 流式化难度 |
|---|---|---|---|
| Apple | 960 | 已 slice 化，参数原生可活改 | **小**（最快出声） |
| VBAP | 866 | 块循环 + 增益矩阵 | 中 |
| HOA | 936 | 块循环 + SH 编码 | 中 |
| EAR | 1051 | 块循环 + libear 增益 | 中 |
| binaural | 1933 | 块循环 + OLA + spreader + diffuse + 线程池 | **大**（最绕，验证契约用） |

## 3. 四个缺口

以 binaural `render_window` 为基准，实时路径缺：

| 缺口 | 现状 | 实时所需 |
|---|---|---|
| ① 输出到 buffer / 回调 | `WriterHandle::open` + `writer.write()` 写文件 + 内联 ebur128 | 写进调用方 PCM 缓冲；实时路径不计量 |
| ② 持久会话状态 | `OLAState` / adapter / reader / cursor 都是 `render_window` 局部，每次重建 | 提升为会话对象，跨 `process()` 调用连续 |
| ③ 块级 pull API | 循环体内联 | 抽成 `process(out, n_frames)` + `seek(frame)` |
| ④ 实时安全 | 用 `TrackWorkerPool` + bw64 磁盘读 | 见 §4：**不需要把 DSP 改成无锁** |

## 4. 实时安全策略：worker 跑在播放头前面 + 环形缓冲

最吓人的需求是「让 libear / SAF / 线程池满足硬实时（无锁、无分配、无系统调用）」。**我们绕开它**：

- 渲染器跑在**播放头前面的 worker 线程**，`process()` 出来的 PCM 填进**环形缓冲（ring buffer）**；
- 音频设备回调**只从 ring 抽数据**，回调侧零分配、零锁（单生产者单消费者 ring 即可）；
- 磁盘读、`TrackWorkerPool`、内存分配全留在 worker 侧。

代价是固定一点缓冲延迟（几十毫秒级），对监听完全可接受。**收益是 §2.5 的批处理 DSP 几乎原样搬进 worker，无需 DSP 级实时改造。**

输入 PCM 也走 worker：`bw64` reader 在 worker 线程按 `seek` + `read` 供数据；长素材可流式读，无需整轨入内存（循环 region 命中的范围可缓存）。

## 5. 设计岔路：四个维度不是同一种「实时」

这是动手前必须共识的一点：

- **gain（对象级 + 块级）**：binaural 里就是 `src.gain` / `block_gain` 一个标量乘（卷积时乘），Apple 里是 `kSpatialMixerParam_Gain` 每 bus 一个参数。**拖滑块下一块即生效，是真·实时**，改动小。前提是新的 prepared/stream 描述要保留 `object_id → source/bus` 映射；当前离线 prepared 只保留渲染所需源/总线，不够 GUI 覆盖层按对象寻址。
- **diffuse / extent / divergence**：这三个在 `prepare()` / `build_sources` 阶段**改变了源的拓扑**——
  - diffuse 把一个源拆成 direct + diffuse 两条，`sqrt(1-d)` / `sqrt(d)` 缩放（`binaural_renderer.cpp:1087-1089`）；
  - extent 展成 17 点 disk cloud（`expand_binaural_extent`，`:994`）；
  - divergence 展成 3 槽（`k_binaural_divergence_slots`）。

  改它们 ≠ 改一个运行时标量，而是要重跑源列表构建。现实的「实时」是：**编辑时做一次廉价 re-prepare**——HRTF / VBAP 表保留不动，只重建源列表（`build_sources`），再 `seek` 回当前播放头——**接近实时、松手即更新、瞬间一个小接缝**。这不是直接重跑今天的整个 `prepare()`；需要把 prepared 内部继续拆成「重型 cache」与「可重建 source/bus plan」两层。

**一期接受这个区别**：gain 给 DAW 般即时手感；diffuse / extent / divergence 是「松手即更新」。要做到后三者拖动也连续，得把拓扑按最大 extent 预建、权重活调，那是另一个量级，**不在一期**。

## 6. 接口草案

保持 ADR 0003：接口里不出现任何第三方类型，只有 `AdmScene` / `std::span` / 标准库。

```cpp
// 持久流式渲染会话：每后端把 render_window 的循环体 + 每输出状态搬进来。
class IRenderStream {
  public:
    virtual ~IRenderStream() = default;
    IRenderStream(const IRenderStream&) = delete;
    IRenderStream& operator=(const IRenderStream&) = delete;

    // 渲 frames 帧交织 PCM 进 out（out.size() >= frames * out_channels()）；
    // 返回实际渲出的帧数（到达素材尾或循环边界可少于请求）。
    // 携带 OLA / STFT / cursor 跨调用连续。跑在 worker 线程，允许分配 / 线程池。
    // 调用方可请求任意 frames；实现内部必须用自己的规范块长 + FIFO 保持与离线 render_window 一致。
    [[nodiscard]] virtual Result<std::size_t> process(std::span<float> out, std::size_t frames) = 0;

    // 跳到绝对输出帧（编辑 diffuse/extent/divergence 后 reseek，或循环 region 回卷）。
    // 必须重置/预热后端状态，使 seek 后输出等价于离线 window render 的同一位置。
    [[nodiscard]] virtual Result<void> seek(uint64_t frame) = 0;

    [[nodiscard]] virtual uint32_t out_channels() const = 0;
    [[nodiscard]] virtual uint32_t sample_rate() const = 0;
    [[nodiscard]] virtual std::string_view output_layout() const = 0;

  protected:
    IRenderStream() = default;
};

class IRenderer {
  public:
    // ...现有 capabilities() / prepare() / render_window()...

    // 打开一个持久流式会话；prepared 必须来自本 renderer 的 prepare()。
    // 为了逐后端落地，这里不能先做 pure virtual；默认实现应返回 unsupported，
    // 已流式化的后端逐个 override。
    virtual Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prepared, const RenderPlan& plan, LogSink& logs);
};
```

活覆盖层——渲染时**逐块读**，可无锁热替换（double-buffer + atomic 指针交换；worker 在块边界读当前快照）。它语义接近 `mradm.semantic-policy.v1` 的对象级相对覆盖，但刻意是实时子集；若以后要支持 `global` / `name` / `track_uid` / direct-speaker block 规则，应先在引擎边界归一化成对象 ID 快照，或直接复用 policy JSON 作为 ABI 入参：

```cpp
// 每对象的相对覆盖（policy 的实时归一化子集，面向逐块求值）。
struct LiveOverride {
    std::string object_id;
    float gain_db{0.0F};      // 即时：下一块生效
    float diffuse_scale{1.0F}; // 触发 stream 重建（廉价 re-prepare + reseek）
    float extent_scale{1.0F};  // 同上
    float divergence_scale{1.0F};
};
struct LiveOverrides {
    std::vector<LiveOverride> objects;
    uint64_t revision{0};      // 递增；worker 比对 revision 决定是否 re-prepare
};
```

引擎侧（建议新模块 `ADMRealtime`，PRIVATE 依赖各 renderer + 设备层；CLI/GUI 经 C ABI 接入）：

- **MonitorEngine**：持有当前 `IRenderStream`、ring buffer、播放时钟、loop region、`LiveOverrides` 的原子快照；worker 线程 `process()` 填 ring，比对 `revision` 决定 gain 即时 / 重建 stream。
- **设备层**：音频输出回调抽 ring。跨平台用 miniaudio（经 P/Invoke 或直接 C++）或平台原生（macOS CoreAudio / AVAudioEngine）——见 §8 决策。
- **热切换**：见 §7。

C ABI（ADR 0007，additive，新 minor）：`adm_monitor_t` opaque + `adm_create_monitor` / `adm_monitor_play` / `adm_monitor_pause` / `adm_monitor_seek` / `adm_monitor_set_loop` / `adm_monitor_set_overrides`（JSON 或 C POD 数组，内部转 `LiveOverrides`；不能暴露 `std::vector` / `std::string`）/ `adm_monitor_switch_backend` / `adm_destroy_monitor`。结构化 progress / level meter 回调另议。

## 7. 热切换渲染后端

因为每后端是 `prepare()` + `IRenderStream`，热切换 = **预热新后端的 stream**（worker 上，pre-warm `prepare()` + `open_stream` + `seek` 到当前播放头）→ 在块边界把 ring-filler 拉取的 stream 切过去 → 短交叉淡化（两个 stream 并行跑 ~10–50 ms，线性 crossfade 避免爆音）。如果两个 stream 的输出布局不同，crossfade 应发生在监听 / 下混后的物理设备格式上，而不是直接混原始 stream。

声道数变化（binaural 2ch → 7.1.4 12ch）由 §8 的监听 / 下混层吸收，**不**让上层感知物理声道变动。

## 8. 监听输出模型（未决，gated 切片 6）

绕不开的物理事实：用户只能听到设备放得出的东西。耳机上「7.1.4 监听」本质是被双耳化的。三个候选（动设备 / 热切换层前必须敲定）：

1. **始终经物理设备，声道数不匹配就下混 / 双耳化**（最贴 Logic：耳机 = 双耳）。设备不变，切的是空间化算法。设备层最简单。
2. **格式需设备支持，否则禁用该选项**。真多声道监听，但切换会重配音频设备（声道数变 → 设备重启，难无缝）。
3. **永远双耳监听，后端只换算法**。输出固定 2ch 双耳，「切后端」= 换喂双耳的空间化路径。范围最小、最快，但听不到真扬声器布局差异。

**此决策不阻塞切片 1–3**（后端流式化 + 活参数层与设备映射解耦）。

## 9. 落地切片与工作量

工程量级，非日历。**强烈建议垂直切片**，不一上来重构五个后端。

| 切片 | 内容 | 量级 | 依赖 |
|---|---|---|---|
| 1 | **实时引擎骨架**（ring + worker-ahead + 最小设备输出或测试 PCM sink + 时钟 + loop region）；**Apple stream**（循环已 slice 化，最快出声） | 中 + 小 | — |
| 2 | **活覆盖层 + gain 即时**（无锁快照，gain 逐块乘走通） | 小-中 | 切片 1 |
| 3 | **binaural stream**（OLA / spreader / diffuse / reader / cursor 提升为会话；算法不变） | 中-大 | 切片 1 |
| 4 | **diffuse / extent / divergence = 廉价 re-prepare + reseek** | 中 | 切片 2、3 |
| 5 | **EAR / VBAP / HOA stream**（套切片 3 同一模式） | 各中 | 切片 3 |
| 6 | **热切换 + 交叉淡化 + 监听 / 下混层** | 中 | §8 决策、切片 5 |

切片 1 先用 Apple 验证「边播边出声 + 引擎 plumbing」；切片 3 用 binaural 验证「批 → 流」契约对最难的后端成立，契约一稳，切片 5 是复制。

## 10. 风险与注意

- **binaural 精巧度**：1933 行，OLA 残差搬运（`:758`）、spreader 的 `prime()` 延迟补偿、diffuse 去相关——提升为会话时必须保证逐块输出与一次性 `render_window` **逐样本一致**。`process()` 外部请求帧数可能来自设备回调，不应改变 DSP 分块；stream 内部要固定 canonical block + FIFO（回归基线：与离线 binaural 渲染 bit 级对比一段定长窗口）。
- **引擎 plumbing**：欠载（underrun）处理、时钟漂移、设备采样率（binaural 固定 48 kHz，设备非 48k 需重采样或拒绝）、loop region 边界回卷的 DSP 状态处理。
- **re-prepare 成本**：diffuse/extent/divergence 编辑重建源列表，必须实测其耗时落在「松手即更新」可接受范围；HRTF/VBAP 表务必复用不重算。
- **ADR 0003 / 0007**：`IRenderStream` 接口零第三方类型；C ABI 仅 additive、新 minor、`SOVERSION 1` 不变。
- **Apple-only**：Apple stream `if(APPLE)` 门控，跨平台 ctest 跳过，与现有 `mr_adm_apple_smoke_tests` 一致。
- **平台覆盖**：实时引擎与设备层须跨平台（macOS + Linux）；设备后端选型（miniaudio vs 原生）影响 §8 与依赖管理（ADR 0004，走 `mr_adm_core_find_or_fetch`）。

## 11. 未决事项

1. **监听输出模型**（§8 的 1/2/3）——切片 6 前必须定，切片 1–3 不阻塞。
2. **设备后端选型**：miniaudio（跨平台、轻、单头）vs macOS 原生 CoreAudio + Linux 另接。倾向 miniaudio 统一，待评估。
3. **level meter / 播放头回调**的 ABI 形状（实时 UI 需要电平 + 播放位置反馈）。
4. **采样率不匹配**策略：拒绝 vs 设备侧重采样。
