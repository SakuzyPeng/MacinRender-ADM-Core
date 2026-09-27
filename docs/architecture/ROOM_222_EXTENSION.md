# 22.2 房间渲染扩展

已接入离线 Release CLI，覆盖点源、标准 7.1.2 bed、等尺寸、尺寸变化及带尺寸运动。
**这是自有 22.2 几何模型，不是 Dolby 22.2 数值复刻。** Renderer 5.5 的参考仍只用于已验证的
7.1.4／9.1.6 及其源元数据、滤波和时间规则；本扩展没有调用 Dolby、IDA 或 LLDB。

```sh
./build/release/mradm render -i input.wav -o output-222.caf \
  --renderer saf --speaker-panner room-compat --output-layout 22.2 \
  --no-peak-limit --write-semantic-report semantics.json
```

沿用已有选项，`renderer_effective.profile` 明确区分为 `room-222-extension-v1`，`spatial_reference` 为 null。
该报告记录实际使用的 22 个节点、房间坐标、滤波路及符号、尺寸规则、源字段解释和最终用户增益。
每个事件还记录进入平滑前的目标空间增益及干／尺寸混合系数，静态案例可直接据此核对实际分支。
默认 SAF、GUI、实时接口和 C ABI 未扩展；旧兼容布局保留独立的参考计算路径。

## 输入与输出范围

- 48 kHz ADM；独立绑定 PCM 的 Cartesian Objects，XYZ∈[-1,1]。
- 等尺寸 `width=height=depth=size∈[0,1]`；三轴独立尺寸继续 unsupported。
- 单个完整、前 10 路按 RC 标签顺序绑定的 7.1.2 bed，可与对象混合。
- 源增益、mute、起止及块时间沿用兼容语义准备层；用户对象和 bed 通道增益／静音独立生效。
- `spread=none` 明确关闭尺寸，走本布局的点源路径；MDAP、额外 geometry、positionOffset、未验证修饰字段、
  嵌套对象、多 bed 等限制保留。旧 7.1.4／9.1.6 的 XYZ 范围及已验证字段组合保持原样。

优先交付 CAF：写入 CoreAudio `CICP_13`，已实测 `afinfo` 显示完整 22.2 标签。
WAV 沿用项目的 ADM AXML／CHNA 布局封装，不使用 7.1.4／9.1.6 的 DAR re-render profile；
不能据此声称通用播放器能自动识别 22.2，macOS A/B 使用 CAF。

## 三层几何

输入 X 向右、Y 向前；Z=0 为中层，Z=1 为上层，Z=-1 为下层。令 `a=80/155`、`w=105/155`。
以下是房间渲染坐标，输出声道仍按现有 BS.2051／CICP_13 定义。

| 层 / Z | 行 / Y | 按 X 从左到右排列的节点 |
|---|---|---|
| 下层 / -1 | a | `(-a,B+045) (0,B+000) (a,B-045)` |
| 中层 / 0 | -1 | `(-1,M+135) (0,M+180) (1,M-135)` |
| 中层 / 0 | 0 | `(-1,M+090) (1,M-090)` |
| 中层 / 0 | w | `(-1,M+060) (1,M-060)` |
| 中层 / 0 | 1 | `(-1,M+030) (0,M+000) (1,M-030)` |
| 上层 / 1 | -a | `(-a,U+135) (0,U+180) (a,U-135)` |
| 上层 / 1 | 0 | `(-a,U+090) (0,T+000) (a,U-090)` |
| 上层 / 1 | a | `(-a,U+045) (0,U+000) (a,U-045)` |

先在行内、再在行间、最后在层间做等功率插值。相邻节点之间的幅度权重为
`cos(pi*t/2)`、`sin(pi*t/2)`。节点外侧投影到最近端点；不先把 XYZ 换成单一方向。

下层只有前方三个点，因此最低层采用前方行投影：该层的 Y 不再改变分配；在 -1<Z<0 时仍与中层的
完整 XY 场连续混合。这是一项明确的建模选择，并不表示存在后方下层扬声器。
正上方 T+000 是真实节点，原点则沿用中层侧方行的左右等功率分配。

所有点源均在 22 个非 LFE 输出上保持单位功率，LFE1／LFE2 始终为零。两个 LFE 不参与空间插值。

## 尺寸空间规则

尺寸使用独立的布局积分模型。对于中心 `p=(x,y,z)` 和 size `s`，每轴区间为
`[max(-1,p-s), min(1,p+s)]`，即裁剪在房间内、半宽为 s 的等尺寸立方体。
令点源在第 i 个输出上的幅度为 `g_i(p)`，尺寸原始幅度定义为：

`G_i = sqrt(mean_cube(g_i(p)^2))`。

行／层权重可分离，代码对分段 sin²／cos² 做解析积分，不依赖抽样训练或逐点补偿表。
原始尺寸增益的平方和保持 1；size→0 连续回到点源。靠墙时按实际裁剪体积求平均，
因此 size=1 不承诺任意中心都得到相同分布。

原对象／尺寸混合沿用已识别的 cos²／sin² 混合曲线、0.2 交叉范围及 1.2 dB 尺寸修正规则；
前方组在此明确对应 M+030、M-030、M+000。最终有状态输出不被强制归一为单位功率。

保留四路全通结构、干湿混合系数、32-frame 滤波块、512-frame 控制时钟、位置和尺寸平滑及归零收尾。
滤波路分配由节点描述驱动，中心轴节点保留原信号，左右配对使用指定滤波路的相反符号。
22.2 空间计算连续，不借用旧 panner 的位置量化；旧两布局的算法、量化及 11 路尺寸分支保持原样。

每个对象独立持有状态与空间缓存；静态元数据复用已计算的积分。渲染开始时重置，裁剪从文件起点预热，
只写出和计量请求窗口，输出长度不增加。

## bed 与 LFE

| 源 7.1.2 bed | 22.2 目的声道 |
|---|---|
| RC_L / RC_R / RC_C | M+030 / M-030 / M+000 |
| RC_Lss / RC_Rss | M+090 / M-090 |
| RC_Lrs / RC_Rrs | M+135 / M-135 |
| RC_Lts / RC_Rts | U+090 / U-090 |
| RC_LFE | 按下面的显式 LFE 策略 |

默认 `--lfe-routing direct` 将唯一的源 LFE 送到 LFE1，LFE2 静音。
`--lfe-routing split-power` 将其以每路 1/sqrt(2) 分给两个 LFE。不会从 Objects 合成低音。
本轮不开放双独立 LFE 输入或多 bed；CICP_13 将这两个已有槽位显示为 LFE2／LFE3，这是名称约定。

## 验证与证据

`tests/unit/room_222_test.cpp` 覆盖全部 22 个节点、网格与边界、左右对称、原点／顶点邻域、负 Z、
同方向不同半径、size=0 邻域、size≈0.2、size=1、尺寸归零、静音尾部和非法输入。
原始空间增益的最大功率偏差约 `8.56e-8`。

固定 seed `0x2220927` 的 32 个新位置，用独立的 24³ 三维中点求积核对尺寸解析积分；
最大绝对功率差约 `3.67e-4`，是与有限分辨率数值求积的差值，不是 Dolby 参考误差。
1、31、32、257、511、512、513、1024 帧分块与 reset 得到逐样本一致结果；不同对象交错处理无共享状态。

Release CLI 套件：

```sh
cmake --build --preset release --target mradm mr_adm_room_222_tests
python3 scripts/research/dar_layouts/verify_room_222.py \
  --template local/dar-bed-semantics/template.wav --output-dir local/room-222/cli
```

22 个逐节点脉冲、7.1.2 bed 映射、两种 LFE 路由、负 Z 尺寸运动、重复渲染、跨事件裁剪、
`spread=none` 与点源等价、LFE 泄漏及静音收尾均通过。探针只用于自有内核验证，未保留不适配的 DBMD。
套件核对 CAF 的 CICP_13 标签及 `afinfo` 的 22.2 识别。

尺寸频谱与相关性采用分层验证：先把既有 Renderer 5.5 捕获的四路滤波 PCM 按声明的 22.2 路由及增益
组合，与实际 CLI 输出核对，最大采样差约 `3.77e-9`。再用不少于 4 秒的独立 PRBS 检查对称滤波对的
公共信号恢复恒等式，使用准备报告中的实际增益，不拟合电平或移动延时。63 Hz–16 kHz 三分之一倍频程
的最大误差约 `2.93e-8 dB`，归一化互谱矩阵相对误差约 `3.20e-9`；显式静音开始后 1024 帧之外全零。
这些验证滤波复用、声道分配与模型内部的一致性，不是拿 Dolby 22.2 扬声器输出作比较。

真实音乐 `/Users/Sakuzy/Downloads/ADMCUT - RADWIMPS - 前前前世_SSDX.wav/ADMCUT - RADWIMPS - 前前前世_SSDX.wav`
已完成全片 76.660146 秒、3,679,687 帧、24 声道的 Release 渲染。22 个非 LFE 输出都有信号，三个下层声道
RMS 约 -48.18／-40.78／-40.67 dBFS；LFE1 逐样本等于源 LFE，默认 direct 下 LFE2 为零。
这验证完整输出和路由，不构成 Dolby 22.2 数值验收或听感结论。

同一真实音乐重渲染为 9.1.6，与先前保留的 CAF 解码 PCM 哈希完全相同；旧点源、size、语义和 bed
参考夹具均通过。Debug 全部 49 项 CTest 通过，Release 的 5 项相关数学／状态回归通过。
记录位于 `local/room-222/`：`cli/report.json`、`real-result.json`、`legacy-real-916.json` 及测试日志。
真实成品为 `zenzenzense-room-222-direct.caf`。只保留一份完整 22.2 音频及小型回归片段，未新增 IDA 数据库。
