# APAC 对象音频研究交接

更新：2026-09-25。本仓库保留研究源码、方法、文档和挑选的数值证据；
本地生成的音频、压缩包、PCM、抓包、调试数据库和临时目录已按用户要求清理。
用户提供的原始 ADM BWF/WAV 位于仓库外，没有删除或提交。

## 已完成的素材

| 源素材 | 场景 | 时长 | APAC 目标／实测速率 | 听音范围 |
| --- | --- | ---: | ---: | --- |
| Tsuioku | 7.1.2 BED＋32 位置对象 | 157.506396 秒 | 12／11.741898 Mbps | 用户确认无扩散对象版正常 |
| Man In the Mirror | 7.1.2 BED＋26 位置对象 | 314.725667 秒 | 12／11.078716 Mbps | 原生片段输出已观察；无用户整曲确认 |
| 前前前世 | 7.1.2 BED＋40 位置对象 | 76.660146 秒 | 12／12.091513 Mbps | 40 对象短样本进入空间输出；其后应用户要求停止额外播放实验 |

Tsuioku 的 10–58 秒片段使用系统压缩包直通裁切：2,252 个 APAC 包与全长文件中连续包逐字节一致，
通过容器时间信息得到 48 秒／2,304,000 有效帧，没有第二次有损编码。
上述文件名、源哈希、逐轨结果、元数据和容器结果见
[数值证据](evidence/apac_objects/README.md)。该目录没有音频文件。

## 当前编码方案

- BED **固定为 Core Audio Atmos 7.1.2 的 10 个标签及其顺序**：
  `L R C LFE Ls Rs Rls Rrs Ltm Rtm`。
- 其余每轨是一个静态单声道对象；自动分成每组件最多 7 个对象。
- 保留源 PCM 的各路音频和增益，以及对象方位、高度、距离。
  在本方案中明确将 diffuse 置零；只有源对象 `diffuse=1` 时才沿已验证的退路省略非零尺寸。
  原始字段保存在 sidecar。完整 ADM 扩散与尺寸语义尚未实现。
- 目标总码率默认 12,000 kbps，按 BED 和对象组件显式分配；实测速率由内容及编码器码率控制决定。
- 系统编码能力表、解码器和播放实现没有修改；脚本不进入产品 GUI/API 或默认 CTest。

主要入口是 [encode_fixed_712.py](../../scripts/research/apac_objects/encode_fixed_712.py)。
它只接受本研究支持的 48 kHz PCM24、静态 ADM BWF。BED 不匹配 7.1.2、动态对象或未覆盖的元数据会报错，
不会猜测布局。默认编码 CAF，再以压缩包复制封装 MP4，并清理大型暂存文件；
`--verify` 才运行完整的解码、元数据和容器验收。

```sh
python3 scripts/research/apac_objects/encode_fixed_712.py \
  'SOURCE_ADM_BWF.wav' local/apac-object-experiments/new-result
```

项目其他研究工具与完整参数见[工具说明](../../scripts/research/apac_objects/README.md)。
若源文件或 macOS AudioCodecs 组件改变，应重新运行相应验证。

## 回放与边界

QuickTime＋AirPods Max USB 曾验证位置对象的左右方向和原分组的 32 条独立测试音。
旧 `diffuse=1` 映射会让隔离人声只在开始约 85 ms 有输出，随后稳态为零。
自建探针与实际 USB 采集分别复现；关闭 diffuse 后该人声持续输出。
AVAssetReader 强制双声道的漏对象／居中现象属于离线转换，不能外推为 QuickTime 的实时播放结果。
调用链、控制样本和限制见[回放研究](APAC_PLAYBACK_CHAIN.md)。

对象数不是简单的 24 路总上限：单组件能力、整流组件数、编解码器默认能力以及元数据缓冲区分别制约不同阶段。
已有默认解码实验达到 70 对象；71 对象在该元数据表达下触及 4,096 字节输出缓冲区。
详见[输入协议](APAC_OBJECT_EXPERIMENTS.md)、[数量边界](APAC_OBJECT_BOUNDARIES.md)、
[LFE 混合](APAC_OBJECT_LFE_MIXED.md)、[混合容量](APAC_OBJECT_MIXED_CAPACITY.md)及[码率](APAC_OBJECT_BITRATES.md)。

先前试验生成的本地媒体和临时目录已删除。历史文档中的 `local/apac-object-experiments/` 路径记录当时实验位置；
需要这些文件时，使用研究工具与用户保留的源文件重新生成。
