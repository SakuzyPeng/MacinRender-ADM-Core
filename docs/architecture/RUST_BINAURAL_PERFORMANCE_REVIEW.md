# 双耳迁移性能调查（4.7 B）

2026-10-27。调查 [BINAURAL-001 / 002](RUST_INVERSION_BINAURAL.md#开放问题)，
比较迁移前 C++ 编排 `c967d840` 与 Rust 编排 `1c52d999`。本次调查只形成记录、复现工具和证据，
未修改生产实现；当时两项问题均开放。

2026-10-28 续：BINAURAL-002 已完成修复与 macOS / Windows 验证，见
[内存修复](RUST_BINAURAL_MEMORY_FIX.md)。以下保留 2026-10-27 的调查过程和原始观察，BINAURAL-001 继续开放。

## 结论

- **文件流式拓扑编辑的内存回退已定位。** 新 `SourceBank` 完整分配后才释放旧 bank，
  重建时两套各 96 个渲染源的卷积状态同时存在；每个新 bank 还重新创建 16 线程 Rayon 池。
  仅提前释放旧 bank 的归因实验将峰值从 52.11 MiB 降到 43.28 MiB，接近迁移前的 43.15 MiB。
- **cloud 的历史端到端“+10%”不能作为精确回退幅度。** 旧采集器以 `sleep(0.002)` 轮询退出，
  本机 100 次休眠的实际中位数为 15.55 ms。把计时改为直接等待进程退出、独立线程采集内存后，
  两轮各 21 次交替测量分别为 +3.21% 和 +1.39%。旧记录保留，不能直接与新口径混算。
- **内部仍有可测的额外成本。** HRTF / 声源准备约 30 ms，前后基本不变；迁移后渲染窗口增加约
  2.6 ms。已验证的优化方向是小批次的任务提交方式，以及离线输出归约的索引循环。
  这些是插桩归因结果，不是正式性能验收。

## 条件与计时修正

Windows x64、i7-11800H、16 个逻辑核心、交流供电。保留日常应用、服务、亲和性和供电设置；
记录每组前后的负载样本，不以负载高低剔除结果。构建与测量串行，复用原 canonical MSVC/Ninja
Release、Cargo 和 FetchContent 目录，没有新建独立缓存。SOFA、CUDA 和一致性诊断均关闭，使用内置 KEMAR。

cloud 输入为原来 1 秒、48 kHz 的 `objects-extent`，展开为 32 个渲染源、47 个内部块。
文件流式渲染使用原 `stream-binaural-cloud-fixed`：`objects-extent-multi`、512 帧拉取，包含增益、
extent / diffuse / divergence、头追和 seek；每个进程重放两遍。

先校验冻结 EXE 的 SHA-256 与原证据一致，再用原脚本重测 9 轮，得到 cloud +11.77%、
文件流式峰值 +20.92%，PCM 不变。随后发现旧采集器的轮询粒度足以影响这类百毫秒进程的毫秒级比较。
新采集器在主线程调用 `Popen.wait()`，立即记录 `perf_counter()`，再结束并回收内存采样线程；
采样线程停止的等待不计入耗时。`GetProcessTimes` 只作辅助记录，不替代这一计时。

每个版本预热一次，按轮次反转版本顺序，不并行运行。

| 无插桩 Release 对照 | 轮数 / 版本 | C++ 基线中位数 | Rust 候选中位数 | 变化 |
| --- | ---: | ---: | ---: | ---: |
| cloud，直接等待进程退出 | 21 | 117.58 ms | 121.35 ms | +3.21% |
| cloud，同口径复测 | 21 | 119.47 ms | 121.13 ms | +1.39% |
| 文件流式完整控制重放 | 15 | 276.73 ms | 291.88 ms | +5.47% |
| 同一重放的峰值工作集 | 15 | 43.15 MiB | 52.11 MiB | +20.76% |

精确等待下的文件流式耗时仍值得继续优化，不能沿用旧采集器的“耗时约相同”结论。
以上是这些短素材和重放的结果，不外推到长节目、其他声源规模或设备欠载率。

## BINAURAL-002：bank 的生命周期

迁移前 `BinauralStream::init_per_source_state()` 先 `src_cs_.clear()`，再创建卷积器与缓冲；
`ola_pool_` 由整个流持有，拓扑重建不重建线程池。

迁移后 `rebuild_sources()` 先通过 `SourceBank::create()` 创建完整的新 bank，再移动赋值给 `bank_`。
Rust `mradm_render_binaural_bank_create()` 为每个 bank 分配 `SourceState`、FFT / 卷积缓冲和线程池。
创建成功前，旧 bank 仍然存活。

一次代表性内存探针中，创建替换 bank 前工作集为 43,573,248 字节，创建后为 53,563,392 字节；
释放旧 bank 后降到 43,843,584 字节。峰值来自重建时的临时重叠，不是根据这些数据判定的持续泄漏。

`release-before-rebuild.patch` 只在重建前插入 `bank_ = SourceBank{}`。无计时探针的 Release，
15 轮对照得到峰值 **43.28 MiB**，相对迁移前仅 +0.29%；耗时 **289.34 ms**，相对迁移前 +4.56%。
输出仍与基线逐位相同。

这个补丁只用于归因：它会在新 bank 创建可能失败前丢弃旧状态，不能直接作为正式修复。
正式实现应把工作池的生命周期移回文件流式会话，并在拓扑描述准备、校验成功后复用或重置卷积工作区；
同时保留失败状态、旧音频块交叉淡化、seek、头追和固定声源归约顺序的契约。

## BINAURAL-001：渲染窗口内的成本

同一轮 15 次插桩对照的阶段中位数如下。阶段分别取中位数，不能把各列相加当作同一次调用。

| 阶段 | C++ 编排 | Rust 编排 |
| --- | ---: | ---: |
| 后端准备（含 HRTF / 声源计划） | 30.09 ms | 30.13 ms |
| 卷积状态初始化 | 2.21 ms | 2.11 ms |
| 创建工作池 | 0.34 ms | 0.39 ms |
| 全部 47 次并行工作调用 | 7.55 ms | 8.89 ms |
| 整个 `render_window` | 17.88 ms | 20.49 ms |

两项归因实验保留 DSP 运算和声源累加顺序：

1. **任务提交。** 把 `pool.install(|| par_iter_mut())` 换成在调用线程提交独立任务的
   `in_place_scope`。cloud 探针中的工作阶段降到约 7.8 ms，HRTF、卷积和任务数量不变。
   但文件流式 96 源重放的工作阶段约为 72.22 / 72.72 ms，没有显示收益，不能据此全局替换调度方式。
2. **输出归约。** `dl[f] += st.left[f]` / `dr[f] += st.right[f]` 改为先限定输入切片、再按迭代器配对累加。
   在同一次 9 轮交替对照中，归约阶段从 **2.0006 ms 降到 0.5961 ms**；另一组同样观察到
   1.8307 → 0.5406 ms。证明这一循环形式有可消除的成本；未以汇编证据进一步划分边界检查与向量化的贡献。

组合实验的 cloud `render_window` 为约 17.35 ms，接近参考的约 18 ms。
这仍是带探针、单素材的归因结果；尚未提交无插桩正式修复候选，不能宣称性能问题已修复。

## 验证范围与后续

保存的 249 次计量运行（两个固定用例的重复和实验变体）全部核对最终 PCM，与 `c967d840` 一致；
文件流式每次还核对两遍进程内重放。249 不是 249 种独立语义用例，也不代替原来的完整正确性矩阵。

后续应先修复 bank / 工作池生命周期，再把归约与调度改动分别作为候选，在 Windows 无插桩 Release
复测完整控制重放、静态重放、增益 / 头追 / seek，以及不同声源规模和时长。通过 Windows 筛选后，
再完成适用的 macOS 正确性与性能验证。本轮没有安排 Linux 主机验证。

## 证据与复现

数据、二进制及临时源码身份见 [汇总](evidence/binaural-performance-20261027/summary.json) 和
[身份清单](evidence/binaural-performance-20261027/identities.json)。主要记录：

- [原采集器复现](evidence/binaural-performance-20261027/reproduction.json)、[休眠粒度](evidence/binaural-performance-20261027/poll-sleep.json)
- [精确等待首轮](evidence/binaural-performance-20261027/cloud-native-wait.json)、[复测](evidence/binaural-performance-20261027/cloud-native-wait-recheck.json)
- [bank 生命周期对照](evidence/binaural-performance-20261027/lifetime-native-wait.json)、[内存探针](evidence/binaural-performance-20261027/stage-memory.json)
- [分段计时](evidence/binaural-performance-20261027/stage-native-wait.json)、[归约实验](evidence/binaural-performance-20261027/reduction-iteration-profile.json)、[组合实验](evidence/binaural-performance-20261027/reduction-dispatch-profile.json)
- [文件流式工作池对照](evidence/binaural-performance-20261027/stream-work-profile.json)、[文件流式提交方式实验](evidence/binaural-performance-20261027/stream-schedule-profile.json)

同目录的 `.patch` 是临时诊断源码覆盖，须应用到身份清单中的对应提交；只作复现，未进入生产实现。
补丁采用零上下文格式，使用 `git apply --unidiff-zero`；已逐一验证能恢复身份清单记录的源码 SHA-256。
`collect.py` 将原本机运行器的目录参数化，使用 Windows 原生 Python，`--wait` 选择修正后的计时方式。
`$Suite` 为原比较集合（`cases.json` 和 `fixtures/`），`$BinaryRoot` 包含已校验的 `baseline/`、`candidate/` EXE：

```powershell
& $Python docs/architecture/evidence/binaural-performance-20261027/collect.py baseline candidate `
  --suite $Suite --binaries-root $BinaryRoot --reference-root $BinaryRoot `
  --output-dir $Output --case cloud --repeats 21 --wait
```

改为 `--case stream-controls` 复现文件流式重放；不传 `--wait` 仅用于比较旧轮询口径。
诊断版本可用 `--schedule in_place`。原运行器、构建日志和未裁剪记录另保存在本机
`local/binaural-perf-20261027/investigation-evidence.zip`，SHA-256：
`a8a661768ca6c0231f76685cfa036debc3ae4baa7cd635c5b3f3f54595588c2e`。

调查结束后，Windows 恢复原分支与干净源码，canonical 全量构建、`backends` 和 3 项双耳 CTest 通过。
参数化采集器另做两种用例的单轮功能烟测，结果保存在 `collector-smoke-*.json`，不计入上述 249 次运行。
其中 cloud 单次耗时为 115.36 / 148.76 ms，也予以保留：进程级耗时仍有波动，两轮 21 次中位数
不是其他运行的回退上限，不能据此关闭耗时问题。
