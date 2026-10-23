# 语义策略与参数 JSON 迁入 Rust（四期 4.4）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)四期 4.4。
> 本切片以结构迁移为主：报告、模板、导出文件与错误消息都要求与迁移前逐字节相同。
> 有意的行为变化单独列在[发布说明](#发布说明)；其中只有 extent 缺省上限一项会改变 PCM。

## 范围

语义策略的解析、匹配、合并、应用、报告与模板，Scene 流式渲染的逐更新应用，导出回写，以及 DirectSpeakers 矩阵与屏幕配置的 JSON 解析迁入 Rust。项目内的 JSON 读写改用新 crate `rust/crates/mradm-json`。

| 内容 | 原 C++ | Rust |
| --- | --- | --- |
| JSON 解析与 `dump(indent)` | nlohmann/json 3.12.0 | `mradm-json`（`#![forbid(unsafe_code)]`，MIT，移植自 nlohmann 的接受规则与 Grisu2 格式化） |
| 离线策略：解析、匹配、合并、布尔字段、HOA 与 DirectSpeakers 过滤、未匹配警告 | `src/adm_core/semantic_policy.cpp` | `mradm-scene` `policy`（数值仍由 `mradm-dsp` `semantic_policy` 计算） |
| 语义报告与中性模板 | 同上（nlohmann `dump(2)`） | `mradm-scene` `report` |
| `06-policy` 场景检查点 | `src/adm_engine/scene_checkpoint.h` | `mradm-scene` `checkpoint` |
| Scene 流式的身份合并、逐代解析与逐元素应用 | `scene_stream_engine.cpp` 的 `build_policy_identities` / `apply_policy_to_state` | `mradm-scene` `live_policy`（不分配内存） |
| 导出回写的差分与 AXML 修补 | `src/adm_metadata/metadata.cpp` 的 `collect_patches` / `patch_axml` | `mradm-scene` `writeback`（修补仍由 `mradm-adm` `Document::apply_patches` 完成） |
| DirectSpeakers 矩阵 JSON | `src/adm_core/direct_speakers_matrix.cpp` | `mradm-scene` `direct_matrix` |
| 屏幕配置 JSON | `src/adm_core/screen.cpp` | `mradm-scene` `screen`（几何校验仍走 `mradm-dsp` room） |

渲染场景（`RenderScene`）在记录之外增加了一层覆盖，保存记录里没有的状态：
- 对象的 user level 和有效静音；
- DirectSpeakers 块的 user level、位置覆盖与有效 `has_position`。

作者写入的 `mute` 与 `has_position` 仍留在记录里，它们决定 `adm_source` 与 `source_position`。报告需要策略前的场景，所以另外保留一份。

C++ 侧只剩适配：
- `RenderService` 对 Rust 场景句柄应用策略后重建 `AdmScene`。`RenderPlan::scene_handle` 指向策略后的场景；调用方传入预导入场景时（预览与监听）为空。
- `SceneStreamEngine` 经 `mradm_live_policy_*` 解析、解析到元素并逐更新应用。诊断文本仍由 C++ 拼接。
- `io::RenderScene::write` 负责导出的文件编排：`get_axml`、`replace_wav_axml`，以及 `input=… output=…` 定位。
- `parse_direct_speakers_matrix` 与 `parse_screen_config_json` 调 Rust，再把结果复制进原有的 C++ 结构体。文件读取、`validate_screen_config`、`resolve_reference_screen` 都不变。
- 策略文件与配置文件仍由 C++ 读取。

删除的 C++：
- `semantic_policy.cpp` 与 `include/adm/semantic_policy.h`；
- `scene_checkpoint.h`；
- `io::write_scene`；
- `metadata::patch_axml` 及其辅助代码；
- 两个解析器里的 nlohmann 代码。

不再有调用者的私有 FFI 也删了：`mradm_adm_parse` / `view` / `destroy` / `patch`，以及 `mradm_dsp_policy_scene_state`。ADMCore 不再依赖 nlohmann；engine、C ABI 等输出 JSON 的模块仍在用它。

私有 FFI 新增：
- `src/adm_metadata/scene_ffi.h` 扩展了策略、报告、模板、检查点与导出入口；
- 新头 `src/adm_realtime/live_policy_ffi.h`；
- 新头 `src/adm_core/config_ffi.h`。

两个新头都用相对路径包含所依赖的头，已加入 `check-ffi-headers.py`。

### 保持不变的规则

- **消息。**
  - 检查顺序、错误码、context 与字段级消息逐字保留；
  - 未知键报按字节序排在第一个的；
  - nlohmann 的 302、304、403 与 406 文本照原样嵌入。
- **数值。**
  - 浮点字段先转 f32，再检查是否有限；
  - 整数到 f32 直接转换；
  - 矩阵权重的和按 double 累加，gain 为 `(float)sqrt(weight/sum)`；
  - 屏幕配置的 `version` 按数值比较，`1.0` 与 `1` 都接受。
- **字节。** 报告与模板与 nlohmann `dump(2)` 逐字节相同：键按字节序，非 ASCII 原样输出，f32 扩展为 double，数字用 Grisu2 格式化。
- **导出。** 字段映射、一致性检查及其顺序与消息都不变。位置、改投向、user level 与 HOA pack 增益照旧不写回。
- **检查点。** 每个检查点名只归一方：`05-import` 与 `06-policy` 都由 Rust 写出。

### 维护者决定

2026-10-17：
- 只有 JSON 语法错误的正文改用 Rust 的描述，前缀、错误码与 context 不变。
- 报告的两处花括号缺陷先在 C++ 修正（A），再逐字节迁移。
- 报告与模板文件三平台统一写 LF。
- 导出回写并入 4.4。

2026-10-23：extent 缺省上限改为 ADM 取值范围，中性模板不再写 `max`，详见下一节。

## 发布说明

仓库没有单独的发布说明文件，4.4 的用户可见变化记在这里：

1. **extent 缺省上限（改变 PCM）。**
   - 启用 extent 缩放但没写 `max` 时，原来一律钳到 1；极坐标的 width 与 height 以度为单位，所以 `{"extent":{"scale":0.5}}` 会把 30° 压到 1°。现在按 ADM 取值范围钳制：极坐标 width、height ≤ 360，depth ≤ 1；Cartesian 三项 ≤ 1，与原来相同。
   - 显式写的 `max` 照旧生效，单位与块的坐标系一致。
   - 中性模板不再写 `"max": 1`，套用后严格恒等。
   - GUI 语义编辑器从来不写 `max`，此前同样受影响。离线、Scene 流式与导出共用这条规则；GUI 预览与双耳监听的实时 extent 覆盖已按同一范围对齐（`7ea6580`）。
2. **JSON 语法错误。** 策略、DirectSpeakers 矩阵与屏幕配置的语法错误正文改为 `JSON 语法错误（第 N 行第 M 列）：…`，前缀与错误码不变。
3. **报告修正（A，`97597a8`）。**
   - `policy` 写成字符串或 `null`，原来是 `["path"]`；
   - HOA 无界块的 `end_sample` 写 `null`，原来是 `[null]`；
   - `1e400` 这类数值溢出现在返回 invalid_argument。原来 406 异常会逃出策略解析。
4. **换行。** Windows 上的报告与模板文件改为 LF。
5. **Triple Balance 预览。** 给了报告路径时，不再写出以空原场景为基准的退化报告。
6. **Scene 流式身份冲突。** 改为按首次出现的顺序报告。原来取决于 `unordered_map` 的迭代顺序，各平台可能不同。生产者传入的非 UTF-8 身份字符串按替换字符比较，只影响 glob 的 `?` 与非法名字的冲突检测。
7. **删除的头与接口。** 删除了 `include/adm/semantic_policy.h` 及其安装规则，以及 `include/adm/io.h` 的 `io::write_scene`。按 ADR 0021 §4，`c_api.h` 以外的头不是对外接口；C ABI 签名不变。

## 黄金数据

每份黄金数据都在删除旧实现之前，由 scratchpad 中的临时程序生成（不提交）。生成器链接当时的 C++ 或 nlohmann，摘要为 FNV-1a 64。

| 语料 | 生成方 | 规模 | 结果 |
| --- | --- | --- | --- |
| `mradm-json/tests/golden/` | nlohmann 3.12.0，输入是同一套 splitmix64 流 | 400 万个随机 double 与扩展后的 f32 位模式、十进制边界邻值、约 95 条手写的接受边界、20 万份变异文档、30 万段随机数字文本 | 全部一致；移动定点阈值会让四个测试中的三个失败 |
| `mradm-scene/tests/policy-v1/` | C 之前的 C++（含 A 的修正）与 C++ `06-policy` 编码器 | 14 个 ADM 场景上的 714 例：五份一致性策略与固定种子的随机策略（554 例应用、160 例拒绝），外加 14 个模板 | 06 字流、报告字节、警告、模板与错误文本一致；植入的两处缺陷被抓到 |
| `mradm-scene/tests/export-v1/` | `c0a3720` 的 C++ `patch_axml` | 14 个无策略场景、714 例 policy-v1，以及两个导出专用场景（common definitions、共享块）上的 12 例，共 742 条 | 283 份 AXML 有改动、20 例报错、160 例策略被拒，逐字节复现；植入的字段交换被抓到 |
| `mradm-scene/tests/config-v1/` | `5db7fd4` 的 C++ 矩阵与屏幕配置解析器 | 958 条输入：合法文档的逐处变异、数值边界、version 各种写法、非法几何、1 MiB 边界 | 227 条成功、731 条报错，逐条一致；植入的两处缺陷被抓到（为此补了 centre 中多个非法元素同时出现的用例） |

语法错误只比较错误码与前缀。policy-v1 中有两个 NaN 字，测试先断言它们是 NaN 再规范化，与 NaN 的符号位无关（`04e04ca`）。

extent 缺省上限修复后，下面这些记录改由 Rust 重算，并标记为 `"regenerated":"extent-adm-range"`：
- policy-v1 的 12 例与 14 个模板；
- export-v1 的 11 条，导出改动数随之由 283 变为 282。

这些记录都在三个使用极坐标度数 extent 的场景上（objects-extent、objects-extent-multi、policy-objects），其余记录仍是 C++ 的原值。重算前先用旧规则跑了一遍对照，结果与已有记录完全一致。

## 验证

**逐批检查（Linux x64，提交前）。**
- Debug ctest 全部通过（80 → 83 个测试）；
- `mr_adm_rust_unit_tests`；
- `mr_adm_rust_quality`（fmt + clippy `-D warnings`）；
- `mr_adm_ffi_header_check`（cbindgen 0.29.2；F 之后为 291 个函数、110 个结构体）；
- 变更 C++ 的 clang-format 与 clang-tidy；
- `check-reference-retention.py`、`version_metadata.py --check`、`language-surface.py`。

本机没有 cppcheck，由 CI 运行；维护者补的头路径修正 `5db7fd4` 就来自它。

**新旧路径对照（D，临时程序，不提交）。** 旧的 C++ Scene 流式策略与新的 Rust 路径同时运行：
- 540 份可解析策略、14,872 个随机生成与 247,016 个生产者状态逐位相同；
- 6,728 个身份冲突的错误码一致，其中 1,082 个按新顺序报出了另一个冲突对象；
- 植入的探测缺陷被抓到。

**本机全量对照（维护者，每批合入前）。** 每批推送后，由维护者在本机对照候选与基线。
- E 的合并提交 `8fff2a8` 记录了 macOS arm64 与 Windows x64 上的结果：
  - 16 次 Release 采集的 PCM、内核与检查点，在两个版本、两种配置、两个平台之间都相同；
  - 742 个导出用例在四种模式下共 11,872 次调用，结果一致。
- A–C、D、F 与 extent 修复同样经维护者对照通过后合入。extent 修复预期只改变用到极坐标度数 extent 且未写 `max` 的用例。

**维护者补充的修正。**
- `04e04ca`：策略黄金数据与 NaN 符号无关；
- `f42749f`：用 `.gitattributes` 保护黄金字节，并修正 cppcheck 发现；
- `5db7fd4`：Scene 流式策略头改为自包含；
- `9472a74`：配置 JSON 的错误消息按长度传递、保留 NUL，根值之后的 NUL 按 nlohmann 的规则视为输入结束；
- `7ea6580`：GUI 预览与双耳监听的 extent 范围与策略一致。

**规模。** 生产 C/C++ 由 44,632 行降到 42,406 行：

| 批次 | 行数 |
| --- | --- |
| C | 44,632 → 44,213 |
| D | 44,208 → 42,748（合入前 `main` 另有改动） |
| E | 42,748 → 42,512 |
| F | 42,512 → 42,409，`9472a74` 后为 42,406 |
| A、B 与 extent 修复 | 不变 |

**三平台。** 每次合入 `main` 后，Consistency 由 push 触发，在 macOS arm64、Linux x64、Windows x64 上采集 config A、B 的普通与诊断结果。

| 运行 | 提交 | 内容 | 结果 |
| --- | --- | --- | --- |
| [39031059419](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/39031059419) | `294415d` | A–C | 四份比较报告均为 283 个 PCM id 相同、0 个不同、平台集完整、门禁失败 0 |
| [39824671399](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/39824671399) | `8fff2a8` | D、E | 同上 |
| [39891455786](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/39891455786) | `cdc23c8` | F | 同上 |
| [39908037480](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/39908037480) | `6ed2255` | extent 修复 | 运行中，结果待回填 |
