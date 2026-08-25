# 普通多声道输入语义

[English](CHANNEL_BED_INPUT.en.md) | 中文

`mradm render` 除 ADM BWF / BW64 外，也接受没有 ADM 元数据的声道式
WAVE、RF64 和 BW64 文件。支持 PCM 16/24/32-bit 与 IEEE float32，声道数限制为
1–64。输入声道会按本页规则合成为一个 DirectSpeakers 场景，再交给所选渲染后端；
因此同一输入既可输出多声道或 HOA，也可输出双耳。

## 检测优先级

未指定输入映射时使用 `auto`：

1. 文件包含 `axml` chunk 时按 ADM 导入；ADM 无效会直接报错，不会静默降级为普通多声道。
2. 文件没有 `axml` 时，读取 WAVEFORMATEXTENSIBLE channel mask；仅识别下表带 mask 的布局。
3. mask 缺失、mask 位数与文件声道数不一致，或 mask 未被识别时，要求显式传
   `--input-layout` 或 `--input-channels`。

显式输入映射会强制按普通多声道处理。如果文件同时含有 `axml`，渲染会忽略它并给出警告。
`--input-layout` 与 `--input-channels` 互斥。

## 预设布局与文件声道顺序

下列顺序是显式 `--input-layout` 的逐声道文件顺序，不会按名称猜测或重排：

| `--input-layout` | 声道数 | 自动 mask | 文件声道顺序 |
|---|---:|---:|---|
| `5.1` | 6 | `0x003F` | `M+030 M-030 M+000 LFE1 M+110 M-110` |
| `5.1.2` | 8 | `0x503F` | `M+030 M-030 M+000 LFE1 M+110 M-110 U+030 U-030` |
| `7.1` | 8 | `0x063F` | `M+030 M-030 M+000 LFE1 M+135 M-135 M+090 M-090` |
| `5.1.4` | 10 | `0x2D03F` | `M+030 M-030 M+000 LFE1 M+110 M-110 U+030 U-030 U+110 U-110` |
| `7.1.4` | 12 | `0x2D63F` | `M+030 M-030 M+000 LFE1 M+135 M-135 M+090 M-090 U+045 U-045 U+135 U-135` |
| `9.1.4` | 14 | — | `M+030 M-030 M+000 LFE1 M+110 M-110 M+150 M-150 M+070 M-070 U+070 U-070 U+150 U-150` |
| `9.1.6` | 16 | — | `M+030 M-030 M+000 LFE1 M+110 M-110 M+150 M-150 M+070 M-070 U+070 U-070 U+110 U-110 U+150 U-150` |
| `22.2` | 24 | — | `M+060 M-060 M+000 LFE1 M+135 M-135 M+030 M-030 M+180 LFE2 M+090 M-090 U+045 U-045 U+000 T+000 U+135 U-135 U+090 U-090 U+180 B+000 B+045 B-045` |

所有带 mask 的显式预设和自动映射都严格遵守 WAVEFORMATEXTENSIBLE 的升序 mask 位序。
因此 `7.1.4` 的后方 L/R（`0x10/0x20`）位于侧方 L/R（`0x200/0x400`）之前。
如果旧文件采用 Atmos 内部顺序（侧方 L/R 在后方 L/R 之前），不得将其声明为标准
`--input-layout 7.1.4`；应使用 `--input-channels` 明确写出实际顺序。
`mradm input-layouts` 会打印最终文件顺序，JSON 的 `channels` 与
`wave_mask_channel_order` 对带 mask 的布局保持一致。

可执行以下命令查看逐声道方位、仰角和 JSON 机器可读表：

```bash
mradm input-layouts
mradm input-layouts --layout 7.1.4
mradm input-layouts --format json
```

## 坐标、标签与别名

方位角以正前方为 `0°`，向左为正、向右为负；仰角以水平面为 `0°`，向上为正。
所有非 LFE 声道的归一化距离为 `1.0`。LFE 是低频语义声道，不赋予几何位置。

| 输入别名 | 规范标签 | 具体语义 |
|---|---|---|
| `L` / `FL` | `M+030` | 方位 `+30°`（左）、仰角 `0°` |
| `R` / `FR` | `M-030` | 方位 `-30°`（右）、仰角 `0°` |
| `C` / `FC` | `M+000` | 方位 `0°`（正前）、仰角 `0°` |
| `LFE` | `LFE1` | LFE，无几何位置 |

`M`、`U`、`T`、`B` 标签及其具体几何来自 `mradm input-layouts` 输出的统一目录。
标签不区分大小写，首尾空白会被移除。`U+110` 和 `U-110` 在内置布局中分别存在
`30°` 与 `45°` 两种仰角，因此自定义映射必须写成
`U+110@30`、`U-110@30`、`U+110@45` 或 `U-110@45`。

自定义映射规则：

- 必须提供 1–64 个标签，数量与文件声道数完全一致；每一项对应同序号的文件声道。
- 空标签、未知标签和重复的规范扬声器标签均报错。
- 只接受目录中的受控标签，不接受任意坐标或未登记的名称。
- `--input-channels L,R,C,LFE,M+090,M-090` 是合法的六声道自定义示例；这里的
  `L` / `R` 只描述输入声道几何，不改变输出语义。

## 输出语义、后端与 HRTF

公开的两声道输出只有 `binaural`，也是默认输出语义。渲染后端与 HRTF 来源独立选择：

| 输出 | 后端选择 | HRTF 来源 |
|---|---|---|
| 双耳 | `auto` / `saf-binaural` | SAF 内置 KEMAR；构建支持时可用 `--sofa <file>` 选择用户 SOFA |
| 双耳（macOS） | `apple` | Apple 系统 HRTF；不接受 `--sofa` |
| 多声道 | `ear` / `saf` / `apple`（macOS） | 不使用 HRTF |
| HOA3 | `hoa` | 不使用 HRTF |

`mradm backends` 的 `HRTF sources` 字段是当前平台与构建的能力真值。向不支持
`user-sofa` 的后端传 `--sofa` 会直接报错，不会换后端或忽略文件。

### DirectSpeakers 标签 / 位置 / 矩阵路由

`--direct-speakers-routing auto|label|position|matrix` 控制 DirectSpeakers：

- `auto`（默认）：SAF/Apple 扬声器使用 `label`；Apple binaural 使用 `position`。
- `label`：先精确匹配输出 speakerLabel，再使用共享别名（如 `L` → `M+030`）；命中后 one-hot
  写入该输出槽位。未命中时做零扩散空间化并 warning：已知 BS.2051 / 别名标签使用标签方向，
  否则使用 ADM 标称坐标。
- `position`：除 LFE 识别外忽略标签，以标称坐标、零扩散、无插值空间化。SAF 使用所选
  `--speaker-geometry standard|apple`，Apple 使用原有 AmbienceBed 路径。
- `matrix`：EAR、SAF 与 Apple 扬声器输出使用 `--direct-speakers-matrix <path>` 指定的严格 JSON，
  将每个非 LFE 输入标签直达一个或多个目标标签；自动选择到 EAR 的扬声器输出也支持。

需要坐标回退但缺少坐标时统一使用前中 `(0°,0°)` 并 warning。LFE 识别始终优先，因此 22.2 ch3/ch9 与
`split-power` 策略不受该选项影响。Apple binaural 显式 `label` 返回 unsupported，并拒绝 `matrix`；
EAR 只额外接受 `matrix`，原有显式 `label` / `position` 支持边界不变；SAF binaural 与 HOA 保持
`auto` 原生行为并拒绝所有显式模式。

矩阵 schema 固定为：

```json
{
  "schema": "mradm.direct-speakers-matrix.v1",
  "output_layout": "7.1.4",
  "routes": [
    {
      "source_label": "M+000",
      "targets": [
        { "label": "M+030", "weight": 1 },
        { "label": "M-030", "weight": 1 }
      ]
    },
    {
      "source_label": "U+000",
      "mute": true
    }
  ]
}
```

规则如下：

- 顶层和嵌套对象都拒绝未知字段；`schema`、`output_layout` 与非空 `routes` 必填。
- 每条 route 必须且只能使用非空 `targets` 或 `mute:true`。每个 `weight` 必须为有限正数，实际线性
  系数固定为 `sqrt(weight / sum(weights))`；不同输入汇入同一目标时直接相加，不做跨输入归一化。
- source/target 使用与 DirectSpeakers 标签路由相同的 BS.2051、RoomCentric 与 DAW 别名规范化。
  规范化后的重复 source、同一行重复 target、未知目标和歧义匹配均报错。
- `output_layout` 可使用 `5.1` / `7.1.4` 等现有别名，但规范化后必须与本次有效输出布局完全相同。
- 有效场景中的每个非 LFE DirectSpeakers block 必须恰好命中一行；缺标签或未覆盖直接失败。
  未被素材使用的 route 只记录 warning。矩阵中禁止 LFE source/target；LFE 继续走专用路径。
- `matrix` 模式缺少 profile、或其它模式携带 profile，均返回参数错误。C++ / C ABI 同时设置 path 与
  内存 JSON 时，内存 JSON 优先并记录 warning；CLI 当前只接受 path。

该配置在离线渲染与实时监听中共用同一 prepared recipe。`ds.gain`、对象增益与实时覆盖各乘一次；
显式 mute 保持精确零。第一阶段不提供 GUI 控件，也不扩展双耳或 HOA。

### 22.2 双 LFE 路由

`--lfe-routing` 只对内置 `22.2`（`9+10+3`）输出生效：

- `direct`（默认）：语义 LFE1 以 unity 写入 ch3，LFE2 / LFER 以 unity 写入 ch9，两路严格独立。
- `split-power`：素材必须只有一路语义 LFE；无论它是 LFE1 还是 LFE2，都以
  `sqrt(0.5)`（−3.0103 dB）分别写入 ch3/ch9。

`LFE`、`LFE1`、`LFEL`、`RC_LFE` 类别以及只有 `channelFrequency.lowPass` 的块归为第一路，
`LFE2` / `LFER` 归为第二路。重复的同一路元数据仍算单一语义 LFE；同时出现两类时，
`split-power` 在后端 prepare 阶段返回参数错误且不创建输出文件。其它布局保持原路由并记录 warning。
该功能不做分频、低频管理、去相关或房间校准。

WAV 输出不会只靠文字注释声明布局：

- `5.1`、`5.1.2`、`7.1`、`5.1.4`、`7.1.4` 写
  WAVEFORMATEXTENSIBLE channel mask；样本按 mask 位序写出。渲染器内部采用不同顺序时，
  只在最终文件边界重排一次。
- `9.1.4`、`9.1.6`、`22.2` 写 ADM DirectSpeakers AXML/CHNA，逐声道记录标签与几何；
  这些布局不会伪造一个无法精确表达它们的 WAVE mask。
- `binaural` 写 ADM Binaural `leftEar` / `rightEar` AXML/CHNA，不写扬声器 mask；
  `hoa3` 写 ADM HOA ACN/SN3D AXML/CHNA，并保留 `ambi` chunk。
- `i24` / `i16` 的 ADM 空间 WAV 使用 PCM BW64。默认 `f32` 使用 RF64 并携带相同的
  AXML/CHNA；若交付链要求规范 PCM BW64，请选择 `--output-bit-depth i24`。

可用 `mradm layouts --format wav` 查询每种输出布局的最终容器语义和文件声道顺序。

规范依据：[Microsoft WAVEFORMATEXTENSIBLE](https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible)
规定 mask 中出现的声道按最低有效位到最高有效位排列；ADM 元素语义采用
[ITU-R BS.2076-3](https://www.itu.int/rec/R-REC-BS.2076/)；PCM BW64、AXML 与 CHNA 封装采用
[ITU-R BS.2088-2](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2088-2-202511-I%21%21PDF-E.pdf)。

```bash
# 自动识别 5.1 WAVE mask，使用默认双耳输出
mradm render -i bed.wav -o bed_binaural.wav

# 显式预设输入，输出 7.1.4
mradm render -i bed.wav --input-layout 5.1 \
  --renderer ear --output-layout 7.1.4 -o bed_714.wav

# 自定义输入顺序，使用 SAF 与指定 SOFA 输出双耳
mradm render -i custom.wav --input-channels L,R,C,LFE,M+090,M-090 \
  --renderer saf-binaural --output-layout binaural --sofa listener.sofa \
  -o custom_binaural.wav

# Apple 系统 HRTF；不传 --sofa
mradm render -i bed.wav --input-layout 5.1 \
  --renderer apple --output-layout binaural -o apple_binaural.wav

# 单 LFE 等功率送入 22.2 的两路 LFE
mradm render -i mono_lfe.wav --input-channels LFE1 \
  --renderer ear --output-layout 22.2 --lfe-routing split-power -o lfe_222.wav

# 用自定义标签矩阵把 C 等功率送往 L/R（routes.json 使用上面的 schema）
mradm render -i bed.wav --input-layout 5.1 \
  --renderer ear --output-layout 7.1.4 \
  --direct-speakers-routing matrix --direct-speakers-matrix routes.json \
  -o bed_matrix_714.wav
```

## 合成场景

每个普通多声道文件稳定合成为一个名为 `Channel Bed` 的 programme、content 和 object；
每个文件声道对应一条 DirectSpeakers track，并覆盖文件的完整帧区间。非 LFE track 使用表中
精确方位与仰角；LFE track 标记低频语义且不设置空间位置。`inspect` 和 scene JSON 会报告
`source_kind=channel_bed`、解析后的输入布局及文件声道顺序。

C++ 调用方使用 `RenderOptions::input_layout` 或 `input_channel_labels`，并可通过
`RenderOptions::lfe_routing_mode` 选择 22.2 LFE 路由；矩阵由
`direct_speakers_routing_mode`、`direct_speakers_matrix_path` / `direct_speakers_matrix_json` 设置。C ABI v1.28 提供
`adm_render_options_set_input_layout`、`adm_render_options_set_input_channel_labels` 与
`adm_input_layouts_json`；v1.30 增加 `adm_render_options_set_lfe_routing_mode`；v1.34 增加 matrix=3 与
path / 内存 JSON setter。输入目录 JSON schema 为 `mradm.input-layouts` v1，路由矩阵 schema 为
`mradm.direct-speakers-matrix.v1`。
