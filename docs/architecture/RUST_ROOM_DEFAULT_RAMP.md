# room 默认 ramp 按采样率换算（五期 5.7）

> 五期 5.7 的记录（2026-11-06 至 2026-11-08）。路线见[审计五期一节](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)。
> 本切片是有意的数值变化：非 48 kHz 的 room 离线与文件流式渲染中，没有 `interpolationLength` 的对象块的默认 ramp
> 改为正确长度。48 kHz 下结果不变，锁定矩阵已有的 291 个 PCM id 不变。

## 范围与原因

room 后端在离线与文件流式渲染时为对象块建事件表（`rust/crates/mradm-render/src/room.rs`）。块没有 `interpolationLength`
又不是 jump 时，位置与增益在 `default_interp_ms` 毫秒内过渡。旧 C++ 把这段时长写成 `default_interp_ms × 48` 帧，
四期 4.6 迁入 Rust 时照原样保留，记为已知问题（[4.6 记录](RUST_INVERSION_TRIPLE_ROOM.md#发布说明)发布说明第 2 项），
四期结项时移交五期（[四期结项](RUST_INVERSION_CLOSEOUT.md#开放问题移交五期)）。

`× 48` 只在 48 kHz 下正确：96 kHz 下 ramp 只有应有长度的一半，44.1 kHz 下又偏长。其他后端（VBAP、HOA、EAR 的离线驱动与文件流式，
Triple Balance 文件流式）都按 `sample_rate × ms / 1000` 换算。

用户决定（2026-11-06）：当期的问题当期处理，已知问题不再往后拖。5.7 因此提前到 5.4 之前完成，并加 96 kHz room 用例进矩阵、设为门禁。

## 修法

- `room.rs` 的默认 ramp 改为 `sample_rate × default_interp_ms / 1000`（u64 整数除法，向下取整），`sample_rate` 是准备好的房间采样率；
  准备阶段已检查它与场景采样率相同。
- 有 `interpolationLength` 的块、jump 块与第一个块不变。
- Scene 流式的 room 不受影响：它的 ramp 来自 producer 的更新，不经过事件表。
- 48 kHz 下 `48000 × ms / 1000 = 48 × ms`，与旧写法逐位相同。

## 新增用例与门禁

- **96 kHz 房间包** `tests/fixtures/room/phase2-96k/`：由 `tests/tools/make_room_phase2_fixtures.py --derive-96k` 从 48 kHz 包派生，
  9 个资产文件逐字节相同，只有 `room.json` 与 `hrir.json` 的 `sample_rate` 改为 96000；一致性采集照 48 kHz 包的方式校验摘要并复制。
- **夹具** `objects-room-96k`（`tests/tools/make_fixture.cpp`）：沿用 `objects-room` 的四个移动块，采样率 96000，块上没有
  `interpolationLength`，帧数加倍以保持时长。
- **3 个 PCM id**，在 `scripts/consistency/phase2-gates.json` 设为门禁：
  - 离线 `room-exact-96k`（exact 档，输出 102,012 帧，含 6,012 帧尾声）；
  - 文件流式重放 `stream-room-96k-fixed` 与 `stream-room-96k-fragmented`（版本 4，事件与 `stream-room-*` 相同）。
- 计数：PCM id 291 → 294，离线场景 79 → 80，文件流式重放 36 → 38。`phase2_tools_test.py` 的计数、夹具清单与 room 包校验同步。

## 发布说明

1. **room 的默认 ramp。** 非 48 kHz 的 room 离线与文件流式渲染中，没有 `interpolationLength` 的对象块的位置与增益过渡改为
   `default_interp_ms` 毫秒（96 kHz 下是旧版的两倍长，44.1 kHz 下变短）。这类节目的 PCM 改变；48 kHz 的输出、Scene 流式 room 与其他后端不变。
2. **构建：libFLAC 的链接顺序。** main `203fb01` 让 `mradm-ffi` 调用 libFLAC 后，GNU ld 单遍解析时 `libFLAC.a` 排在
   `libmradm_ffi.a` 之前，Linux 链接失败。
   - `fc812c2`：把 FLAC 依赖挂到 Rust 静态库目标 `mradm_ffi-static` 上，修好 CMake 链接的目标；
   - `4d901ff`（维护者）：Cargo 自己链接的 examples 与测试（含 `mr_adm_phase2_*` 工具）也链接 CMake 选定的 FLAC，经
     `mradm-ffi/build.rs` 读取 `MR_ADM_FLAC_LINK_FILE`。`fc812c2` 没有覆盖这一部分，合并点的 Windows 构建因此失败；
   - `750e3e8`（维护者）：Windows 上的 PCM 工具保留 Unicode 路径；
   - `5bb28ce`（维护者）：声明 FLAC 的 Windows 分配函数。

## 验证

本机对照在 Linux x64、config A 的 Release 构建上进行，基线是 main `203fb01` 加上同一处链接修正。

| 对照 | 结果 |
| --- | --- |
| Rust `tests/room.rs`：44.1 / 48 / 96 kHz 与不同时长下的默认 ramp 长度，块的第一段无 ramp | 9 个测试通过；恢复 `× 48` 与改为向上取整两处植入缺陷都被抓到 |
| 现有 115 个离线与文件流式行，plain 与 diag | 逐字节相同，diag 下 12,258 个检查点相同 |
| 3 个新 id 与基线（旧 ramp）比较 | 从第 24,320 帧起不同：第二个块的起点（第 24,000 帧）加上房间的传播延迟；两遍输出相同，固定与碎片分块逐位相同 |
| 完整 plain 采集 | 294 个输出，`validate()` 通过 |

- **常规检查**：ctest 83/83；`mr_adm_rust_quality`、一致性工具测试、clang-format / clang-tidy、retention、version 与棘轮都通过。
- **维护者合入**：5.7 A 合入为 `6a9673d`（与 `ae69291` 树相同），之后是发布说明第 2 项的三个修正。
- **Consistency**：

  | 提交 | 运行 | 结果 |
  | --- | --- | --- |
  | `6a9673d`（合并点） | 42375717762 | Windows config A / B 构建失败（Cargo 链接的工具缺 FLAC），Compare platforms 跳过 |
  | `4d901ff` | 42377820416 | 通过 |
  | `750e3e8` | 42379461985 | 通过 |
  | `5bb28ce` | 42381474098 | 通过；Compare platforms 的 config A、B 普通与诊断四份报告都是 294 个 PCM id 相同、0 个不同、平台集完整、门禁失败 0 |

## 规模

生产 C/C++ 不变（29,260 行，`language-surface.py`）。测试区只有 `make_fixture.cpp` 增加了 96 kHz 夹具的参数。
