# Renderer 5.5：对象增益与生命周期

本页限定 **Renderer 5.5.0 release 直接读取 ADM BWF 的离线 re-render**，48 kHz、标准 7.1.4／9.1.6、
独立绑定 PCM 的 Cartesian Objects 与已验证等尺寸。默认 SAF 的 ADM 处理保持原样。

## 已确认的解释规则

| 源 ADM 字段／情形 | 参考输出与兼容行为 |
|---|---|
| audioObject gain、block gain，包括线性 0 与相应 dB 值 | 不改变送入对象处理器的 PCM；采用单位原生增益 |
| audioObject mute | 不抑制对象 PCM，也不重置尺寸滤波器 |
| audioObject start／duration | 记录验证诊断，仍按文件 PCM 时间线处理；不添加 start 偏移、不按 duration 门控 |
| block rtime | 在文件的 512-frame 控制时钟上消费；不叠加 audioObject start |
| block duration、块间元数据空隙、末块缺省 duration | 不门控 PCM、不清空 DSP；保持最后位置与尺寸目标 |
| 首块 rtime 在 `[0,512)` | 该目标用于第一个控制块的初始化 |
| 首块 rtime ≥512 | 之前使用 ADM 原点 `(0,0,0)`、size=0；输入 PCM 从文件开头参与混音 |
| 文件中的真实 PCM 静音 | 继续现有尺寸处理器的静音、尾部及复位逻辑 |

“忽略”由同 PCM 的字段隔离、真实导入／处理调用命中和最终 PCM 联合验证。
部分不规范时间字段会产生 `[ADM validation]` 日志，但导出仍成功；不能仅用日志的 ERROR 级别判断导入被拒绝。
这些规则不代表 ADM 规范要求如此，也不直接适用于 Logic、AVPlayer 或其他 Renderer 版本。

显式用户控制独立生效：`gain.scale`／`gain_db` 在每个对象的原对象与尺寸分支合并后只应用一次；
显式 mute 优先，关闭该对象最终输出，同时继续推进状态。策略的匹配、继承与字段覆盖顺序沿用现有实现。
全局策略作用于常见静音 bed 母版时，不再把用户音量误判为未验证的原生 bed gain。
48 kHz 标准单个 7.1.2 bed 的非静音路由和源字段解释已接入，用户对象／通道电平独立生效；
拓扑限制及 bed＋Objects 混合验收见 [bed 接入](DAR_BED_SEMANTICS.md)。

## 实现与报告

导入器保存 gain 单位／是否省略、原生 mute、对象时间、相对 block 时间和对象引用关系。
现有绝对采样时间继续供普通后端使用；私有 `room_compat_semantics` 只转换兼容模式的准备数据。
嵌套对象、共享 PCM 绑定、未验证采样率和其他修饰字段继续返回 unsupported。

复用原有离线命令：

```sh
./build/release/mradm render -i input.wav -o output.wav \
  --renderer saf --speaker-panner room-compat --output-layout 9.1.6 \
  --no-peak-limit --output-bit-depth f32 --write-semantic-report semantics.json
```

语义报告保留 `original`／`effective` 的原含义，并补充块 gain、时间、原始坐标表示和 PCM 索引。
新增 `renderer_effective` 来自实际准备过程，记录忽略／采用／转换的字段、控制块时间、默认状态及用户输出增益。
其中的位置、尺寸是进入已有平滑／量化内核前的目标值；不能把它们当成每个采样的平滑后参数。
准备失败时也保留原始报告和拒绝原因；文件与内存报告内容一致。没有新增 CLI 开关或 C ABI 函数。

每次渲染创建独立 DSP 状态，prepared 数据只保存元数据。尺寸裁剪从文件开头预热；输出和响度只计请求窗口。
显式 `spread=none` 继续关闭尺寸，报告会标明尺寸被用户忽略。生产代码不读取 Dolby 进程或任何研究数据文件。

## 调用证据与时间精度

二进制身份沿用 [尺寸调用链](DAR_GAIN_CALL_CHAIN.md)：arm64 UUID
`F11646CF-9535-3815-8184-524335D4CC3A`，SHA-256
`ec9432d6af6679cfdf5cc668d37be0b3ac295ea9453b264ec84e0d1706bb2390`。

| 环节 | 静态 VA | 证据 |
|---|---|---|
| audioObject SAX start-element | `0x102379454`，虚表槽 `0x103d36a70` | 正常导入命中，捕获 ID、名称、start、duration；已分析的子元素分支不消费 gain／mute |
| Objects block SAX start-element | `0x102373eec`，虚表槽 `0x103d36380` | 正常导入命中，记录 block ID 与消费者，关联 rtime 和后续 OMO 事件 |
| 时间验证 | `0x10238615c` | object start／duration 的诊断位置，与实际导出成功及 PCM 对照相互核验 |
| OMO → 原对象／尺寸分支 → OAR | 沿用既有已验证链 | 输入 PCM 逐样本对应最终 BWF，混音重建对应最终 WAV |

捕获按实际加载地址重定位，核对模块身份和原虚表指针。包装器只转发正常调用并复制已确认内存。
导入记录与导出记录有分别的启用窗口；恢复母版时不继续记录导入。
有追踪与无追踪 PCM 相同，捕获数据重建输出的最大绝对误差约 `6.7e-8`。

新增边界还发现旧点源运动使用 double 状态会越过量化边界。向 `Z=0.5` 靠近时，参考 float 状态停在
`0.4999999701976776`；旧实现最终舍入为 0.5，产生约 3.5% 包络误差。
点源现使用与已验证尺寸路径一致的 float 房间坐标平滑；对应案例误差降到约 0.02%。
空间内核和固定增益／时延没有改为逐案例补偿。

## 一键复现与验收

```sh
python3 scripts/research/dar_layouts/run_semantic_suite.py \
  --output-dir local/my-semantic-boundary \
  --channel-map local/dar-compat-suite-20260925/channel-map.json --candidate

python3 scripts/research/dar_layouts/run_semantic_suite.py \
  --phase final --freeze local/my-semantic-boundary/acceptance.json \
  --seed 0x26092653 --output-dir local/my-semantic-final \
  --channel-map local/dar-compat-suite-20260925/channel-map.json --candidate
```

`--resume` 核对案例、信号种子、源代码、分析器和 Release 可执行文件身份；模型变化会重新比较候选。
完整边界验收还要求代表案例的真实解析命中和混音验证。`--only`／`--skip-trace` 仅用于诊断，不能作为最终集冻结依据。
最终集使用新的固定随机时间线组合与独立 PRBS；若用于调整模型，必须降为回归并换新种子。

评分保留带符号增益、RMS、声道能量和能量占比的区别，并沿用已冻结的能量、频谱、互谱、尾部及动态门槛。
加入单帧意外静音检查，避免整体均值掩盖掉声；尾部沿用显式静音段和 PCM24 噪声底。
用户控制、重复渲染、跨事件裁剪另作集成校验。

套件逐例清理大 WAV；保留哈希、完整测量、关键片段和无损参考缓存。与基线完全相同的参考共享经 PCM 哈希确认的缓存。
导出桥通过一次 LLDB 安装驻留 Qt 传输，进程互斥防止导出争用，JSON 响应原子发布，结束核对 key 24／251 和母版／布局恢复。

本轮证据在 `local/dar-object-semantics/`。`failed-before-float-smoothing/` 保留原数值失败；
`failed-generator-and-transport/` 保留 DBMD 计数及传输故障，二者不作为算法失败或通过证据。
正式验收以冻结后的 `acceptance.json` 为准；最终汇总完成后记录在本页末尾。

## 本轮最终结果

154 个边界／字段组合案例在两布局全部通过；冻结后以种子 `0x26092653` 生成的 16 个新时间线组合
使用独立 PRBS，在两布局也全部通过。最终集未参与模型修正，固定映射、零时延和单位全局电平保持不变。

| 独立最终集指标 | 两布局实测最大值 | 门槛 |
|---|---:|---:|
| 归一化能量占比相对 L2 | 0.0100% | 5% |
| 总功率差 | 0.000015 dB | 0.1 dB |
| 有效声道三分之一倍频程误差 | 0.00885 dB | 0.5 dB |
| 归一化互谱矩阵误差 | 0.0153% | 5% |
| 可测尾部曲线误差 | 0.000312 dB | 1 dB |
| 动态能量包络 NRMSE | 0.0118% | 2% |
| 主要变化时刻偏差 | 48 samples／1 ms | 1 ms |

两组均未发现意外静音帧或 LFE 泄漏。全局音量、显式静音、规则优先级、重复渲染与跨事件裁剪通过；
裁剪与完整渲染截取逐样本一致。46/46 Debug CTest、20/20 研究脚本测试通过；既有运动和 gain 的 Release 回归通过。
DBMD 修复后的双对象接续与叠加均通过，生成前后检查和错误缓存拒绝有独立回归测试。

最终证据：`local/dar-object-semantics/verified/acceptance.json`、`final/acceptance.json`；
汇总：`local/dar-object-semantics/delivery-report.json`。上述完成状态仅适用于本页限定的语义与布局范围。
