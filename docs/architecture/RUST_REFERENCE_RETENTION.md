# Rust 迁移参考实现的保留与退役

> 2026-10-06：落实 [ADR 0008](../adr/0008-rust-entry-and-saf-replacement.md)"按单元记录退出条件与移除参考实现的时机"。登记表为 [`tests/reference/retention.json`](../../tests/reference/retention.json)，由 `scripts/quality/check-reference-retention.py` 校验。

## 两类参考

每个迁移批次都会保留旧实现，用来比较 Rust 版本。参考分两类：

| 类别 | 位置 | 构建 | 风险 |
|---|---|---|---|
| 冻结参考（`frozen`） | `tests/reference/*_legacy.h`、`hoa/`、`live_vbap/`、`monitor/`、`triple_balance/`、`scene_numeric/`、`wav_container/` | 随 `MR_ADM_CORE_BUILD_TESTS` 默认编译，CI 每次运行 | 引用生产头文件，生产接口变化时需要跟着维护 |
| 第三方对照（`third-party`） | SAF / libear / libadm / libbw64 / libsamplerate / libebur128 / dr_wav 的参考测试 | 只在默认 OFF 的 `MR_ADM_BUILD_*_REFERENCE_TESTS` 下获取并构建 | 必需 CI 不运行，容易无声腐烂；由手动 `reference-tests.yml` 防腐 |

两类参考都**不得**作为运行时回退，也不得进入发行产物。

## 登记与冻结校验

`tests/reference/` 下每个文件必须属于登记表中的一个单元，并与登记的 SHA-256 一致。每个单元还登记以下内容：

- 迁移文档、接受日期、基线提交；
- 原有 `provenance.json`（如有）；
- CMake 开关、ctest 名称、依赖关系；
- 尚未满足的退役条件（`open_conditions`）。

`check-reference-retention.py` 在 ci 的 `version-metadata` job 运行。

- 冻结参考如需随生产接口调整，在同一提交更新登记哈希，并在对应迁移文档说明改动。不得借此修改被比较的算法。
- 各批次原有的 `provenance.json` 和 `evidence/*/validation.json` 是历史记录，不改写。新单元的来源信息直接写进登记表，`provenance` 留空即可。

## 退役条件

一个单元同时满足以下三条才可以删除。条件 ID 与 `retention.json` 的 `conditions` 字段对应。

1. **`released`**：该批次的 Rust 实现已随至少一个正式 `v*` tag 发布。截至 2026-10-06，最新 tag 为 `v1.9.1`，所有批次都还没有发布。
2. **`independent-regression`**：比较测试守护的行为已固化为不依赖旧实现的回归，并在必需 CI 运行。形式可以是 golden 输出或哈希、误差阈值，或运行时生成的 fixture。只证明"与旧实现一致"的测试不算。
3. **`phase2-cleared`**：二期跨平台逐位一致计划确定数值改动范围后，逐单元确认不再需要它作为行为对照。二期计划确定之前，所有单元都视为需要保留。

依赖顺序：`hoa` 和 `live_vbap` 引用 `scene_numeric/spatial.h`，所以 `scene_numeric` 必须最后退役。校验脚本拒绝"保留单元依赖已退役单元"。

第三方对照还有一条快速通道：如果在 `reference-tests.yml` 中失败，且修复成本高于它剩余的对照价值，可以只满足条件 1 就退役。退役记录里写明失败现象。

## 退役步骤

1. 删除该单元的参考文件、CMake 目标和开关。第三方对照还要同步删除以下内容并重新生成许可证文档：
   - `cmake/MRDependencies.cmake` 中的获取；
   - `third_party/manifest.json` 条目、`third_party/licenses/<name>/`、SBOM；
   - 质量脚本里的排除项。
2. 登记表保留该单元：`status` 改为 `retired`，填写 `retired_in`（删除提交），清空 `files` 和 `open_conditions`。`provenance` 保留历史路径，可从删除提交的父提交查阅；退役后不再要求该文件存在。`migration_doc` 仍须保留。
3. 在对应 `RUST_*_MIGRATION.md` 的"参考实现退出"小节记录删除提交、满足条件的证据（发布 tag、独立回归测试名、二期结论）。`evidence/` 保持不动。

## 当前状态

2026-10-07 [二期限定矩阵已结项](RUST_PHASE2_CLOSEOUT.md)。本次决定继续保留全部 16 个单元、
45 个文件，不批量解除登记表的三个退出条件。跨平台 PCM 相等不能替代各参考的旧算法行为、
容器或错误路径回归；独立 Monitor API 也不在该矩阵内。正式发布与逐单元退役审查仍在后续版本
周期完成，`released`、`independent-regression` 和 `phase2-cleared` 保持登记中的未解除状态。
二期技术结项不等于参考退役批准。逐单元的路径、测试和开关见登记表。

2026-10-06 在 Linux（GCC 13，Release，`55c9fa9` 之后的工作树）本地实测第三方对照：

- `libadm`、`libbw64`、`resampler`、`ebur128` 构建并通过。
- `saf` 编译失败：SAF 自身的 `saf_hrir.h` 在 `extern "C"` 内包含 C++ `<complex>`，GCC 报 "template with C linkage"（下文 `5dd0378` 的修复之前）。

[首轮参考 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37460398513)（`44c972b`）中，`libadm`、`libbw64`、`resampler`、`ebur128` 均通过；`saf` 在 Xcode 26.6 上也触发上述 C/C++ linkage 编译错误。修复方式是在测试入口显式提前包含 `<complex>`，让标准模板在进入 SAF 的 `extern "C"` 前加载，参考算法不变。SAF 继续使用原 macOS/Accelerate 数值验收环境。修复后的[第二轮参考 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37462536719)（`5dd0378`）五个单元全部通过（SAF 在 macos-26，其余在 ubuntu-24.04）。

另外，`tests/unit/hrtf_interpolation_test.cpp` 内联了一个旧 C++ 插值算法作为对照，不在 `tests/reference/` 下。它随所在测试一起维护，不单独登记。
