# 原生 96 kHz 房间与 SkinRLC 距离集合

`room-binaural` 的离线渲染、文件流式渲染和 Scene 流式渲染支持 48/96 kHz。
处理时钟跟随素材，房间包必须使用相同采样率。96 kHz 素材需要选择 96 kHz 房间包；
不匹配会报错。Scene 可在房间处理之后转换到设备输出采样率。

## 集合入口与兼容性

`--room-hrir`、`room-check --hrir` 和 C ABI 的 `hrir_manifest` 接受目录、索引或单 SOFA。
目录解析其 `distance_index.json`。新增 `mradm.sofa-bank` v1，继续接受
`mradm.hrtf-collection` v1；两者的距离层支持 48/96 kHz `HRIR_DFEQ100`。
SOFA bank 没有参考层字段时采用 1.5 m，缺少该层时报错。

SkinRLC 原始索引的九层为 0.2、0.3、0.4、0.5、0.75、1、1.3、1.5、1.6 m，
每层 1850 个方向。96 kHz 路径保留原始 1920 taps 和已包含在 FIR 内的 84 样本公共位移，
没有 48 kHz 中间处理，也不重复应用 EQ、传播或归一化。48 kHz 会话使用完整支持重采样。
直达按真实声源距离、早反射按完整镜像路径长度选层，晚场使用参考层；越界只钳制 HRTF。

加载时认证文件长度、SHA-256、实际采样率、半径、方向顺序和耳序。逐层来源计数来自经过认证的
`MRADMPointOrigin` / `MRADMPointOriginHighBand`；交付目录的 `quality_report.json` 作为附带
证据展示，不改变滤波。0.2/0.3 m 的 DVF 虚拟替代标记不会被清除。

检查 JSON 增加 `source_sample_rate`、`hrir_resampled` 和 `hrir_taps`；`sample_rate` 为处理时钟。
预处理 HRIR 未记录原始采样率时，前两项为 null。公开 C ABI 符号和 POD 大小保持不变。
源数据、准备频谱及会话分别持有采样率，缓存身份包含处理时钟；不同采样率的活动会话可并存。
换房不能改变会话采样率，失败时保留当前房间。

## 时间与资源契约

| 项目 | 48 kHz | 96 kHz |
|---|---:|---:|
| 滤波器过渡 | 480 帧 | 960 帧 |
| 换房过渡 | 4800 帧 | 9600 帧 |
| 实时反射／头姿周期 | 1024 帧 | 2048 帧 |
| 内部 FIFO／短卷积分区 | 256 帧 | 256 帧 |
| Diffuse 最长物理延时 | 31 帧 | 62 帧 |

反射源 ID 相位保持相同物理时刻。81 taps 分数延时核和 1024 相位插值表仍按样本定义；
完整响应、保护区、尾声及 seek 预热按实际资源和时钟计算，不按能量裁剪。
旧 48 kHz 入口保留原始数值路径和资源身份。

集合准备预算继续为 1 GiB，涵盖受控数组及最大的临时频谱；它不是整个进程 RSS 上限。
实际 SkinRLC 96 kHz 查询 bank 为 825,275,648 字节，公共插值保护为 121 帧，输出支持 2176 taps。
活动会话另有卷积历史、目标滤波器和工作区。准备在控制线程完成，处理不分配或读文件，
不建立磁盘缓存。

## 新房间预设

`reference-7x6x3.2-fullband-96k-v1` 独立于旧 48 kHz 校正参考包，继承几何、监听点、
距离约束、四阶反射、世界方向基和可听频段目标。生成器为
[`generate-96k.py`](../../scripts/room/generate-96k.py)。

- 材料 FIR 为 1024 taps，沿用八频带；20–24 kHz 平滑过渡到最高频带高棚，延续至 Nyquist。
- 共用最小相位 EQ 为 32768 taps，保留 300 Hz 以下修正，300 Hz 以上目标为 0 dB。
- 晚场保留校正包的可听响应，以完整支持、传递函数增益守恒的转换进入 96 kHz。
  新高频用 PCG64 固定种子 1 合成有限噪声，衰减和共同增益从原包 12–20 kHz 提取；
  20–24 kHz 使用互补功率交叉带，不对方向分别归一化。
- 晚场共同准备保护为 532 帧，在发送预延时中补偿，最终物理预延时仍为 18 ms。
  生成器和依赖版本、原包哈希、频段规则及高频衰减目标均写入清单。

该高频房间响应是合成模型。新包的 28 个参考位置单独通过既有 RT、频响、早反射和声道差门槛，
高频另外验证数值传递，不能解释为实测超声房间或听感认证。证据见
[`evidence/room-96k`](evidence/room-96k)。

## 使用与复现

```sh
build/release/mradm room-check \
  resources/rooms/reference-7x6x3.2-fullband-96k-v1/room.json \
  --hrir /path/to/SkinRLC_96k_AllDistances_v1

build/release/mradm render -i programme-96k.wav -o binaural-96k.wav \
  --renderer room-binaural \
  --room resources/rooms/reference-7x6x3.2-fullband-96k-v1/room.json \
  --room-hrir /path/to/SkinRLC_96k_AllDistances_v1
```

48 kHz 素材选择原有 48 kHz 房间包，可使用同一原始集合目录。
`room_hrir_check COLLECTION --sample-rate 96000` 扫描所有节点、距离中点和边界。
[`validate-native-96k.py`](../../scripts/room/validate-native-96k.py) 使用 Release CLI 和 omni 探针，
验证原始九层加载、三分支脉冲高频传递，以及真实集合的 30/40 kHz 正弦输出。
新小型夹具不含私人数据；私人集合不纳入仓库。

维护者验收平台为 macOS arm64 和 Windows x64。两平台的精确档位回放比较全部准备检查点、
HRIR／分支和 PCM，覆盖 48 kHz、96 kHz，以及屏幕／密集运动／头姿接缝组合。
Linux 测试机已下线，不属于后续验证矩阵。
