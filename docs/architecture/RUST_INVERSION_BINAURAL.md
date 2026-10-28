# 双耳渲染迁入 Rust（四期 4.7 B）

2026-10-27 对照基线 `c967d840` 与 Claude 候选 `1c52d999`。两平台已测 PCM 全部逐位相同，
Windows 性能回退仍开放。维护者要求记录问题后合并、推送；本次合并不表示性能验收完成。

同日后续[性能调查](RUST_BINAURAL_PERFORMANCE_REVIEW.md)确认拓扑重建时新旧卷积 bank 重叠是内存回退的原因，
并修订了 cloud 的计时依据：旧采集器的 2 ms 休眠实际约 15.55 ms，改用直接等待进程退出后，两轮无插桩
Release 对照为 +3.21% / +1.39%。内部调度与输出归约仍有额外成本；归因实验未作为正式修复，两项仍开放。

2026-10-28：BINAURAL-002 已通过持久 bank / 工作池与卷积工作区复用修复，两平台 Release 内存和 PCM
对照通过；BINAURAL-001 继续开放。范围、源码身份和数据见[内存修复](RUST_BINAURAL_MEMORY_FIX.md)。

## 迁移范围

`mradm-render::binaural` 持有声源规划、divergence × extent 槽、DirectSpeakers 标签与 LFE 旁路、
spreader 轨道和分组；`binaural_render` 持有逐源 OLA、spreader 适配器、工作池、离线窗口、
延迟补偿、计量与尾声。C++ 保留路由与采样率检查、写出、取消、进度和日志。
文件流式渲染仍由 C++ `BinauralStream` 编排，通过过渡 FFI 驱动 Rust OLA bank。
本次没有执行 Scene 流式渲染对照或设备欠载验收。

## 正确性对照

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

## 开放问题

以下是正常桌面环境中的端到端进程测量，未调整日常应用、后台服务、亲和性或供电设置。
Windows 为 i7-11800H、16 个逻辑核心、交流供电。素材长 1 秒；耗时包括启动、准备、渲染、
计量与输出，文件流式工具每次运行在同一进程内重放两遍。峰值内存使用
`GetProcessMemoryInfo` 的 peak working set，处理期间每 2 ms 采样。

### BINAURAL-001：Windows cloud 离线端到端耗时回退（P2，开放）

- 条件：`objects-extent`，`saf-binaural`、`cloud`、f32、关闭峰值限制。
- 预热后交替测量 5 轮，中位数由 132.54 ms 增至 149.04 ms（+12.45%）。
- 同条件交替复测 9 轮，中位数由 135.08 ms 增至 148.62 ms（+10.02%）；峰值内存约相同。
- PCM 在所有轮次保持一致。回退根因尚未确认；后续需区分准备、工作池调度和渲染成本，
  在 Windows 修复并复测后再继续适用的 macOS 性能矩阵。

### BINAURAL-002：Windows 文件流式拓扑编辑峰值内存回退（P2，已修复）

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

## 证据

原始轮次、请求参数、PCM 与二进制 SHA-256、构建配置及工具链身份保存在：

- [汇总](evidence/binaural-render-20261027/summary.json)
- [Windows 初测](evidence/binaural-render-20261027/windows-short.json)
- [Windows 9 轮复测](evidence/binaural-render-20261027/windows-recheck.json)
- [Windows 文件流式隔离](evidence/binaural-render-20261027/windows-stream-isolation.json)
- [macOS PCM](evidence/binaural-render-20261027/macos-pcm.json)
- [Windows PCM](evidence/binaural-render-20261027/windows-pcm.json)
- [构建与夹具指纹](evidence/binaural-render-20261027/provenance.json)
