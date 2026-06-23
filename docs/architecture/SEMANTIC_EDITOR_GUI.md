# 语义策略编辑器 GUI(设计与实现规划)

## 1. 背景与目标

ADM 语义策略(`mradm.semantic-policy.v1`,见 `include/adm/semantic_policy.h`)目前只能通过手写
JSON + CLI(`--semantic-policy` / `--write-semantic-report`)使用,交互门槛高。本功能在现有
批渲染 GUI(Avalonia,`gui/MacinRender.Gui`)中新增一个**语义编辑模式/Tab**,让用户:

- 载入单个 ADM 文件,看到对象结构与各对象当前的语义取值;
- 用控件(非 JSON)逐对象或批量改写听感相关语义;
- 实时试听 + 看"前后数值 diff"验证;
- 单文件渲染落地。

**绝不让用户编辑 JSON**——这是交互层面的约束(用控件而非文本框),**不是**把 ADM 概念改名或包装:
术语忠于 ADM 原义(EN 直接用 `gain`/`diffuse`/`extent`/`divergence`,中文忠译 增益/扩散/范围/发散),
不重组成"空间感/包围感"之类的感知营销词——目标用户是懂 ADM 的专业用户。

## 2. 为什么是"单文档"

语义编辑改写的是**这一个文件的对象图**,policy 按 `id`/`name`/`track_uid` 匹配具体 audioObject,
必须先看到该文件的对象与当前语义值才能有意义地编辑。因此形态是**单文档工作台**(载入一个 → 编辑
→ 渲染),不是批渲染队列。这与批渲染模式互补,共存于同一 app。

## 3. 范围

**一期纳入(听感相关维度)**

| EN(原词) | 中文(忠译) | 子参数 |
|---|---|---|
| gain | 增益 | scale 缩放 · gain_db dB 偏移 · mute 静音 |
| diffuse | 扩散 | enabled 启用 · scale 缩放 · max 上限 |
| extent | 范围 | width/height/depth 宽/高/深 缩放 · max 上限 |
| divergence | 发散 | scale 缩放 · range_scale 范围缩放 · max_range 范围上限 |

**一期排除**:`position`(整个定位家族,含动态对象轨迹被压平的坑)、`channelLock`(声道锁定)、
`interpolation`(插值)、`directSpeakers`。匹配方式一期只做"树多选"(不做规则构建器)。

## 4. 语义维度 × 后端 行为参考(阈值/机制切换的事实清单)

这些维度并非线性平滑生效,过阈值会切换渲染机制,且**因后端而异**。事实来源:`render_common`、
`include/adm/scene.h` 的共享预处理,以及各后端 `capabilities()`。

### 4.1 总览

| | EAR | VBAP(SAF) | HOA | Binaural | Apple |
|---|---|---|---|---|---|
| gain | 线性乘 | 线性乘 | 线性乘 | 线性乘 | 线性乘;**linear≤0 → −120 dB 静音** |
| diffuse | ✅ BS.2127 去相关 | ❌ 忽略 | ✅ 32 向去相关场 | ✅ 单声道延迟去相关 | ❌ 忽略(SpatialMixer 无去相关器) |
| extent | ✅ 始终 | ⚠️ **仅 3D 布局**,2D→忽略 | ✅ 始终 | ✅ 始终(>1° 可走 SAF spreader) | ✅ 始终 |
| divergence | ✅ | ✅ | ✅ | ✅ | ✅(均经 `scene.h` 共享 3 源展开) |

`supports_diffuse`(后端级)与 per-layout `supports_spread` 已经在 `capabilities()` /
`adm_render_support_matrix_json` 里——"此维度在该后端/布局是否生效"可直接查,无需新代码。

### 4.2 各维度细节(确切阈值)

- **gain** —— 纯线性乘(`ev.gain = source.gain × obj.gain`),无机制切换。唯一拐点:`mute` 或
  Apple 上 linear≤0 → −120 dB(静音)。无需阈值提示,只需说明 mute。

- **diffuse** —— `>0` 接入去相关(声音从"定位清晰"变"弥散包围")。
  EAR=BS.2127(direct + diffuse 总线,FIR 去相关,补偿延迟 255;diffuse=1 → 全去相关无直达);
  HOA=32 向去相关场(延迟 1024);Binaural=单声道延迟去相关(延迟 32)。
  **VBAP / Apple 完全忽略**——最该提醒的后端分歧。

- **extent**(width/height/depth) —— 盘云半径(`render_common::extent_disk_radii`,所有非 VBAP 后端共用):
  `s = clamp(1/max(0.4, distance), 0.5, 2.5)`;`depth_r = depth×20°×s`;
  `width_r = width×60°×s + depth_r`;`height_r = height×45°×s + depth_r`。
  阈值:width_r 与 height_r 同时 ≤1e-4 → 退回单点。
  **VBAP 例外**:MDAP,`spread = min(180°, hypot(w,h,d))`,**仅 3D 布局**(`supports_spread = is_3d`),
  2D 布局忽略 extent。**Binaural 额外**:saf_spreader 模式下 extent **>1.0°**(`k_spreader_extent_threshold_deg`)
  才走 SAF 协方差 spreader,否则用 17 点云。

- **divergence** —— 共享展开(`scene.h::expand_object_divergence`,所有后端一致):
  阈值 `>1e-4` → 拆 **3 个源**:中心 + 左右 ±`divergence_azimuth_range`(钳 0–120°)。
  权重:中心 `(1−d)/(1+d)`,两侧各 `d/(1+d)`。**悬崖:d=1 → 中心权重归零,能量全到两侧**;
  d=0.5 → 三点均分。

## 5. GUI 设计

### 5.1 三栏心智模型

每个参数并排显示三栏,把抽象语义变成具体数字:

```
            当前值        覆盖               生效值
diffuse     0.30    →    [✓] scale ×0.5  →  0.15
extent W    20°     →    [ ] (不动)      →  20°
```

- 当前值:来自 **`adm_inspect_file_json`**(该文件每对象/每块的真实 ADM 取值)。一期取首个 object
  block 作为代表;动态(逐块变化)对象的多块差异是已知简化,后续细化。
  ⚠️ **注意**:`adm_policy_template_json` **不是**当前值来源——它是**中性可编辑骨架**(每对象的
  identity + 空覆盖槽,`build_semantic_policy_template` → `neutral_override_template`,applied
  unmodified 即 identity)。它只告诉你"哪些维度可编辑 / 序列化形状",不含实际维度值。
- 覆盖:用户设的 override(GUI 在内存里拼 policy JSON);
- 生效值:`adm_render_result_semantic_report_json`(apply 后读回)。

### 5.2 覆盖行 = `[☐ 覆盖此项] + [值]`

整个 policy 模型全是 `std::optional`(含三态 bool `enabled`)。统一控件:一行 = 勾选框(是否覆盖)
+ 值控件。不勾 = 不动、继承原值、整行变灰。这一个模式吃下所有 optional,一眼看出动了哪些。

### 5.3 目标选择 = 树多选(独立 + 批量)

- 左侧场景树:programme → content → object(+ HOA pack),标注 importance / dialogue;
- **单选** → 编辑该对象自己的参数(独立);
- **多选**(Ctrl/Shift/勾选)→ 批量模式,改任一参数应用到所有选中;**混合态感知**:选中值不一致
  的行显示"— / 混合",一改则统一覆盖,改完各对象 override 仍独立;
- **全选 / "全局"项** → policy 的 `global` / `all`。

数据模型:GUI 内部维护 `对象id → override 集` 映射;序列化为每个有 override 的对象一条 `objects[]`
id 规则(+ 可选 `global`),忠实、可与 report diff 一一对应。

### 5.4 验证闭环(试听 + diff,都做)

- **试听**:`adm_create_preview_session`(载入 + apply 内存 policy 一次)+ `adm_preview_render_window_v2`
  围绕播放头渲染几秒试听;编辑(防抖)后重建 session。听感聚焦下试听是主验证。
- **diff**:`adm_render_options_set_capture_semantic_report` + `adm_render_result_semantic_report_json`
  做前后数值对比(客观兜底)。

### 5.5 阈值/特殊效果提示

原则同 support-matrix:**不硬编进 GUI,从 core 出**。
- **一期(现成数据)**:用 `supports_diffuse` / per-layout `supports_spread` 做"**你设了但所选后端/布局
  不生效**"的警告(如 diffuse 在 VBAP/Apple 无效、extent 在 VBAP+2D 无效)。这是最有价值的提示,零核心改动。
- **二期(需新增)**:把 §4.2 的阈值数字/公式提炼成**声明式描述**,经一个新 C ABI 查询给 GUI,渲染为
  滑块阈值刻度 + 跨阈值高亮(随所选后端联动);进一步由 semantic report 标注"实际触发了什么"。

## 6. C ABI 依赖(一期全部现成)

| 用途 | 入口 | 绑定状态 |
|---|---|---|
| 场景树 + **各对象当前值** | `adm_inspect_file_json`(权威源:对象身份 + 逐块维度实际值) | ✅ 已绑 |
| 中性可编辑骨架(可选,非当前值) | `adm_policy_template_json` | ✅ 已绑 |
| 内存策略 | `adm_render_options_set_semantic_policy_json` | ✅ 已绑 |
| 生效报告 | `adm_render_options_set_capture_semantic_report` + `adm_render_result_semantic_report_json` | ✅ 已绑 |
| 试听 | `adm_create_preview_session` / `adm_preview_render_window_v2` / `adm_destroy_preview_session` | ✅ 已绑 |
| 后端/布局能力 | `adm_render_support_matrix_json` / `adm_capabilities_json` | ✅ 已绑 |
| 渲染落地 | `adm_render_file_ex2`(复用) | ✅ 已绑 |

**结论:一期不需要任何 core / C ABI 新增,只在 GUI 侧补 P/Invoke 绑定 + 新视图。** 阈值描述符是二期的核心改动。

## 7. 实现阶段(概要)

1. **P/Invoke 补绑**:`adm_inspect_file_json`、`adm_policy_template_json`、capture/report、preview session 三件套。
2. **模式导航**:MainWindow 引入"批渲染 / 语义编辑"切换(首次多模式)。
3. **场景树 + 当前值**:载入 → `inspect_file_json` 建树**并填当前值**(对象身份 + 逐块维度实际值,
   一期取首块代表)。`policy_template_json` 仅作可选的"可编辑维度/序列化形状"参考,不取值。
4. **编辑器(三栏 + 覆盖行)**:gain/diffuse/extent/divergence 卡片;内存拼 policy JSON;多选/混合态。
5. **生效回填 + diff 面板**:capture report 读回,前后对比。
6. **试听**:preview session + 内存 policy,播放头窗口试听,编辑防抖重建。
7. **渲染落地**:复用 support-matrix 的格式/布局选择,单文件渲染。
8. **后端不生效提示(现成数据)**:用 supports_diffuse / supports_spread 做警告。
9. **(二期)阈值描述符**:core 新增声明式阈值/效果查询 + GUI 刻度高亮 + report 实测标注。

i18n、AOT 约束沿用现有 GUI 既有做法(运行时字典 + DynamicResource)。
