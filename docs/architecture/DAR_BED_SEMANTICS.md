# Renderer 5.5：非静音 7.1.2 bed 的路由与语义证据

本轮已测出标准 7.1.2 bed 到 7.1.4／9.1.6 的固定路由，并用实际 OMO／OAR 数据流和独立宽带 PCM 验证。
默认 SAF 的主要差异来自 DirectSpeakers 标签匹配失败后的方向 VBAP 分配。研究阶段先用显式矩阵验证，
随后已将专用 bed 路由、源语义与用户覆盖正式接入离线 `room-compat`，无需外部矩阵或 Dolby 进程。
兼容模式整体仍为实验功能；本页下半部分保留研究阶段证据，并记录接入后的验收范围。

22.2 的 bed／点源／等尺寸输出另见[自有房间扩展](ROOM_222_EXTENSION.md)，不属于本页 7.1.4／9.1.6 的 Dolby 数值参考范围。

## 当前 CLI 支持

```sh
./build/release/mradm render -i input.wav -o output.wav \
  --renderer saf --speaker-panner room-compat --output-layout 9.1.6 \
  --no-peak-limit --output-bit-depth f32 --write-semantic-report semantics.json
```

输出也可选择 `7.1.4`。当前接入范围是 48 kHz、一个完整 7.1.2 bed，前 10 路 PCM 依次绑定下述十个
RoomCentric 标签。每个通道的标签及 pack 保持不变；可与已验证的独立 Cartesian 点源、等尺寸及运动对象
同时渲染。源 gain／mute／起止字段按本页参考规则解释，用户 object gain／mute 和逐 bed 通道 gain／mute
作为额外输出控制独立生效，源零增益不会吞掉用户覆盖。

`room-compat` 的 WAV 封装也跟随参考 re-render：使用 WAVEFORMATEXTENSIBLE，7.1.4 的掩码为 0x2D63F，
9.1.6 的掩码为 0，且不附加 AXML／CHNA。小文件为 RIFF，容量需要时才使用 RF64；采样位深仍由用户选择。
这是独立的兼容写出分支，普通 SAF 的 ADM 布局元数据保持原样。9.1.6 WAV 与 Dolby 一样由 `afinfo` 显示
`no channel layout`；播放器需要人工指定布局。macOS 自动识别布局的比较文件请输出 `.caf`，其
`Atmos_9_1_6` 标签可被 CoreAudio 正确识别。不要把 CHNA 引起的未知 `16.0` 显示误认为 9.1.6 已被识别。

`renderer_effective.beds` 报告来自实际准备数据，包含原始块事件、忽略字段、固定布局内部排列的增益及
用户最终电平；`original`／`effective` 继续描述原始 ADM 和策略应用后的场景。DirectSpeakers 导入层补充
gain 单位、相对时间与原始坐标表示。异常组合仍写出语义诊断。

多 bed、不完整 bed、重新排列／别名、非 48 kHz、未验证位置／频率修饰、用户位置覆盖，以及额外 position／
matrix 路由均返回 `unsupported`。原先任意静音 bed 的绕行入口改为显式拓扑验证；对象无 bed 的已验证
路径保持不变。默认 SAF、GUI、实时接口及 C ABI 没有扩展。

## 路由识别阶段的范围与身份

- Renderer 5.5.0 release，arm64，直接读取最终 ADM BWF 后 re-render。
- UUID：`F11646CF-9535-3815-8184-524335D4CC3A`。
- 安装文件 SHA-256：`ec9432d6af6679cfdf5cc668d37be0b3ac295ea9453b264ec84e0d1706bb2390`。
- 48 kHz，一个 7.1.2 DirectSpeakers bed，10 个 PCM 绑定；另保留一个静音 Objects PCM，共 11 声道。
- bed 标签为 `RC_L/R/C/LFE/Lss/Rss/Lrs/Rrs/Lts/Rts`，其中 Lts／Rts 是源顶部声道。
- 使用已冻结的带名称 multi-mono／interleaved 声道映射，固定零时延、单位全局电平。
- 不涉及监听校准、bass management、其他 bed 格式、多 bed 或 bed 与非静音对象同时混合。

生成器由已验证的 Conversion Tool 母版拓扑构造新的最终 BWF。仅修改探针 PCM 和指定 XML 字段，保持
CHNA、声道数、帧数及不透明 DBMD 一致，不猜测其内部偏移。每例记录 BWF、PCM、AXML、CHNA、DBMD 哈希，
以及完整 track UID／PCM 绑定、源字段和时间。

## 基础路由

| 输入 bed | 7.1.4 输出及幅度增益 | 9.1.6 输出及幅度增益 |
|---|---|---|
| L、R、C | 各自同名声道，1 | 各自同名声道，1 |
| Lss、Rss | 各自侧环绕，1 | 各自侧环绕，1 |
| Lrs、Rrs | 各自后环绕，1 | 各自后环绕，1 |
| LFE | LFE，1 | LFE，1 |
| Lts | Ltf、Ltr，各 `0.7071067690849304` | Ltm，1 |
| Rts | Rtf、Rtr，各 `0.7071067690849304` | Rtm，1 |

9.1.6 的宽声道、前顶和后顶没有来自该 bed 的信号。7.1.4 的顶部是同一信号的等功率分配，两路高度相关，
没有尺寸处理中的去相关信号。各输入总功率为 1，误差限于浮点及输出量化。

40 个隔离脉冲覆盖每路的正、负、半幅及不同 512 帧相位；响应均在原采样点，无可测延迟或尾部。随后
十路独立确定性宽带信号同时驱动约 3.91 秒，并保留文件内静音段。由脉冲得到的零时延矩阵预测全文件
输出：9.1.6 逐样本一致，7.1.4 最大绝对残差 `1.4901161193847656e-8`。

LFE 的脉冲及宽带 PCM 均为单位增益直达，没有在此 re-render 配置中观察到低通或额外 10 dB；不外推到
监听链或其他导出配置。

## 源字段隔离

每个变体与宽带基线使用相同的最终 PCM／CHNA／DBMD，两布局全文件逐样本比较：

| 变体 | 结果 |
|---|---|
| bed 所属 audioObject gain：0、0.5 | 与基线逐样本一致 |
| 各 DirectSpeakers block gain：0、−6.020599913279624 dB | 与基线逐样本一致 |
| audioObject mute：1 | 与基线逐样本一致 |
| audioObject start=48000、duration=96000 帧 | 与基线逐样本一致，边界外 PCM 未被门控 |
| block rtime=48000、duration=96000 帧 | 与基线逐样本一致 |
| 96031 帧处新增 block，gain 从缺省变为 0.5 | 与基线逐样本一致 |
| 非 LFE 通道坐标全部设为原点 | 与基线逐样本一致 |
| L／R 和 Lts／Rts 的坐标互换，标签不变 | 与基线逐样本一致 |
| L／R 和 Lts／Rts 的标签互换，坐标不变 | 拒绝导入：`The master file contains an unsupported bed format.` |

这些是指定有效 bed 拓扑中的成对 PCM 证据，不是任意 DirectSpeakers 元数据的通用规则。标签交换案例
保持 11 路绑定和原 DBMD，错误不是此前的 DBMD 声道计数不匹配。它表明标签／bed 格式约束参与导入验证，
尚不足以定义任意重新排列、别名或自定义 bed 的兼容规则。

本轮没有为每个忽略字段新增 SAX 追踪；不能凭 PCM 不变断言字段具体在哪一层被丢弃。

## 内部调用与 PCM 验证

复用版本锁定的驻留 Qt 桥和已有捕获包装器。每次一次 LLDB 附加安装后立即分离，通过正常离线导出采集，
不独立调用内部 panner，不使用 GUI 操作。

实际命中 `OMO worker 0x101e54840 → OMO merge 0x101e54774 → Sushi Home OAR 0x101e3ebcc`。
相关静态子函数见 [DAR_GAIN_CALL_CHAIN.md](DAR_GAIN_CALL_CHAIN.md)。72-byte 输入事件需要按种类解释：

| 偏移 | 本批非静音 bed | 对照 Objects |
|---|---|---|
| `+32` u32 | 1 | 0 |
| `+36` | u32 声道代码：L/R/C/LFE/Lss/Rss/Lrs/Rrs/Lts/Rts = 1/2/3/4/5/6/7/8/11/12 | float X，后续为 Y/Z |
| `+52` float | 0 | size |

不能把 bed 的 `+36` 当成浮点 X。报告保留原始字节及 u32 值，未知字段不猜名称。

两布局各核对 23 个含真实 PCM 的控制块：

- OMO 原始分支逐样本等于 bed 输入，44 条尺寸分支全零，尺寸增益回调零命中。
- OAR 每路输入指针及 PCM 对应 OMO 输出，实际 target gain 等于上述路由表。
- 按 target gain 重建各 OAR 实例 PCM，误差为 0。
- OAR 输出求和与最终 WAV 的最大差：9.1.6 为 0；7.1.4 为 `1.210719347000122e-8`。
- 有追踪与无追踪的完整导出 PCM 在两布局均逐样本一致。

OAR adapter 的输入视图会把 LFE 移到前面。旧对象分析中把上游 selection range 当作调用后 PCM 排列的
假设不适用于此；bed 分析器改用实际缓冲区地址，并核对全部采样来关联。例如容器 `[0,7)` 的调用后
真实输入顺序为 `[3,0,1,2,4,5,6]`。原失败分析保留在 `traces/prbs-916/bed-validation-before-pointer-link.json`，
没有因此调整音频或声道映射。

`array Ls/Rs flattener` 仍只有既有静态线索，本轮没有新增其运行命中证据，不能用该名字解释声场变化。

## 当前 SAF 差异与矩阵诊断

SAF `label` 路由将 RoomCentric 标签映射到标准位置标签；输出没有对应位置时，使用方向 VBAP。标准位置
并不总等于 Dolby bed 的语义槽位，例如源侧环绕对应 M±090，项目 9.1.6 的侧环绕采用 M±110。
源 Lts／Rts 在两布局中也没有直接命中。

Release 实测：7.1.4 每个顶部 bed 向同侧环绕输出约 `0.337652`，向前顶／后顶各输出约 `0.665579`；
9.1.6 的侧环绕分到侧环绕与宽声道，后环绕分到侧／后环绕，顶部信号大量进入侧环绕和前顶。
其他已匹配声道及 LFE 的基础路由与参考一致。

研究脚本通过现有 `--direct-speakers-routing matrix` 表达标准槽位映射，只设置目的声道与等权分配，
不含拟合补偿。LFE 继续使用专门路由。同一最终 ADM、零时延、单位全局电平下：

| Release 矩阵诊断 | 7.1.4 最大 PCM 差 | 9.1.6 最大 PCM 差 |
|---|---:|---:|
| 逐路脉冲 | `4.470348358154297e-8` | 0，逐样本一致 |
| 独立宽带、多路同时输入 | `1.210719347000122e-8` | 0，逐样本一致 |

上表是接入前的基础路由诊断。显式矩阵仍沿用普通后端的 gain／时间处理，不能独自复现忽略源字段的规则；
当前离线 `room-compat` 已使用独立语义准备和固定路由，无需这个诊断矩阵。默认 SAF 行为保持不变。

## 复现与保存

```sh
python3 scripts/research/dar_layouts/trace_bed_semantics.py \
  --output-dir local/dar-bed-semantics \
  --channel-map local/dar-object-semantics/final/channel-map.json \
  --capture --compare-saf --compact
```

同目录可复跑：参考必须匹配 ADM、PCM 及恢复状态；压缩参考先验证哈希再恢复。新目录会重新生成、导出
和采集。`--only impulses,prbs` 只测基础路由，`--generate-only` 只生成探针。

脚本为 `scripts/research/dar_layouts/trace_bed_semantics.py`、`analyze_bed_trace.py`、`test_bed_semantics.py`。
证据在 `local/dar-bed-semantics/`：`summary.json`、各例 `case.json/result.json`、`traces/*/bed-validation.json`、
`candidate/report.json`、`state-audit.json`。诊断矩阵为 `candidate/bed-7.1.4.json`、`candidate/bed-9.1.6.json`。

13 个合成 ADM 中 12 个导入成功、1 个按预期拒绝，另有两次追踪导出。全部 15 次操作结束后，母版、布局、
导出器及 key 24／251 均恢复。比较音频全部由 Release 生成。原始 WAV／trace 无损压缩前后核对 SHA-256，
保留完整测量、拒绝日志和恢复记录；数据约 130 MiB，没有新增 IDA 数据库或工作区。

验证：18 个语义研究 Python 测试、6 个既有增益追踪测试、2 个 DirectSpeakers matrix Debug 回归通过。
压缩后完整复跑也通过，没有额外重复导出参考。

## 接入验收

新增私有 `room_compat_bed`；固定增益由准备层编译进入现有离线混音，准备配置不保存可变 DSP 状态。
bed 和对象各自施加用户输出增益，现有尺寸处理器从文件起点预热，裁剪只写出和计量请求窗口。

```sh
cmake --build --preset release --target mradm
python3 scripts/research/dar_layouts/verify_bed_integration.py \
  --reference-root local/dar-bed-semantics \
  --channel-map local/dar-object-semantics/final/channel-map.json \
  --output-dir local/dar-bed-integration
```

12 个有效源字段案例在两布局共 24 次 CLI 比较全部通过，标签交换案例正确返回 unsupported。
另生成 bed＋点源、bed＋静态 size、bed＋尺寸运动及对应 object-only 四个最终 ADM，共 8 次参考比较：
能量占比、功率、频带、互谱、尾部、动态包络和变化时刻均通过原门槛。含 bed 时单独核对 LFE 等于其
绑定的源 PCM；Objects-only 继续使用禁止 LFE 泄漏的原规则，没有删除 LFE 后评分。

新增混合案例的最大能量占比误差约 `0.00746%`、功率差约 `0.00000121 dB`、频带误差约 `0.000514 dB`、
互谱误差约 `0.0119%`、动态包络误差约 `0.00755%`。这是新增案例的成绩，不替代既有独立对象验收集。
重复渲染和跨元数据边界裁剪逐样本一致；bed 与对象的参考叠加误差不超过一个 24-bit LSB。
源增益为零时的用户增益、指定顶部通道增益、LFE 用户静音及混合文件中独立 bed 静音均通过。

接入记录为 `local/dar-bed-integration/report.json`，四次新增参考导出全部恢复母版、布局、导出设置及
key 24／251。新增原始音频和记录按需无损压缩，测试夹具只保存真实捕获的固定增益与来源哈希。
Debug 全部 47 项 CTest 通过；源语义研究 Python 测试 19 项通过。
多 bed、其他 bed 格式以及未验证修饰字段仍留待后续。
