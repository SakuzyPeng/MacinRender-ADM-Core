# 双耳文件流式拓扑重建的内存修复

2026-10-28。修复 [BINAURAL-002](RUST_INVERSION_BINAURAL.md#开放问题)。
基于 `949e14e4` 的修复，源码身份见 [source.json](evidence/binaural-memory-fix-20261028/source.json)。
Windows 与 macOS 的无插桩 Release 对照确认，完整控制重放的峰值内存分别减少约 17.8% 和 16.9%，
最终 PCM 保持逐位一致。BINAURAL-001 的其他耗时问题继续保留。

## 实现

文件流式会话现在一直持有同一个 Rust `BinauralBank` 和工作池。拓扑编辑先生成新的声源描述，
然后由 `Bank::rebuild` 调整已有卷积状态：

- 同规模重建复用 FFT、卷积和声道缓冲；缩小时释放移除声源的工作区；增长时只分配差额。
- 新增工作区准备成功后才重置旧历史并提交新描述，可恢复失败保留原 bank、视图和音频历史。
- 更新每个声源的输出指针和语义键，保留增益槽、头追、seek 与声源归约顺序。
- 交叉淡化仍先计算完整旧块，再在同一输入块上计算新图并混合；重用工作区前已保存旧块输出。

私有 FFI 的 `mradm_render_binaural_bank_create` 将 `*output` 作为输入／输出：初始 null 创建，
已有句柄原地重建，成功时更新视图。C++ 删除整套 bank 的替换和重复初始化，仅保留适配。
公开 C ABI 没有变化，生产 C/C++ 由 34,958 行减到 34,953 行。

这里的零分配验证针对同规模／缩小的卷积工作区；声源描述和 FFI 视图仍会在重建时创建。

## 无插桩 Release 对照

复用既有构建与 Cargo / FetchContent 缓存，保留日常桌面应用、服务、亲和性与供电设置。
构建与性能测量串行。Windows 使用 canonical MSVC/Ninja、内置 KEMAR、SOFA/CUDA/诊断关闭；
macOS 使用同一 KEMAR 性能用例，正确性另覆盖外部 SOFA。

主线程直接等待进程退出，Windows 的内存采样独立运行；macOS 由 `/usr/bin/time -l` 收集峰值 RSS。
预热一次后交替运行，每轮反转版本顺序。Windows 的“修复前”是冻结 Rust `1c52d999`，其生产实现
与 `949e14e4` 相同；macOS 的直接 Rust 参考由 `949e14e4` 无修改生产源码重新构建。

| 平台与用例 | 轮数／版本 | 修复前峰值 | 修复后峰值 | 修复前耗时 | 修复后耗时 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Windows：完整控制重放 | 15 | 52.31 MiB | 43.02 MiB | 282.61 ms | 236.51 ms |
| Windows：碎片拉取 | 9 | 52.22 MiB | 43.30 MiB | 292.81 ms | 240.06 ms |
| Windows：10 次控制循环 | 5 | 56.40 MiB | 48.96 MiB | 1359.47 ms | 958.76 ms |
| macOS：完整控制重放 | 15 | 58.30 MiB | 48.45 MiB | 156.32 ms | 140.02 ms |
| macOS：10 次控制循环 | 5 | 66.31 MiB | 56.38 MiB | 883.63 ms | 815.70 ms |

数值为各指标中位数；Windows 使用 peak working set，macOS 使用 peak RSS，不跨平台比较绝对值。
完整控制重放包含 gain、extent/diffuse/divergence、头追、seek 和恢复默认控制，每个进程重放两遍。
较长用例反复跳回同一素材并重复控制事件，累计请求 480,000 帧；它不是长期设备欠载验收。

Windows 首轮短测以峰值至少减少 10%、耗时不增加超过 5% 为进入后续验证的条件，结果分别为
-17.76% / -16.31%。后续静态、增益／头追／seek、碎片拉取、拓扑收缩／扩展及较长重放均逐位一致。
与迁移前 C++ `c967d840` 参考相比，Windows 完整控制重放的峰值为 43.12 → 42.97 MiB，
macOS 为 48.08 → 48.89 MiB，均在 1.10 的内存调查阈值内。

macOS 对 C++ 参考的静态短测曾得到 72.95 → 76.73 ms（+5.19%），该记录保留在
[macos-static.json](evidence/binaural-memory-fix-20261028/macos-static.json)。补做直接 Rust 修复前／后
21 轮静态对照为 76.15 → 76.09 ms（-0.07%），峰值 46.91 → 47.02 MiB；本次改动未显示新增静态开销。
这不关闭原双耳迁移的其他耗时问题，也不把不同批次的绝对耗时混为同一对照。

## 正确性与质量

- Windows：13 个离线用例、54 个冻结 PCM golden、10 个文件流式重放，共 77 项与修复前逐位一致。
- macOS：19 个离线用例（含 6 个 SOFA）、54 个 golden、10 个文件流式重放，共 83 项与 C++ 参考一致。
- 两平台共用的 77 个候选输出也逐位一致；每次文件流式重放还核对两遍进程内输出。
- 两平台各通过 13 项 Rust 双耳测试和 2 项新增 FFI 测试；Mac Debug、Mac Release、Windows Release
  的 3 项双耳 CTest 均通过。
- 新增覆盖检查工作区保留、增长／缩小／清空后与全新 bank 的输出相同、同规模／缩小时不分配卷积工作区，
  以及失败重建不改变句柄、视图和历史。
- 私有 FFI 校验通过（344 个函数、143 个结构体）；Rust clippy `-D warnings`、格式检查、cppcheck、
  语言规模棘轮及差异检查通过。clang-tidy 退出码为 0，保留第三方 fmt 的构造诊断。

没有安排 Linux 主机验证。Scene 流式渲染与设备欠载率不属于本次性能结论。

## 证据与复现

[汇总](evidence/binaural-memory-fix-20261028/summary.json)关联原始轮次、平台与供电／负载记录、
二进制 SHA-256、PCM、请求参数和源码身份。[source.patch](evidence/binaural-memory-fix-20261028/source.patch)
采用零上下文格式，基于 `949e14e4` 使用 `git apply --unidiff-zero`；已验证应用后每份源码的 SHA-256
与清单一致。历史调查和失败证据见[性能调查](RUST_BINAURAL_PERFORMANCE_REVIEW.md)。

`collect-windows.py` 与 `collect-macos.py` 以原比较集合和两个程序目录为输入，不创建构建缓存。
例如 Windows 原生 Python 可运行：

```powershell
& $Python docs/architecture/evidence/binaural-memory-fix-20261028/collect-windows.py before fixed `
  --suite $Suite --binaries-root $BinaryRoot --reference-root $BinaryRoot `
  --output-dir $Output --case stream-controls --repeats 15 --wait
```

可选用例为 `stream-static`、`stream-gain-head-seek`、`stream-fragmented`、`stream-topology-cycle`
和 `stream-long`；Mac 使用对应采集器与无 `.exe` 后缀的程序。所有输出都验证有限样本、帧数、
两遍重放及版本间 PCM 一致，再删除临时音频。

Windows 验证后已恢复原分支与干净源码，canonical 重建及双耳烟测通过。Mac 工作区保留修复后的
源码和 Release 程序；临时参考程序及重复夹具副本已删除，共享构建缓存保留。原运行器和完整构建日志
另归档于本机 `local/binaural-memory-fix-20261027/final-evidence.zip`，SHA-256 见汇总记录。
