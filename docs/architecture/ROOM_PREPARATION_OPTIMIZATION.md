# ROOM 准备优化与 Windows 验证顺序

日期：2026-10-18。对照基线：`718a80495b812dcd442b8f5109dfd595afb1d1d5`。

## 采用范围

本次只采用加载准备与验证工具改动。运行时保持 `room-realtime-v2`；exact 的数值契约、
声学模型、距离层、完整 FIR、事件更新、淡化、FIFO、延迟和尾声均不变。

- HRIR 时延估计按连续行分组，使用全进程最多四线程的专用准备池。每个任务拥有独立 scratch，
  每行沿用原有 f64 运算顺序；少于 128 行时串行处理。结果与首个错误按原行序返回。
  准备池不使用音频 worker，线程创建失败不永久缓存失败状态。
- 距离集合未命中时只读取、认证每层一次，并把已经认证的 SHA-256 交给 SOFA 准备步骤。
  活动弱缓存按索引内容、处理采样率、展示名称和来源报告隔离；命中仍逐层检查长度和 SHA-256。
  读取和计算都在缓存锁外执行，活动引用释放后数据可回收，不建立磁盘缓存。
- 已准备房间的缓存身份包含集合报告信息，避免旧的展示名称或来源报告被复用；重计算不占用缓存锁。
- 保留 Windows 线程 CPU 计时、路径传递／响应变换剖析、原生资源回放入口、完整 realtime 数值比较、
  加载计时和零超时验收工具。诊断计时只在相应 feature 开启时执行。

同轮 realtime v3 融合查询、相位表和 SIMD 卷积候选已撤回。Windows 96 kHz／512 帧／32 源／
8 workers 的完整会话短测中，位置／距离运动均值分别为 12.10／11.07 ms，均为 1024/1024 次
超过 5.333333… ms。该结果足以终止候选，无需继续长测。错误 AVX 候选的性能记录不作为证据。
独立子带、HOA 和 DVF 研究不包含在本次采用范围内。

## 验证

Windows 使用 canonical MSVC/Ninja 构建与既有 Cargo 缓存，验证暂存区的完整源码；
研究工作区中的额外代码不参与该构建。加载与缓存的定向测试覆盖逐位结果、篡改／删除检测、
来源报告更新、名称／采样率隔离、并发加载和弱引用回收。原有 ROOM 集成测试继续检查时序、
尾声、质量档位和处理阶段零分配。

准备计时与运行时验收分别报告。“冷准备”表示没有存活的准备句柄，保留操作系统文件缓存；
活动命中保持第一个句柄存活，每次命中仍读取和认证全部层。私有 C 边界计时包含 HRIR 准备与
PreparedSet 构造，房间配置加载另计；不能与公开 Scene 创建的绝对时间混合比较。

拆分后的测试与加载数据记录在 [validation.json](evidence/room-preparation/validation.json)。
本次不声明解决完整 ROOM 的 Windows 零超时目标。

Windows 拆分后的加载短测，每个采样率各一对基线／候选，单位为秒：

| 采样率 | 冷准备：基线 → 候选 | 活动缓存命中：基线 → 候选 |
|---|---:|---:|
| 48 kHz | 14.307 → 10.327 | 14.643 → 0.409 |
| 96 kHz | 18.477 → 9.932 | 17.845 → 0.384 |

两项加载短测均有收益；单轮数据不承诺稳定的加速幅度。计时期间没有本任务的并行构建或其他基准，
加载计时不代替运行时零超时验收。Windows 的 48／96 kHz × exact／realtime 四组
完整回放 manifest 与各 17 份载荷哈希均与同机基线相同，包括完整尾声、帧数和延迟。
Windows 原生定向测试、工具回归和 canonical 构建后的 4 项 CTest 通过。
随后 macOS arm64 的同类定向测试、clippy、Release 构建与 4 项 CTest 通过；
48／96 kHz × exact／realtime 四组完整 manifest 与各 17 份载荷也逐位相同。
暂存的 8 个 Rust 文件通过格式检查。Windows 原工作区的 HEAD 和 Git 状态已完整恢复，
临时 SOFA 与验证目录已删除，原有构建缓存保留。

## 复现

使用现有 Cargo target，不新建依赖缓存。Windows 设置 canonical MSVC／SDK 环境，追加
`--target x86_64-pc-windows-msvc --target-dir <existing-cargo-target>`；macOS 同样指定既有 target。
从 `rust/` 运行定向正确性检查：

```sh
cargo test --locked --no-default-features -p mradm-dsp --lib room::
cargo test --locked --no-default-features -p mradm-ffi --lib room::native_sofa_tests
cargo test --locked --no-default-features -p mradm-dsp --test room --test room_distance --test room_quality --test room_sample_rate --test room_no_alloc
cargo test --locked --no-default-features -p mradm-dsp --features room-profile --test room_profile
cargo build --release --locked --no-default-features -p mradm-ffi --example room_prepare_bench
cargo build --release --locked --no-default-features -p mradm-dsp --example room_replay
```

用 Release 探针配对测量同一份资源：

```sh
room_prepare_bench ROOM_JSON COLLECTION 1
room_replay ROOM_JSON NEW_OUTPUT_DIR --hrir COLLECTION --quality exact --dense --screens --head-motion wrap
room_replay ROOM_JSON ANOTHER_OUTPUT_DIR --hrir COLLECTION --quality realtime --dense --screens --head-motion wrap
```

本次只改变加载，两个质量档位均可直接比较完整 manifest 和每个载荷的 SHA-256，
要求与同机基线一致。未来 realtime 算术候选才按 ADR 0020 的舍入门槛使用 `compare-realtime.py`；
exact 不放宽。工具回归从仓库根目录运行：

```sh
python3 -B tests/unit/room_realtime_tools_test.py
cmake --build build/win-canon --target mr_adm_room_tests mradm_exe mr_adm_c_api_header_smoke --parallel 8
ctest --test-dir build/win-canon -R "^(mr_adm_room_tests|mr_adm_room_96k_tests|mr_adm_room_realtime_tools_tests|mr_adm_c_api_header_smoke)$" --parallel 8 --output-on-failure
```

## 后续候选的停止条件

开发、代码修改与常规正确性检查默认在本地完成；Windows 优先仅指性能筛选与验收顺序。
进入性能筛选时，先补齐必要的 Windows 原生构建与烟测，再做无插桩 Release 完整会话短测。ROOM 运行时门槛固定
8 workers、96 kHz、512 帧、32 源，位置和距离运动各 1024 次调用，要求零次超时。
短测失败就停止该候选，不继续 macOS 性能矩阵或六轮长测；新候选重新从 Windows 短测开始。
短测通过后才进入适用的跨平台与长测验收。隔离链的加速、加载收益都不能代替完整会话达标。
