# 切片 1 落地子计划：实时引擎骨架 + IRenderStream + Apple stream

> 状态：草案
> 日期：2026-06-16
> 母文档：`REALTIME_MONITORING.md`（§6 接口、§9 切片表）。本文件只展开**切片 1**的落地细节：文件清单、接口落点、CMake/ABI、验证路径与范围边界。对应任务 #21。

## 1. 切片 1 的验收（Definition of Done）

- `IRenderStream` + `IRenderer::open_stream`（默认 `unsupported`）落入 `include/adm/render.h`；现有五后端**不**改（继承默认即可），编译通过、既有测试全绿。
- 私有 internal 库 `MacinRender::ADMRendererFactory`（§5）：把 `render_service.cpp` 的后端解析抽成 `resolve_renderer`，RenderService 改调、**行为 bit 不变**。
- 新模块 `MacinRender::ADMRealtime`：`MonitorEngine`（经注入的 `IRenderStreamFactory`，§4.1）+ SPSC ring + worker-ahead + 播放时钟 + loop region；设备层自有抽象 `IAudioOutputDevice`（miniaudio sink + 测试用 capture sink）。
- Apple stream（`AppleRenderer::open_stream`，macOS-only）：把 `render_window` 的 AU 驱动循环搬进 `IRenderStream`，输出从 `WriterHandle` 换成填调用方 buffer。
- C ABI v1.15：`adm_monitor_*` 八个入口（create/play/pause/seek/set_loop/get_status/get_levels/destroy）+ 两个查询结构体。
- **验收测试**：跨平台单测经 `PatternStreamFactory + CapturePcmSink` 验引擎/ring/seek/loop/分块边界**逐样本一致**（无真实后端，Linux CI 可跑）；Apple stream 的 bit-exact 对比走 macOS-only smoke（见 §8）。

**不在切片 1**：活覆盖层 / gain 即时（切片 2）、binaural stream（切片 3）、热切换 / 下混 / 双耳化（切片 6）、非 48k 重采样（返回 unsupported，后续）。

## 2. 接口落点（`include/adm/render.h`）

紧挨现有 `IPreparedRender`（`render.h:109`）/ `IRenderer`（`:124`）之后加，签名照 `REALTIME_MONITORING.md` §6（不复述）。要点：

- `IRenderStream`：纯抽象 + 删拷贝/移动（同 `IPreparedRender` 风格）。
- `IRenderer::open_stream(prepared, plan, logs)` **非纯虚**，基类默认实现返回
  `make_error(ErrorCode::unsupported, "backend has no realtime stream", {})`——让五后端零改动继续编译，已流式化的后端逐个 `override`。
- `EmptyPreparedRender`（`:121`）仍可用：Apple 的 `prepare()` 已产出 `ApplePrepared`，`open_stream` 直接 `dynamic_cast` 复用。

## 3. 新模块 `ADMRealtime`（`CMakeLists.txt` + `src/adm_realtime/`）

照现有模块模式（`CMakeLists.txt:121` ADMRenderCommon 是最简范本）：

两个新 target——`ADMRendererFactory`（§5，被 ADMEngine + ADMRealtime 共用）与 `ADMRealtime`：

```cmake
# 私有 internal 库:后端解析,只给 ADMEngine / ADMRealtime 链,不进 public header。
add_library(mr_adm_renderer_factory)
add_library(MacinRender::ADMRendererFactory ALIAS mr_adm_renderer_factory)
target_link_libraries(mr_adm_renderer_factory
    PUBLIC  MacinRender::ADMCore
    PRIVATE MacinRender::ADMRenderEar MacinRender::ADMRenderVBAP MacinRender::ADMRenderHOA
            MacinRender::ADMRenderBinaural)
if(APPLE)
    target_link_libraries(mr_adm_renderer_factory PRIVATE MacinRender::ADMRenderApple)
endif()

add_library(mr_adm_realtime)
add_library(MacinRender::ADMRealtime ALIAS mr_adm_realtime)
# PUBLIC: ADMCore（接口里只有 IRenderStream / AdmScene / 标准库）
# PRIVATE: 解析库 + ADMAudio（测试 sink 复用 WriterHandle）+ 设备库
target_link_libraries(mr_adm_realtime
    PUBLIC  MacinRender::ADMCore
    PRIVATE MacinRender::ADMRendererFactory MacinRender::ADMAudio miniaudio)
```

`MacinRender::ADMEngine`（`:388`）改链 `ADMRendererFactory`（替代现在直接链各 renderer 的部分，行为不变）并增链 `ADMRealtime`；C ABI 在 `ADMCAPI`（`:424`）落 `adm_monitor_*`。miniaudio 经 `mr_adm_core_find_or_fetch`（ADR 0004，`cmake/MRDependencies.cmake`）接入，**不**散落 FetchContent。

文件清单：

| 文件 | 模块 | 职责 |
|---|---|---|
| `src/adm_engine/renderer_factory.h/.cpp` | ADMRendererFactory | 见 §5：`resolve_renderer(...) → ResolvedRenderer` |
| `src/adm_realtime/monitor_engine.h/.cpp` | ADMRealtime | `MonitorEngine`：持 stream + ring + worker + clock + loop；play/pause/seek/get_status/get_levels |
| `src/adm_realtime/render_stream_factory.h` | ADMRealtime | `IRenderStreamFactory` 抽象（**依赖注入点**，见 §4/§8）+ 真实实现（经 `resolve_renderer` + `prepare` + `open_stream`） |
| `src/adm_realtime/ring_buffer.h` | ADMRealtime | SPSC float ring（header-only，无第三方） |
| `src/adm_realtime/audio_output_device.h` | ADMRealtime | `IAudioOutputDevice` 抽象（**接口零第三方类型**） |
| `src/adm_realtime/pcm_file_sink.cpp` | ADMRealtime | 落文件 sink：实现 `IAudioOutputDevice`，内部用 `audio::WriterHandle`（手测/可选） |
| `src/adm_realtime/miniaudio_device.cpp` | ADMRealtime | miniaudio 实现；**miniaudio 类型只在此 .cpp**，不进任何头 |

边界自检（ADR 0003）：`audio_output_device.h` / `monitor_engine.h` / `render_stream_factory.h` 里 `grep miniaudio|ma_` 必须为空。

## 4. 设备层抽象 + 测试 sink

```cpp
namespace mradm::realtime {
// 设备拉模型：设备回调向 sink 要 frames 帧交织 PCM。零第三方类型。
class IAudioOutputDevice {
  public:
    virtual ~IAudioOutputDevice() = default;
    // pull: 设备回调调用,要求填 out（frames * channels 交织）；返回实际填入帧数。
    using PullFn = std::function<std::size_t(std::span<float> out, std::size_t frames)>;
    virtual Result<void> start(uint32_t channels, uint32_t sample_rate, PullFn pull) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual uint32_t actual_sample_rate() const = 0; // §11 决策4:可能≠请求
};
}
```

- **miniaudio sink**：`start()` 配 `ma_device`（playback、f32、请求 48k），`data_callback` 里调 `pull`。`actual_sample_rate()` 反映 miniaudio 实际拿到的设备率（喂 §8 下混 / §11 重采样判断；切片 1 只在非 48k 时 `unsupported`，重采样留后）。
- **测试用 capture / file sink**（单测，见 §8）：同步、确定性、无声卡依赖——`start()` 在调用线程按固定块长循环调 `pull`，把 PCM 收进内存 buffer（或经 `WriterHandle` 落 WAV）供 bit-exact 断言。

### 4.1 stream 工厂依赖注入

`MonitorEngine` **不直接 new stream**，而是构造时接收 `IRenderStreamFactory`：

```cpp
class IRenderStreamFactory {
  public:
    virtual ~IRenderStreamFactory() = default;
    // 解析后端 + prepare + open_stream,产出一个流式会话。
    virtual Result<std::unique_ptr<IRenderStream>> open(const AdmScene& scene, const RenderOptions& opts,
                                                        LogSink& logs) = 0;
};
```

- **真实工厂**（C ABI / 生产路径）：内部走 §5 `resolve_renderer` + `prepare` + `open_stream`。
- **测试工厂**（单测）：`PatternStreamFactory`，产出 `PatternStream`（见 §8）。

这样引擎逻辑（ring / worker / clock / loop / seek）能在**无任何真实后端**下被跨平台断言，且测试桩**不**成为公开 renderer、**不**出现在用户可选 backend 列表里。

## 5. renderer 解析复用（小重构）

当前后端选择逻辑内联在 `render_service.cpp:566-596`，**不只是 `create_*_renderer()` 分派**，还含一组行为：

- `automatic` + `0+2+0` / `binaural` → `saf_binaural`；
- 禁止 speaker stereo，除非 `internal_allow_speaker_stereo`；
- `binaural` legacy alias 告警；
- saf-binaural 强制 `effective output layout = binaural`；
- Apple 非 macOS `unavailable`。

所以**不要只抽裸 factory**（`make_renderer(sel)` 会让 RenderService 与 MonitorEngine 行为分叉）。抽**完整解析结果**：

```cpp
struct ResolvedRenderer {
    std::unique_ptr<IRenderer> renderer;
    RendererSelection selected;          // 解析后的实际后端（automatic/legacy alias 归一）
    std::string effective_output_layout; // 如 saf-binaural 强制为 "binaural"
    // 诊断（legacy alias 告警等）——由调用方决定怎么记日志。
    std::vector<std::pair<LogLevel, std::string>> diagnostics;
};

Result<ResolvedRenderer> resolve_renderer(RendererSelection requested,
                                          std::string requested_layout,
                                          bool internal_allow_speaker_stereo,
                                          LogSink& logs);
```

**放置**：`src/adm_engine/renderer_factory.h/.cpp`，**不进 `include/adm/`、不进 ADMCore**——它天然依赖 `create_ear_renderer/create_vbap_renderer/...`，知道所有后端；放进 ADMCore 会把「核心模型层」污染成「后端注册层」。

**target**：做成**私有 internal 库 `MacinRender::ADMRendererFactory`**（`add_library(mr_adm_renderer_factory)`，PRIVATE 链各 renderer + Apple `if(APPLE)` 门控），只给 `ADMEngine` / `ADMRealtime` 链，不进任何 public header。

`RenderService::render` 改调 `resolve_renderer`，**行为 bit 不变**（既有 saf_binaural 别名告警、speaker-stereo 禁用、layout 强制全保留）。MonitorEngine 也调它，共享同一套行为。**这步先做、单独验证既有测试不回归**，再叠 stream。

## 6. Apple stream 实现（`spatial_mixer_renderer.cpp`，macOS-only）

`render_window`（`:593`）的结构：建 `WriterHandle` + ebur128 + 双缓冲 `out_buffers` → `while(frames_done<win_end)` 每 `k_render_block` 一次 `AudioUnitRender` → `writer.write`。流式化：

- 新 `AppleStream final : IRenderStream`，构造时持 `ApplePrepared` 引用 + 建 `AudioUnit`（`create_spatial_mixer_unit`）+ 输入 staging + bus 游标 + AU sample-time 游标。**AU 与 buses 从 render_window 局部提升为 stream 成员**。
- `process(out, frames)`：内部固定 `k_render_block`（§10 canonical block）拉 `AudioUnitRender` 到 staging，按 bus 事件 `AudioUnitSetParameter`（沿用 `:454-463` 逻辑），交织进 `out`；用 FIFO 吸收 `frames` 与 `k_render_block` 不整除。**不写文件、不计量 ebur128**（实时路径）。
- `seek(frame)`：重建 / reset AU 状态 + 重定位 bus 游标 + AU sample time（AUSpatialMixer 是黑盒 stateful，沿用 render_window 的「对齐前一块 pre-roll」做法 `:618-623`）。
- 输入仍由 worker 从 `bw64` reader 读到 AU staging（母文档 §2.3 / §4）。
- `AppleRenderer::open_stream` `dynamic_cast<ApplePrepared>` 后 `make_unique<AppleStream>`。
- `render_window` 与 `AppleStream` **共享**取事件 / 设参数 / extent 展开等纯函数，避免分叉（抽到文件内 helper）。

## 7. C ABI（`include/adm/c_api.h` + `src/adm_c_api/`，v1.15 additive）

`ADM_API_VERSION_MINOR` 14→15 + 头部 changelog；`SOVERSION 1` 不变（ADR 0007）。

```c
typedef struct adm_monitor_t adm_monitor_t; /* v1.15 opaque */

/* opts 复用 adm_render_options（renderer/layout/sofa…）。input_path 当前必须 48kHz。*/
adm_error_code_t adm_create_monitor(adm_context_t*, const char* input_path,
                                    const adm_render_options_t* opts, adm_monitor_t** out) ADM_API_NOEXCEPT;
adm_error_code_t adm_monitor_play(adm_monitor_t*) ADM_API_NOEXCEPT;
adm_error_code_t adm_monitor_pause(adm_monitor_t*) ADM_API_NOEXCEPT;
adm_error_code_t adm_monitor_seek(adm_monitor_t*, double seconds) ADM_API_NOEXCEPT;
adm_error_code_t adm_monitor_set_loop(adm_monitor_t*, double start_sec, double end_sec /* <=0 = 整段 */) ADM_API_NOEXCEPT;
void             adm_destroy_monitor(adm_monitor_t*) ADM_API_NOEXCEPT;

/* 轮询(§11 决策3):首字段 struct_size,后续 minor 可追加字段。*/
typedef struct { uint32_t struct_size; int32_t state; double playhead_sec; double duration_sec;
                 float ring_fill; uint64_t underruns; uint64_t override_revision; } adm_monitor_status_t;
/* levels: 调用方提供 peak/rms 缓冲 + capacity(声道上限),引擎写回 out_count(实际声道)。
   避免写死 32 声道,且多声道/追加字段都向后兼容。*/
typedef struct { uint32_t struct_size; uint32_t capacity; uint32_t out_count;
                 float* peak; float* rms; } adm_monitor_levels_t;
adm_error_code_t adm_monitor_get_status(adm_monitor_t*, adm_monitor_status_t* out) ADM_API_NOEXCEPT;
adm_error_code_t adm_monitor_get_levels(adm_monitor_t*, adm_monitor_levels_t* out) ADM_API_NOEXCEPT;
```

`adm_monitor_set_overrides` / `adm_monitor_switch_backend` **留到切片 2 / 6**（不在 v1.15 首批，避免空壳 ABI——ABI 一旦上就是承诺，空壳会逼后续兼容旧形状）。所有导出 `noexcept` + `try/catch` 翻译为 `adm_error_code_t`（ADR 0005）。

## 8. 验证路径

1. **既有回归**：`make_renderer` 重构后 `ctest --preset debug` 全绿（行为 bit 不变）。
2. **新单测 `mr_adm_realtime_tests`**（跨平台，**无真实后端依赖**，经 §4.1 注入 `PatternStreamFactory + CapturePcmSink`）：
   - ring SPSC 正确性（多线程产/消、wrap、欠载计数）；
   - **`PatternStream`**：每帧输出确定函数 `sample(frame, ch) = frame * 0.001f + ch`。捕获 sink 收到的 PCM 与该函数**逐样本一致**，专抓：分块边界 off-by-one、`seek` 后状态没清（seek 到 F 后第一帧必须是 `sample(F,·)`）、ring wrap 写错、loop region 回卷、pause/resume 后 playhead 连续。`SilenceStream` 仅作补充（只证「没崩、长度对」）。
   - `process()` 请求帧数 **≠** 内部 canonical block（如请求 100 / 1000 / 4096）时输出仍逐样本正确（canonical block + FIFO 的硬约束）。
   - `get_status` / `get_levels` 的 `struct_size` 前后兼容（旧 size 读取不越界）。
3. **Apple smoke（macOS-only，`if(APPLE)`）**：`AppleStream.process` 拼起的整段 vs `render_window` 整段 bit-exact（沿用 `mr_adm_apple_smoke_tests` 跳过模式）。
4. **手测**：miniaudio sink → 真实声卡，48k binaural/Apple 听感连续、无爆音、seek 正常。
5. **质量门**：`scripts/quality/check-changed.sh --base origin/main --build-dir build/debug` + 边界 grep（§3）。

## 9. 落地顺序（切片 1 内部）

1. `IRenderStream` + `open_stream` 默认 unsupported（`render.h`），全绿编译。
2. `resolve_renderer` 抽取成 `ADMRendererFactory`（§5）+ RenderService 改调，回归 bit 不变。
3. ring + `IAudioOutputDevice` + `IRenderStreamFactory`（§4.1）+ capture sink（纯 CPU）。
4. MonitorEngine（worker + clock + loop）+ `PatternStreamFactory`，跑通 §8.2 逐样本断言。
5. C ABI v1.15 + headless 自测（GUI 的 `--selftest` 可加一条 monitor 冒烟）。
6. Apple stream（macOS，真实 `IRenderStreamFactory`）+ smoke bit-exact。
7. miniaudio sink + 手测真实声卡。

## 10. 风险

- **Apple AU 黑盒 seek**：AUSpatialMixer HRTF 模式 stateful，`seek` 后等价性靠 pre-roll；bit-exact 对比需容忍 AU 内部状态（必要时 smoke 用「整段一次性 process vs render_window」而非任意 seek 点）。
- **canonical block vs 设备帧数**：设备回调帧数任意，stream 内部固定块 + FIFO 是硬约束（母文档 §10），单测要专门覆盖「请求帧数 ≠ 块长」。
- **miniaudio 接入**：先确认 `mr_adm_core_find_or_fetch` 能拿到、许可证（MIT-0 / public-domain）登记进 `docs/THIRD_PARTY_LICENSES.md`、`MR_ADM_CORE_FETCH_DEPS=OFF` 发行路径有兜底。
- **跨平台 CI 覆盖**：切片 1 真实后端仅 Apple（macOS），Linux 侧靠测试桩 stream 保证引擎不裸奔——这是 §8.2 引入测试桩的原因，别省。
