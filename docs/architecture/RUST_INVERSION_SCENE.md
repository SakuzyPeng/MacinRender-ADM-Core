# Rust 渲染场景（四期 4.3）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)四期 4.3。
> 本切片是结构迁移：锁定矩阵的 PCM id、内核文件与诊断检查点都要求与切片前逐位相同。

## 范围

输入到 `AdmScene` 的整条导入链迁入新 crate `rust/crates/mradm-scene`（`#![forbid(unsafe_code)]`）：

| 内容 | 原 C++ | Rust |
| --- | --- | --- |
| 扬声器布局表：标准与 Apple 两套几何 | `src/adm_render_common/speaker_layouts.cpp` | `layouts` |
| 声道床预设、自定义标签目录、标签解析 | `src/adm_io/channel_bed_importer.cpp` | `input` |
| WAVE 格式与 axml / chna 读取、CHNA 映射规则、ADM 投影的调用、声道床校验与场景合成 | `src/adm_io/scene_importer.cpp`、`channel_bed_importer.cpp` | `import` |
| 渲染场景检查点 `05-import` 的编码与写出 | `src/adm_engine/scene_checkpoint.h` 的导入一站 | `checkpoint` |

C++ 侧只剩适配：
- `io::import_scene` 把原生路径按现有约定转为 UTF-8，调用 `mradm_scene_import`；
- `metadata::scene_from_view` 由 Rust 场景的只读记录视图生成公共 `AdmScene`。导出回写以重新解析的源文档为基线，用的是同一个转换器；
- `io::input_layouts()`、`input_channel_catalog()`、`resolve_input_channel_labels()` 与 `render_layouts::speaker_layouts()` 的公共接口不变，改为 Rust 静态表的视图。
  调用方、Windows 岛与冻结参考都不改。

私有 FFI 头 `src/adm_render_common/layout_ffi.h`、`src/adm_metadata/scene_ffi.h` 已加入 `check-ffi-headers.py`。

### 保持不变的规则

- **路由。** 只有未指定布局（或为 `auto`）、未给自定义标签、且文件含 `axml` 时走 ADM，否则走声道床。
  强制声道床且文件含 AXML 时追加同一条警告。
- **CHNA。** trackIndex 为 0 报错。UID 去掉尾部空格与 NUL 后按 ASCII 大写，重复 UID 以最后一条为准，空 UID 忽略。
  映射按大写 UID 的字节序传给投影，投影再做规范化（前缀大写、十六进制小写）。
- **声道床。**
  - 1–64 声道；PCM16/24/32 或 float32；采样率与帧数非零。
  - 掩码的置位数必须等于声道数。
  - 7.1.4 预设的掩码顺序交换不变。
  - 目录跳过 0+2+0，位置容差 1e-4，同名不同仰角的标签生成 `label@仰角` token。
  - 合成的对象、轨道、pack 的 ID 与 LFE 的 120 Hz 低通不变。
- **错误。**
  - 检查顺序、错误码与消息逐字保留。
  - Rust 只返回定位的种类（无、`input=`、`path=`），C++ 用调用方的原生路径拼接，Windows 上报告的路径与改动前一致。
  - 唯一的差别：不是 UTF-8 的布局名或标签，错误消息中以替换字符显示。改动前按原字节显示，两者都不匹配任何预设。
- **溯源。** 声道床没有 ADM 溯源：`adm_source` 与 `source_position` 保持为空，与合成场景一致。

### `05-import` 检查点

`05-import` 由 Rust 场景按版本 1 编码，经 `mradm-dsp` `diagnostics::process` 写出，这是该写出器的第一个生产调用点。
- 编码逐字段沿用 C++ 视图的映射，包括导入从不设置的公共默认值（user level、覆盖标记）。
- 只有离线渲染的导入写出它（`io::import_scene_for_render`，`RenderService` 调用），与改动前相同；
  文件流式工具、预览与 inspect 的导入不写。
- `06-policy` 仍由 C++ 编码，4.4 语义策略迁入 Rust 时一并迁移。每个检查点名只归一方。

### 维护者决定（2026-10-17）

- **布局表提前到 4.3。** 声道床目录依赖它，先迁可以避免 C++→Rust 的临时数据方向。审计四期表 4.5 行相应删去布局表。
- **`RenderPlan` 的 Rust 场景句柄推迟到 4.4。** 4.3 时语义策略仍在 C++ 改写 `AdmScene`，句柄只能指向策略前的场景，
  与 `plan.scene` 不一致；4.4 把策略迁入 Rust 后，句柄与 `plan.scene` 一致。4.3 的句柄只在导入内部使用。

## 验证

**本机对照（Linux x64，config A）。** 基线为 `main` 的 `f277b64`，候选为 `a602277`（含布局表与导入迁移，生产代码与本记录提交相同）。
两者各做一次普通采集与诊断采集，采集自带的重复性与分块检查均通过：

- 283 个 PCM id、237 份内核测量逐字节相同；
- 诊断采集中 201 份内核检查点、30,772 个场景与渲染检查点全部相同。其中 79 个 `05-import` 现由 Rust 写出，
  恰好 5 个策略用例的 `06-policy` 与 `05-import` 不同，与 4.2 相同；
- 候选的普通与诊断采集 PCM 相同，诊断无扰动。两者只有记录构建选项的 `kernels/capabilities.txt` 不同，基线同样如此；
- 比较器 `compare-rust-phase2.py` 的 `validate()` 对两份候选输出均通过。

**CLI 行为。** 用 46 个用例对照基线与候选的 `mradm`，比较退出码、标准输出、标准错误与写出的策略模板：
- ADM fixture 的 `inspect` 与 `--write-semantic-policy-template`；
- 合成 WAVE 的 `inspect`：带 / 不带掩码、7.1.4 掩码、掩码与声道数不符、未知掩码、8 位、float32、零帧、
  非 WAVE、空文件、缺失文件；
- `input-layouts`；
- 声道床 `render` 的布局与标签选项：预设、未知布局、`AUTO`、别名、歧义、带仰角 token、空标签、布局与标签同时给出，
  以及对 ADM 文件强制声道床。

全部相同。渲染输出的 PCM 由上面的采集覆盖；CHNA trackIndex 0 与缺失 axml 由 `mradm-scene` 单元测试和既有 io 测试覆盖。

**布局表。** 新表由 C++ 旧表生成的规范转储 `rust/crates/mradm-scene/tests/layouts-v1.txt` 固定，标签、顺序、角度与范围的每个 f32
逐位相同（标准与 Apple 两套几何）。

**质量。**
- Debug ctest 80/80；
- `mr_adm_rust_unit_tests` 含 `mradm-scene` 的布局、导入与检查点测试；
- `mr_adm_rust_quality`（fmt + clippy）与 `mr_adm_ffi_header_check`（cbindgen 0.29.2，280 个函数、99 个结构体一致）；
- 变更 C++ 的 clang-format 与 clang-tidy；
- `check-licenses.sh`、`check-reference-retention.py`、`version_metadata.py`。

本机没有 cppcheck，留给 CI。

**规模。** 生产 C/C++ 由 45,026 行降到 44,632 行（布局表 −174，导入与声道床 −220）。

**三平台。** 按四期集中验收的约定，4.3 不单独触发；合入 `main` 后 push 触发的 Consistency 结果回填到本节。
