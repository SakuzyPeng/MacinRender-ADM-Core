# 48 kHz 等尺寸对象：自有内核与离线接入

本实现按 Renderer 5.5 已命中的处理路径重建尺寸空间增益、四路滤波和时间状态。
生产代码使用本项目内核（数值状态现已[迁入 Rust](RUST_TRIPLE_BALANCE_MIGRATION.md)）；运行时不读取研究记录，不附加调试器，也不加载 Dolby 进程或私有库。
调用链来源见 [DAR_GAIN_CALL_CHAIN.md](DAR_GAIN_CALL_CHAIN.md)。

## 使用与支持范围

```sh
./build/release/mradm render -i input.wav -o output.wav \
  --renderer triple-balance --output-layout 9.1.6 \
  --no-peak-limit --output-bit-depth f32
```

`--no-peak-limit` 用于保持数值比较的单位全局电平；原响度和峰值处理流程仍可按需使用。

- 非零尺寸限 48 kHz、7.1.4／9.1.6、Cartesian、`width=height=depth` 且 size∈[0,1]。
- 保留兼容模式约束：固定 standard geometry；48 kHz 标准单个 7.1.2 bed 已接入固定路由与源语义，
  范围及混合验收见 [bed 接入](DAR_BED_SEMANTICS.md)。
  Objects 的增益、静音、起止语义与用户覆盖见 [对象语义对齐](DAR_OBJECT_SEMANTICS.md)。
  positionOffset、divergence、channelLock、screenRef、headLocked、HOA 及未验证组合返回 unsupported。
- 兼容语义层解释对象与块的时间；DSP 从文件第 0 帧推进。每个 512-frame 控制块允许多条
  时间非递减的元数据事件，以块内最后一条作为控制目标；同采样点按文件顺序选择，首块规则相同。
  密集事件是后续新增的输入支持，不将原有参考验收扩大为参考渲染器的密集事件行为结论。
- `spread=auto` 对含非零尺寸的轨道使用有状态内核；`spread=none` 明确忽略尺寸，继续原点源路径。
  全片 size=0 不进入尺寸处理器；显式 MDAP 与 Triple Balance 互斥。
- 当前通过独立 `triple-balance` 后端提供离线渲染与实时监听；实时编辑支持等尺寸倍率，独立三轴尺寸仍不开放。详见 [Triple Balance](TRIPLE_BALANCE_RENDERER.md)。
  22.2 已作为独立的[自有几何扩展](ROOM_222_EXTENSION.md)接入，不属于本页的 Dolby 数值兼容范围。

## 空间规则

`adm_render_triple_balance/size_panner` 将内部房间坐标与 Q15 参数分开，避免重复坐标转换。
内部左／前／地面为 `(0,0,0)`，对应 ADM 的 `((X+1)/2, (1-Y)/2, Z)`。
位置和尺寸转换为 32768 尺度的整数，正上界为 32767。

原始尺寸输出固定为 11 个非 LFE 位置：L、R、C、Lss、Rss、Lrs、Rrs、Ltf、Rtf、Ltr、Rtr。
算法按房间几何计算轴向等功率基函数、尺寸核的积分与房间边界贡献，然后组合并归一化。
尺寸到内部半径的分段节点为 `(0,0)、(0.2,0.075)、(0.5,0.25)、(0.75,0.45)、(1,0.7)`；
核的幂次、log2/exp2 近似和插值次序均来自已确认的函数。

实现按需计算积分角点，不保存从对象测试点拟合的增益表，也没有逐声道误差补偿。
本批 462 次真实回调的初始差分最大绝对误差约 `6.4e-7`；该数据用于分层回归，不作最终独立集。

混合后处理与原始几何分开：原对象和尺寸分支分别使用观测到的 cos²／sin² 系数，cutoff 为 0.2，
再应用尺寸功率修正、声道滤波路选择与左右符号。尺寸分支没有被强制恢复成单位总功率。
9.1.6 的宽声道和顶中声道由原对象分支保留，因此小尺寸不等于折叠成 7.1.4。

## 滤波、时间与生命周期

`SizeDecorrelator` 复现有状态结构，而旧的测量 FIR `SizeFilterBank` 保留作研究对照。
新结构包括 96-sample 输入延迟、前后电平包络控制，以及四级全通延迟 152／200／263／346；
四路系数的绝对值为 0.4，符号和输出位置映射按确认的参考配置处理。
静音检测、32-frame 子块的进入/退出包络和尾声截断独立保存状态。

`SizeObjectProcessor` 每对象独立推进文件绝对时间。元数据以 512-frame 控制块更新，位置与尺寸的
平滑时间常数分别为 1200、960 个采样；增益在块内插值。尺寸目标为零时，按参考阈值收敛到零。
原对象系数为零时，参考清除其高度使能标志，OAR 的静音点源状态也必须继续更新。

尺寸归零控制块仍混合上一尺寸的插值尾声，**在该块混音后**重置滤波器。
这条生命周期由“24576 帧归零、28672 帧恢复”的额外压力案例验证：旧候选过渡能量 NRMSE 约 6.8%，
修正后约 0.015%。原失败、修正后结果及小型 PCM fixture 均保留。

内部 push/finish 缓冲完整控制块，因此调用者使用 1、31、32、257、511、512、513、1024 帧输入块
得到相同结果；文件尾部只输出原有长度。prepared 对象仅保存元数据，DSP 状态每次 render 独立创建。
有状态裁剪从文件起点预热，窗口外样本不写出、不进入窗口响度计量。

## 验证与复现

- 空间原始增益、四路滤波输出有来自真实捕获的小型 fixture。
- Debug 单测覆盖分块一致性、reset、对象间状态独立、文件尾部、LFE、unsupported 路径和快速尺寸恢复。
- 离线适配层测试复用同一 prepared 数据，比较完整渲染、裁剪和再次完整渲染。
- 所有音频比较使用 Release；映射、时延及全局电平固定，不逐案例拟合对齐。
- 静态评分包含能量占比、总功率、三分之一倍频程、互谱矩阵及尾部；尾部分类和曲线统一使用
  显式静音段与 PCM24 分辨率确定的噪声底。额外尾声另行检查。
- 曾发现旧固定 `1e-9` 尾部分类把 size=0.0022 的可测尾声当成无尾声；修正的是噪声底分类，
  不是放宽 1 dB 门槛。原失败评分保留，新增 Python 回归防止复发。

静态边界与最终集一键入口：

```sh
python3 scripts/research/dar_layouts/run_native_size_acceptance.py \
  --phase boundary --output-dir local/my-size-boundary \
  --channel-map local/dar-compat-suite-20260925/channel-map.json

python3 scripts/research/dar_layouts/run_native_size_acceptance.py \
  --phase final --freeze local/my-size-boundary/acceptance.json \
  --seed 0x26092652 --output-dir local/my-size-final \
  --channel-map local/dar-compat-suite-20260925/channel-map.json
```

脚本先构建 Release；按四案例一批生成新 ADM，两端使用同一最终 BWF，核对 PCM 与元数据。
每例包含四秒独立 PRBS 和显式静音尾段。保留评分、频谱档案、哈希、生成描述和回归音频，清理大 PCM。
导出被中断时可加 `--resume`；版本、源文件或最终 ADM 不匹配时拒绝复用。
独立集一旦用于调整模型，就降级为回归，并在新冻结点生成新种子的最终集。

已有动态或长 PRBS 套件可通过 `run_compat_suite.py --candidate-size` 直接检查生产 CLI；
支持 `size-prbs-long`、`size-motion`、`size-motion-boundary` 和 `size-route-scan`。
之前的空间拟合失败、较早候选和首次最终集仍是历史证据，不代表当前内核验收。

当前证据根目录为 `local/dar-native-size/`：`reset-boundary-acceptance/` 为修正后的边界集，
`verified-final-acceptance/` 为新冻结点的最终集，`rapid-reset/` 为快速恢复案例。

最终 32 个新随机案例在两布局全部通过。跨两布局的最大值如下，均保持零时延、单位全局电平：

| 指标 | 实测最大值 | 门槛 |
|---|---:|---:|
| 归一化能量占比相对 L2 | 0.0103% | 5% |
| 总功率差 | 0.000088 dB | 0.1 dB |
| 有效声道频带误差 | 0.0165 dB | 0.5 dB |
| 归一化互谱矩阵误差 | 0.0091% | 5% |
| 可测尾部曲线误差 | 0.0193 dB | 1 dB |

生产 CLI 的长 PRBS、普通动态和边界动态套件通过；快速归零恢复的能量包络 NRMSE 为
7.1.4 约 0.0048%、9.1.6 约 0.0152%。跨该过渡裁剪与完整渲染截取逐样本一致。
45 项 Debug CTest 和 9 项研究脚本回归通过。汇总为 `local/dar-native-size/delivery-report.json`。
