# 离线驱动的计量重叠修复

2026-11-24。扬声器离线驱动（`mradm-render` `offline::Driver`，VBAP、HOA、EAR 与 Triple Balance 共用）
把计量放在后台线程，与下一块的渲染重叠。自 4.5 起，取回的缓冲放进第一个空槽而不是当前槽，
每隔一块都要等计量线程清空，上一块的计量不再与本块渲染重叠。修复后每块只等两块之前的计量，
与迁移前的 C++ 循环相同。最终 PCM、响度与 True Peak 读数不变。CLI 的 Ctrl-C 监视线程让每次渲染
多等 0–25 ms，单独的提交改为在渲染结束时唤醒它（见[监视线程一节](#cli-的-ctrl-c-监视线程)）。

## 现象

维护者用每版交替运行 12 轮的完整 CLI 耗时中位数对照。参考 `c0a3720e` 是后端编排迁移前的版本
（该提交不在 GitHub 仓库的历史中），当前版为 `201592ca`。最后一列是只改缓冲区归还位置的无插桩实验。

| 后端 | 素材 | 迁移前 | 当前 | 回退 | 改归还位置后 |
| --- | --- | ---: | ---: | ---: | ---: |
| VBAP 7.1.4 | 12 秒 | 198.3 ms | 218.7 ms | +10.3% | 192.5 ms |
| EAR 7.1.4 | 12 秒 | 236.2 ms | 274.8 ms | +16.3% | 217.1 ms |
| HOA 三阶 | 12 秒 | 266.4 ms | 293.2 ms | +10.1% | 268.2 ms |
| Triple Balance 7.1.4 | 1 秒 | 73.8 ms | 90.7 ms | +22.9% | 67.6 ms |

改归还位置后四项都回到迁移前的水平或更低，HOA 比迁移前高 0.7%。

## 原因

`Driver` 有两组（输入，输出）缓冲。`commit` 把含计量帧的块连同缓冲交给计量线程，计量线程按提交次序
测量；`render` 在当前槽为空时取回最旧的一组。修复前取回的缓冲放进第一个空槽
（`position(Option::is_none)`）：两组都在计量线程、当前槽为 1 时，取回的缓冲落入槽 0，
循环再取一次，等上一块刚提交的计量也测完。

| 渲染的块 | 槽 | 修复前取回 | 修复后取回 |
| --- | ---: | --- | --- |
| 0、1 | 0、1 | — | — |
| 2 | 0 | 块 0 | 块 0 |
| 3 | 1 | 块 1、块 2 | 块 1 |
| 4 | 0 | — | 块 2 |
| 5 | 1 | 块 3、块 4 | 块 3 |

修复前，从块 3 起每个奇数块都等到计量线程清空，偶数块的计量与下一块的渲染串行。
设每块的渲染（读入、混合、写出）耗时 R、计量耗时 M，修复后每块约为 max(R, M)，修复前每块平均多出
约 min(R, M) / 2，两者越接近回退越大。

迁移前的 C++ 循环（扬声器的 `render_speaker_pcm`、HOA 与 EAR 的 `render_window`）给每组缓冲各配一个
计量 future，只等这组缓冲上一次的计量，与修复后相同。修复前的取回方式随 Rust 离线驱动进入：
VBAP 与 Triple Balance 自 `abaea382`（4.5 C，C++ `render_speaker_pcm` 改为逐块驱动 Rust 驱动），
HOA 自 `c6ab4cd6`（4.5 E），EAR 自 `ea726502`（4.5 F）。5.2 把逐块驱动移进 `mradm-engine`，
驱动本身没有变化。

## 修复

`render` 在当前槽为空时只取回最旧的一组，放回当前槽；原来的 `while` 此后只会执行一次，改为 `if`。
两组缓冲尺寸相同、可以互换；最旧的一组在上一块渲染期间就能测量，刚提交的一块在本块渲染期间继续测量。
计量次序仍是提交次序，读数与内联计量相同。计量出错时在下一次取回或 `finish` 时报告，
与 C++ 按缓冲 future 报告的时机相同。

新增单元测试 `offline::tests::the_meter_measures_each_block_while_the_next_one_renders`：
每次 `render` 之后，上一块若有计量帧，它的计量必须仍未取回（未取回的计量恰为 1，否则为 0）。
取回会阻塞到结果返回，所以这个计数与线程时序无关。测试覆盖全部块计量、计量窗口与输出窗口三种情况，
并核对驱动的读数等于逐块内联计量的读数。恢复修复前的取回方式后，测试在块 3（帧 3072）失败。

## Linux 复核

4 vCPU 的 Linux x64 云主机，`cargo build --release --locked -p mradm-capi` 的 `mradm`：修复前为
`201592ca` 的无修改源码，修复后只改 `offline.rs`。素材由 `mr_adm_make_fixture` 生成，为 12 秒的
`objects-long` 与 1 秒的 `objects-room`。四个用例都是默认 WAV 输出，含峰值限制增益与 WAV 布局收尾。
每版预热一次后交替 12 轮，每轮反转顺序；表中为中位数，括号内为修复后更快的配对轮数。
“原样 CLI”指监视线程修复之前的 CLI。

| 用例 | 原样 CLI | 不起 Ctrl-C 监视线程 |
| --- | ---: | ---: |
| VBAP 7.1.4，12 秒 | 322.8 → 258.0 ms，−20.1%（12/12） | 301.3 → 264.4 ms，−12.3%（12/12） |
| EAR 7.1.4，12 秒 | 460.0 → 383.7 ms，−16.6%（12/12） | 447.0 → 358.5 ms，−19.8%（12/12） |
| HOA 三阶，12 秒 | 422.0 → 371.6 ms，−11.9%（11/12） | 445.8 → 366.6 ms，−17.8%（11/12） |
| Triple Balance 7.1.4，1 秒 | 56.3 → 56.9 ms，+1.0%（3/12） | 46.6 → 44.9 ms，−3.7%（8/12） |

1 秒的 Triple Balance 只有 47 块，按原因一节的估算节省几毫秒，与这台主机的轮间波动相当。不起监视线程
追加 40 轮为 42.9 → 39.1 ms（−8.8%），配对差中位数 −4.4 ms，31/40 轮更快。原样 CLI 下两版都落在
同一个 25 ms 节拍上（见下节），看不出差别。

每个用例两版的最终 PCM 位图（`mr_adm_pcm_bits`）及日志中测得的响度与 True Peak 都相同。
峰值 RSS 为 11.1–12.4 MiB，两版相差不超过 0.1 MiB。这台共享主机的绝对耗时在几组运行之间漂移，
只比较同一组内交替运行的两版。没有在本机运行一致性矩阵。

## CLI 的 Ctrl-C 监视线程

`mradm render` 在渲染期间起一个监视线程，每 25 ms 检查一次 SIGINT。原先渲染结束后 `thread::scope`
要等它醒来退出，所以每次渲染都在监视线程的下一个 25 ms 节拍上结束，多出 0–25 ms。6.4 之前的 C++ CLI
（`RenderInterruptScope`）同样每 25 ms 轮询并在析构时 join，参考版与 `201592ca` 都有这段等待，
它不是上面回退的原因。

它把完整 CLI 耗时取整到约 25 ms 的台阶，小于一个节拍的差异可能被抹平，也可能放大到一个节拍。
维护者表中 VBAP、HOA 与 Triple Balance 的回退和恢复都在 17–27 ms 之间，与一个节拍相当，
可能含有这种取整；方向与 Linux 复核一致。用计量修复后的同一程序交替 12 轮，1 秒 Triple Balance
带监视线程为 56.7 ms，不起监视线程为 38.5 ms，配对差中位数 −19.1 ms，12 轮全部更快。
12 秒用例的轮间波动大于这段等待，数据保留在证据中，不据此下结论。

### 修复（单独提交）

监视线程改用 `park_timeout` 等待（`mradm-cli` `watch_interrupt`）。渲染结束时，一个守卫置位结束标志并
`unpark` 唤醒它；渲染 panic 展开时守卫同样生效。release 构建是 `panic = "unwind"`，`thread::scope`
在重新抛出 panic 之前要等所有线程结束，所以原先渲染一旦 panic，CLI 会卡住，直到按下 Ctrl-C。
轮询间隔、日志行与退出码 130 都不变。

新增三个 `watch_interrupt` 单元测试：渲染结束后不等轮询，渲染 panic 后同样立即结束，Ctrl-C 取消渲染且只
报告一次。测试用 10 秒的轮询，假的 Ctrl-C 也在第 10 秒按下，所以修复回退时测试在 10 秒后失败，
不会卡住。改回 `sleep`、去掉 `unpark`、去掉守卫三种注入都被测试抓到。

- 正常渲染：VBAP、EAR、HOA 三阶、Triple Balance 与双耳五个用例的 `-v` 输出和最终 PCM 与修复前逐字节相同。
- Ctrl-C：在 EAR 渲染开始后 40–200 ms 发送 SIGINT，修复前后都以 130 退出，都输出
  `Ctrl-C received; cancelling render...` 与 `render cancelled`，也都不留下输出文件。
- 耗时：与计量修复后的程序交替 12 轮，1 秒 Triple Balance 由 57.7 ms 降到 45.6 ms，12 轮全部更快，
  配对差在 −20.7 到 −7.3 ms 之间，落在 0–25 ms 的等待之内。另一组交替中修复后为 44.3 ms，
  同一程序不起监视线程为 48.9 ms，没有可测的差别。12 秒用例的中位数快 1.2%–6.9%，在轮间波动之内。
- 这一组两版的峰值 RSS 相差约 1 MiB，差别全部来自映射的程序文件页（`RssFile`）；`201592ca`、
  计量修复与本修复三版的匿名内存都是 3.5 MiB，内存用量没有变化。

## 验证

计量修复：

- `cargo test --workspace --locked`：129 个测试程序共 998 项通过，1 项按原设置忽略。
- `cargo clippy --locked --workspace --all-targets -- -D warnings` 与 `cargo fmt --all -- --check` 通过。
- 没有修改 C/C++，`scripts/quality/language-surface.py --check` 通过。

监视线程修复：

- `cargo test --workspace --locked`：129 个测试程序共 1,001 项通过（新增 3 项），1 项按原设置忽略。
- `mradm-cli` 的 10 项二进制冒烟测试以 Release `mradm` 运行（`MRADM_EXE_PATH`），全部通过。
- clippy `-D warnings` 与 rustfmt 通过。

## 证据与复现

[evidence/offline-meter-overlap-20261124](evidence/offline-meter-overlap-20261124/) 包含：

- `bench.py`：交替运行器。每侧写作 `NAME=BINARY[,PRELOAD]`，记录逐轮耗时、GNU time 报告的峰值 RSS、
  PCM 位图与测得读数；两侧结果不同时报错退出。
- `no_sigint.c`：测量用的 `LD_PRELOAD` 垫片。它让 `signal(SIGINT, …)` 失败，CLI 因此不起监视线程，
  只多输出一条警告。
- `rss_split.py`：渲染时每 0.5 ms 读一次 `/proc/<pid>/status`，把峰值 RSS 分成匿名页（`RssAnon`）与
  文件页（`RssFile`）。
- `linux-x64-cli.json`、`linux-x64-no-watcher.json`、`linux-x64-triple-balance-40.json` 与
  `linux-x64-watcher-cost.json`：计量修复各组的逐轮数据，以及程序、垫片和素材的 SHA-256。
- `linux-x64-watcher-fix.json`、`linux-x64-woken-vs-no-watcher.json` 与 `linux-x64-rss-split.json`：
  监视线程修复的耗时对照与 RSS 拆分。

运行器需要 GNU time（`/usr/bin/time`）。两版 `mradm` 各自在 `rust/` 下用
`cargo build --release --locked -p mradm-capi` 构建，复制到 `<before>` 与 `<fixed>` 两个目录。
素材工具与 PCM 工具也在 `rust/` 下构建：

```bash
cargo build --release --locked -p mradm-testkit --example make_fixture --example pcm_bits
```

其余命令在仓库根目录运行：

```bash
mkdir -p fixtures && for kind in objects-long objects-room; do build/rust/release/examples/make_fixture $kind fixtures/$kind.wav; done
```

```bash
cc -O2 -shared -fPIC -o no_sigint.so docs/architecture/evidence/offline-meter-overlap-20261124/no_sigint.c -ldl
```

```bash
python3 -I docs/architecture/evidence/offline-meter-overlap-20261124/bench.py \
  --build before=<before>/mradm,no_sigint.so --build fixed=<fixed>/mradm,no_sigint.so \
  --fixtures fixtures --pcm-tool build/rust/release/examples/pcm_bits --output no-watcher.json
```

去掉两处 `,no_sigint.so` 即测原样 CLI；`--case` 只测指定用例，`--rounds` 改轮数。
