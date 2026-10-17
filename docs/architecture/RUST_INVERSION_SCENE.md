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

待本机对照完成后填写。

**三平台。** 按四期集中验收的约定，4.3 不单独触发；合入 `main` 后 push 触发的 Consistency 结果回填到本节。
