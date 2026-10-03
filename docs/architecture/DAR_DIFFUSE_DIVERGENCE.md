# Renderer 5.5：diffuse 与 objectDivergence 调用链

2026-09-26。本轮限定 Renderer 5.5.0 release 直接 ADM re-render，48 kHz、7.1.4／9.1.6、
Cartesian 点源和等尺寸对象。使用新合成 ADM，不使用编码全景声或 GUI 操作。

## 结论

`objectDivergence` 在此路径中没有对应的 block 子元素处理器，未进入对象参数。
`diffuse` 则确实被解析：数值先转换成非零／零布尔值，存入 block，再传到 OMO。
但当前扬声器路径按 size 决定尺寸混合和滤波，随后清除交给 OAR 的原对象分支上的 size／diffuse 标记。
因此，本批 diffuse 和 divergence 字段变化没有改变最终 PCM。

这解释了为什么 size>0、diffuse=0 仍有去相关处理，以及为什么 size=0、diffuse=1 没有产生独立扩散效果。
本结论是该参考入口的兼容行为，不是对 ADM 标准含义的重新定义。

## 运行命中与字段流

模块身份沿用已冻结 Renderer：arm64 UUID `F11646CF-9535-3815-8184-524335D4CC3A`；
安装二进制 SHA-256 `ec9432d6af6679cfdf5cc668d37be0b3ac295ea9453b264ec84e0d1706bb2390`。
下列是静态 VA，运行时按实际加载基址重定位并检查原虚表指针。

| 环节 | 函数／结构 | 确认内容 |
|---|---|---|
| Objects block 分发 | `0x102373eec` | `diffuse` 建立处理器；实测八个 `objectDivergence` 标签均无 delegate |
| diffuse 处理器构造 | `0x102374a84` | 数值转换器虚表为 `0x103d365d8` |
| 数值解析与转换 | `0x1023761b4` | 将解析得到的 double 以 `value != 0` 转成布尔值，调用消费者槽 `+104` |
| block 消费者 | `0x102290e54` | 设置 `+22` presence mask 的 `0x200` 位，将布尔值存到 `+44` |
| block 提交 | `0x102290bf4 → 0x10228f5fc` | 正常导入中的回调目标已通过实时虚表读回定位 |
| OMO 输入事件 | 72-byte event，`+52` size，`+56` diffuse bool | 动态输入中的 diffuse 标记真实变化，不能说它在解析阶段丢失 |
| OMO 滤波启用 | `0x101e6b2ac` | 此配置中以 size 和已有滤波状态决定处理，不以 diffuse bool 作为扩散比例 |
| 尺寸增益参数转换 | `0x101e6e754` | 从事件 `+16` 的参数区读取 XYZ、size、zone、height 等；没有读取对应 diffuse 的槽位 |
| 原对象分支清理 | `0x101e6f0cc` 尾部 | 清零事件 `+48/+52` 与 `+56`，已处理的原对象 PCM 交给后续 OAR |

动态 diffuse 序列 `0, .01, .25, .5, .75, 1, .5, 0` 的消费者布尔记录为
`0, 1, 1, 1, 1, 1, 1, 0`。在动态 size=0.25 捕获中，OMO 输入到原对象输出的标记对为
`(0,0)` 或 `(1,0)`；输入为 1 的 18 个采样块均在原对象分支被清成 0。
size=0、diffuse=.5 的捕获中，31 个采样块均为 `(1,0)`。

包装器只转发正常调用并复制内存，没有额外调用引擎方法。输入事件的前后副本、导入标签、delegate
和布尔 setter 分别记录，未知字段仍保留原始字节。有追踪与无追踪 PCM 逐样本相同；
捕获的 OMO／OAR 混音重建最大绝对误差约 `6.7e-8`。

## 字段隔离验证

5 个基线组、26 个字段变体，共 31 份新 ADM。每份导出两布局。
变体与基线的 PCM、CHNA、DBMD、时长及非目标 XML 内容均核对相同；对象名称和 block 时间也不随变体改变。

- 静态 size=0／0.25：diffuse 省略及 `.01/.25/.5/.75/1`；divergence 的非零值配合 `.25/1` positionRange。
- 静态尺寸下，diffuse、divergence 分别变化及同时变化。
- XYZ 运动配合 size=`0/.01/1/.25`，交叉改变 diffuse 或 divergence。

**26 个变体在两布局的全部样本均与各自基线相同**，包括显式静音尾段。
另用原始变体 ADM 在 Release CLI 中通过 semantic policy 关闭这两个字段，保留 size，
点源与非零尺寸动态代表案例均通过既定能量、频谱、互谱、尾部和包络评分。
这是诊断性核对，不是修改源 ADM，也不代表生产接口已扩大支持范围。

## 脚本与边界

```sh
python3 scripts/research/dar_layouts/trace_spatial_semantics.py \
  --output-dir local/my-spatial-semantics --resume
```

`--only static_0,dynamic_0.25` 可只运行指定组。脚本生成基线和字段变体、检查 DBMD 拓扑及字段隔离，
调用驻留导出桥、核对状态恢复并比较完整 PCM；相同的变体 WAV 清理后保留其哈希和完整基线。
用 `run_gain_probe.py` 与本轮 profile 采集，再用 `analyze_spatial_trace.py` 汇总导入与 OMO 字段流；
完整混音验证仍使用 `measure_gain_trace.py`。

证据位于 `local/dar-spatial-semantics/`：`probes/summary.json`、`traces/*/spatial-semantics.json`、
`traces/*/gain-validation.json`、`candidate-diagnostics.json`。静态代码复用主要 IDA 数据库，
局部导出保存在 `local/dar-gain-trace/spatial-semantics*/`。

本轮只扩展研究入口与证据，生产 `triple-balance` 的校验范围保持原状：中间 diffuse 值、纯 diffuse 点源和
非零 divergence 仍未自动开放。后续可把已确认的行为接入为明确的忽略规则，并在 renderer_effective 中报告。
Polar、非等尺寸、其他 renderer／双耳路径及其他修饰字段组合没有由本批实验覆盖。
