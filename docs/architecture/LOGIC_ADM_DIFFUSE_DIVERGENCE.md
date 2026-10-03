# Logic ADM diffuse / divergence 调查

2026-09-26；Logic Pro Creator Studio **12.3.1 (6682)**；macOS **27.0 / 26A428**；arm64。
接续 [Apple Renderer 链路调查](LOGIC_APPLE_RENDERER_ALIGNMENT.md)。

## 结论与证据范围

对本版本已经追踪的 **ADM → Logic 工程 → Dolby 对象处理 → AppleSpatializer** 路径：

| 字段 | 解析层 | 工程/渲染映射 | 导出层 |
| --- | --- | --- | --- |
| diffuse | 原生解析器能保留 0、0.25、1 | 未进入已检查的导入自动化；字段 getter 的静态调用者只有对象 description | 根据 size 的有状态判断重新生成 `diffuse=1.0` |
| objectDivergence | Cartesian positionRange / polar azimuthRange 两种探针均被忽略 | 未发现 divergence 字段或对应映射 | 已检查的导出路径没有此项 |

这里的“解析器验证”是**独立进程加载本机未修改的 MAFiles.framework，实际解析合成 XML**，
不是仅根据字符串缺失推断。导出判断也实际调用了 MACore 中的原始函数。
最初的解析器/导出器探针不附加 Logic 主进程。随后通过
[完整后台 ADM 并轨链路](LOGIC_HEADLESS_BOUNCE.md) 实际导入六组 Cartesian 夹具并输出 PCM，
音频结果与上述结论一致，见第 4 节。临时工程和原工程状态按该工具的恢复规则处理。

因此，更准确的解释是：本路径没有把输入 ADM diffuse 当作独立的 direct/diffuse 混合比例；
divergence 则在导入解析阶段已经失去作用。不能由此推断 Apple/Dolby 所有其他入口的能力，
也不能将“忽略输入 diffuse”解释成 size 渲染完全没有去相关处理。

## 1. diffuse：可解析，未见渲染消费者

MAFiles 的 `-[ADMAudioBlockFormat_Object setSubElement:value:attributes:]`
（文件虚拟地址 `0x5fcf8`）比较标签名；匹配 `diffuse` 后调用 `floatValue` 和 `setDiffuse:`。
原生探针固定坐标和 size，只改变 diffuse，正确读回 0 / 0.25 / 1，其他已检查字段不变。

字段引用检查得到：

- `0x5fe54`：解析器调用 `setDiffuse:`。
- `0x60488`：`description` 调用 `diffuse` 以格式化描述文本。
- `_diffuse` ivar offset 的匹配引用只有 getter / setter；该项扫描覆盖了此构建所用的
  ADRP + LDRSW 地址读取形式，不宣称覆盖任意可能的机器码模式。
- Logic.framework 和 MAAudioEngine.framework 均无 `diffuse` / `setDiffuse:` Objective-C
  selector reference，也没有导入这个 ivar 的符号。
- Logic.framework 中 `diffuse` 字面量的唯一匹配代码引用位于 **ADM exporter**，不是 importer。

已逐段检查的 importer 创建位置、size、音量自动化，没有 diffuse 对应事件。
进一步检查 `Noon::ObjectMetadataCache::ProcessParamChanges`（MAAudioEngine，`0x90e074`），
其参数分支接受 `posx`、`posy`、`posz`、`size`，除以 100 后更新对象元数据；其他参数进入断言。
这不是整个 AU 只有四个参数的结论：BRM、trim 等有其他路径；它限定的是此对象位置/size 更新入口。

MAAudioEngine 自带的 Dolby `atmos_storage_adm` 另有 diffuse XML parser 和合法性校验符号。
这些符号属于另一套存储/解析实现，不能替代 Logic importer 已检查的数据流，
也不能据此声称 diffuse 数值会送入当前 Apple Renderer 监听路径。

## 2. divergence：原生解析器忽略

扫描应用顶层 Frameworks 的可执行文件，没有发现 `objectDivergence`、`divergence`、
`azimuthRange` 或 `positionRange` 字符串。进一步检查发现：

- `ADMAudioBlockFormat_Object` 的属性和已检查的字段处理分支没有 divergence。
- 不认识的标签由对象子类传给基类 `setSubElement:value:attributes:`；基类也没有 divergence
  分支，最后直接返回。
- `ADMDocument` 在 XML 元素结束时把标签名、文本和属性交给上述字段处理器。

原生验证共 11 组，均成功解析到一个 channel 和一个 object block，无解析错误：

| 对照 | 观察结果 |
| --- | --- |
| Cartesian 基线 vs divergence=0 / 1，positionRange=0.7 | 对象属性清单、所有读取字段及 description 完全相同 |
| Polar 基线 vs divergence=0 / 1，azimuthRange=60° | 同上 |
| diffuse=1 与 diffuse=1 + Cartesian divergence=1 | divergence 没有额外影响 |
| diffuse=0 / 0.25 / 1 | diffuse 正确保留，证明探针确实走过对象字段解析 |

通过 KVC 读取 objectDivergence、divergence、positionRange、azimuthRange 均得到
`NSUnknownKeyException`。结合解析分支和缺失的数据模型字段，可把“所测输入 divergence
被忽略”定位到解析层，而非猜测它在 SpatialMixer 里被某种扩散算法吸收。

## 3. 导出的 diffuse 是 size 派生标记，带迟滞

Logic 的
`-[MAADMBWFExporter translateAutomationValues:toAudioBlockFormat:atIndex:forTrackTableEntry:channelIndex:staticValues:]`
（`0xb22954`）把同一个 size 标量写入 width / depth / height，然后在 `0xb234bc` 调用
`DolbyDecorrelationCalculator::FeedNewSize(float)`。返回 true 才添加：

```xml
<diffuse>1.0</diffuse>
```

此处没有读取原始 ADM 的 diffuse；false 分支不添加该元素。
`FeedNewSize` 位于 MACore 的 `0x7b788`，原始函数的行为是：

```text
当前关闭：size >= 0.04 → 开启，否则保持关闭
当前开启：size <= 0.02 → 关闭，否则保持开启
返回当前状态
```

常量实际为 float32 的 `0x3d23d70a` 与 `0x3ca3d70a`。
独立探针通过 `dlsym` 调用原函数，13 步 size 序列验证结果如下：

```text
size   0 .01 .02 .03 .039 .04 .03 .021 .02 .03 .25 1 0
状态   0   0   0   0    0   1   1    1   0   0   1 1 0
```

同样的 size=0.03 可因先前状态不同而写出不同 diffuse 标记。
这些是 **ADM 导出标记生成** 的阈值，不能直接当作实时 size DSP 的全部开关/滤波数学。
“非零 size 一律 diffuse=1”不适用于这段 Logic 实现。

## 4. 完整 Logic 输出的成对实测

使用 AC4 Release CLI 生成的六秒轴向对象粉红噪声探针，创建六份独立 RF64 夹具。
每份都有 141 个 Cartesian 对象 block；PCM 与 dbmd 字节完全相同。
只在新夹具中改变指定 AXML 字段，原输入 SHA-256 保持不变。
Logic 不消费 MacinRender semantic policy，因此此项外部宿主对照必须使用实际 ADM 夹具。

均通过无窗口临时工程原生导入，使用 Apple Renderer / Music / Generic HRTF、头追关闭，
得到 288,000 帧 × 2 声道 Float32。格式、有限性、非静音和工程恢复检查全部通过。

| 夹具 | 变化 | PCM 对照结果 |
| --- | --- | --- |
| control | 缺省 size、缺省 diffuse/divergence | 基线 A |
| diffuse-1 | 每块 diffuse=1 | 与 A 逐字节相同 |
| divergence-1 | 每块 objectDivergence=1、positionRange=1 | 与 A 逐字节相同 |
| size04-diffuse0 | width/depth/height 均 0.4，diffuse=0 | 基线 B，与 A 不同 |
| size04-diffuse1 | 同 B，diffuse=1 | 与 B 逐字节相同 |
| size04-divergence1 | 同 B，另加 divergence=1、positionRange=1 | 与 B 逐字节相同 |

启用连续两遍 PCM 一致门槛后，六份夹具再次全部通过，各执行三遍后选取一致的第 2、3 遍。
A 的最终 PCM SHA-256：`725fde5aff22d7ce0bc3616da903c5953c12e8c31e95ccaf97589edf070b783f`。
B 的最终 PCM SHA-256：`3294a7d16d974cbee75088ca92ca3f565d20052d840164f12b5238e98832d8f6`。
size 对照证明本轮确实能把有效对象字段变化带入音频，并非整段元数据没有被导入。

该音频实测范围是本构建的 **Cartesian ADM 导入到 Apple Renderer / Music**、这一组六秒轨迹。
Polar divergence 目前仍只有原生解析器证据；不把它写成已经完成音频实测。
切换工程测试发现首遍输出可能带历史状态，因此这里报告经过重复门槛的最终结果，
不把所有首遍都概括为确定性结果。早期未经重复门槛的六组输出也保留在 `validation.json`；
其组内相等、组间不同的关系与最终结果一致。

夹具生成器：`scripts/research/logic_bounce/make_semantic_probes.py`。
本地结果：`local/logic-bounce-20260926/semantic-probes/manifest.json`、`repeat-verified-validation.json`，
以及每份输出对应的 `.logic-bounce/run.json`。

## 5. 对我们对齐工作的影响

- 当前 Apple 后端已经不处理 ADM diffuse。为匹配这条 Logic 导入路径，现有证据不支持
  单独增加一个按 ADM diffuse 比例工作的扩散器。
- 当前 Apple 后端会通过 `expand_object_divergence()` 展开多个相干点源；
  Logic 对所测 divergence 忽略，因此这是明确需要隔离的兼容性差异。
  下一轮比较可用 semantic policy 关闭 divergence，保留 extent/size，不改源 ADM。
- size 必须单独对齐。关闭 diffuse 不等于关闭尺寸，也不等于关闭 Dolby size 内部的去相关。
  导出文件中出现 diffuse=1 不能单独证明 Logic 监听在消费这个输入数值。
- 上述行为应只作为 Logic 兼容目标，不应据此删除其他后端对标准 ADM 语义的支持。

## 复核材料

本地目录：`local/logic-apple-renderer-20260926/`。

- `semantics-manifest.json`：Logic / MAFiles / MACore / MAAudioEngine 的路径和 SHA-256。
- `macho_semantics.py`：只读 Mach-O selector/literal/reference 检查及反汇编注释工具。
- `diffuse-divergence-strings.json`、`diffuse-divergence-xrefs.json`、
  `diffuse-divergence-literal-xrefs.json`：字符串、selector 调用和字面量引用结果。
- `mafiles-semantics-annotated.txt`：字段解析、getter/setter 与 XML 元素处理。
- `adm-import-annotated.txt`、`adm-export-annotated.txt`：工程自动化导入及 ADM 导出。
- `object-metadata-disassembly.txt`：对象参数进入元数据缓存的路径。
- `parser_probe.m` / `parser-probe.json`：原生解析器的 11 组输入与结果。
- `decorrelation_probe.m` / `decorrelation-probe.json`：size 派生标记原函数的调用结果。
- `decorrelation-calculator-disassembly.txt`：原函数的阈值与状态分支。

探针使用 `xcrun clang -O2 -fobjc-arc -framework Foundation` 编译，并给可执行文件设置
指向本机 Logic `Contents/Frameworks` 的 rpath；运行时加载现有框架，无应用副本或新构建缓存。
原生探针均正常退出，stderr 为空；字段隔离与迟滞序列断言通过。
