# AVPlayer Dolby Atmos 实时调用链与 7.1.4 tap

2026-09-25，macOS 27.0 / 26A428。原始证据位于
`local/atmos-playback-chain-20260925/`；研究工具位于
`scripts/research/atmos_playback/`。本轮没有修改产品渲染器。

后续已打通同一系统解码器的指定标准布局／同步离线调用，并与本报告实时参考完成定量对照，
见 [原生 Atmos 离线渲染](ATMOS_NATIVE_OFFLINE.md)。

## 结论

**本机、本样本的原生 AVPlayer 确实先把 E-AC-3 JOC 渲染为 7.1.4，随后再做面向输出设备的空间化。**
加入 `MTAudioProcessingTap` 可以实时取得这一级的 PCM；无 tap 的对照也选择相同的 7.1.4 输出格式。

对象到 7.1.4 的处理发生在系统 `AudioCodecs` 组件内的 `ACDDPAtmosDecoder`，实际命中了
Dolby JOC、对象元数据和 OAR / OMG panner 路径。不能把它等同于项目 Apple 后端所调用的
`AUSpatialMixer` 通用 `VectorBasedPanning`，也不能仅凭“都有声像分配”把它当成 SAF VBAP。

## 样本与条件

- 输入：用户目录中的 `Tsuioku_SS2RPV4_ec3_1024K_drc_none.m4a`。
- MediaInfo / ffprobe 确认 E-AC-3 JOC，48 kHz、1024 kbps，兼容层为 5.1。
  MediaInfo 报告 complexity index 16、15 个动态对象及一个 LFE bed 声道。
- 最终无调试器采集：正常播放源时间 56–63 秒，提取 57–62 秒；断点观察：57–60 秒。
- 当时默认输出为 **MacBook Pro 内置扬声器**。调用链观察仅静音自建 AVPlayer，
  最终定量采集则正常播放该进程；未改变系统设备、音量或空间音频偏好。
- 探针以 `-O2 -g` 编译。tap 回调仅复制到预分配内存，原样返回系统音频；退出时才写文件。
- 断点只观察自建进程，不改寄存器、属性、返回值或媒体。带断点的数据用于调用链定位，
  定量 PCM 对照使用无调试器的实时采集。

## 实际调用链

```text
AVPlayer / MediaToolbox / AudioQueue
  → AudioConverter：ec+3 → 12ch float PCM
  → ACDDPAtmosDecoder::ProduceOutputBufferList
      → ddp_udc_int_mainprogram_process
          → ddp_udc_int_jocd_process_one_frame
      → oar_process_safe
          → oamd_framer_get_metadata
          → omg_process
              → omg_panner_process
                  → obj_render_process
                      → point_panner_get_gains
                          → point_panner_compute_gains_in_layer
                      → size_panner_get_gains
  → 7.1.4 PCM
  → MTAudioProcessingTap（启用时；可以取得这一层的音频）
  → MEMixerChannel::ConfigureSpatializationUnit / 后续系统空间化
      输入 12ch，输出 2ch，BuiltInSpeakers / UseOutputType
  → 设备输出链路
```

以上树状结构概括调用关系，不把 JOC 和 OAMD 两条分支表述成每个对象都严格串行经过的单一函数链。
`point_panner_compute_gains_along_rows`、`point_panner_compute_gains_between_rows` 也实际命中，
保存的首批栈是在 `room_config_size_panner_init` 初始化阶段。函数名和调用栈仍不足以确定全部增益公式。

格式协商初期还观察到了 16ch `ec+3` / PCM，随后实际播放输出明确改为 12ch。
不能把前面的 16ch 直接解释为 9.1.6，也不能把 JOC 重建信号当成原始 ADM 母版的独立对象轨。

## 7.1.4 的确认依据

`trace-none.log` 与 `trace-pre.log` 都包含：

1. `AudioConverterSetProperty` 和 `AudioCodecSetProperty` 设置 `ocl ` 为 **`0x00c0000c`**，且返回成功。
   SDK 定义为 `kAudioChannelLayoutTag_Atmos_7_1_4 = (192 << 16) | 12`。
2. 下游空间化单元输入布局同样是该 tag；输入 ASBD 为 48 kHz、12ch float32。
3. 该单元输出为 2ch；属性 3100 = 2（BuiltInSpeakers），3000 = 7（UseOutputType）。
4. 前置、后置 tap 均实际返回 12 个单声道 float buffer，而非只声明一个格式。

`dolby-v2-none.log` 与 `dolby-v2-pre.log` 都实际命中上面的 JOC、OAMD、point panner、size panner。
两组均正常播放到 60 秒，没有观察器内存读取错误。
较早的 `dolby-*.log` 因组合断点的限流粒度过大，遗漏了部分后续函数；以 `dolby-v2-*.log` 为准。

CoreAudio 的 PCM 声道顺序为：

```text
L R C LFE Ls Rs Rls Rrs Vhl Vhr Ltr Rtr
```

比较 WAV 或其他工具输出时必须按标签重排，不能默认侧环绕和后环绕所在的槽位相同。

## PCM 验证与参考片段

最终采用 **正常播放、前置 tap、1 秒预滚和1 秒尾部余量**。两次独立播放均取源时间 57–62 秒：

- **240,000 帧 × 12 声道**，不同采样数 **0**，最大绝对误差 **0**。
- 两组均无格式拒绝、无缓冲溢出，PCM 全部有限；12 路均有非零信号。
- 240,000 帧均有信号，没有整帧归零；每个目标帧的源时间覆盖恰好一次，没有时间戳重叠或缺口。
- 这是两次有 tap 的中间 PCM 一致性，不是无 tap 与有 tap 最终设备输出的逐样本一致性证明。

tap 会返回预热回调和预取数据，不能把回调总帧数直接当作用户请求的节目长度。
后置 tap 的时间戳也不是简单的源时间逐帧映射，因此参考片段使用前置 tap 的连续源时间戳提取。

已保存 `tsuioku-57s-62s-apple-atmos-714.caf`：**240,000 帧，5 秒，48 kHz，12ch float32**。
这是实时采集 PCM 的截取和封装，没有离线解码／渲染、重采样或归一化。
`afinfo` 确认 7.1.4 标签、顺序和有效帧数；CAF 数据区与提取出的原始 PCM 哈希一致。

PCM SHA-256：`a4dc4a612d63c7008c9063a966b921790509137314be8aad2192c32878bc78e5`。
完整指标、源文件哈希、原始采集哈希见 **`summary-final.json`**；最终封装核对见 `caf-final-verification.json`。

### 早期静音采集不能用作完整参考

最初 `player.muted=YES` 时，前置／后置 tap 对齐 1484 帧后，305,716 个重叠帧逐样本相同。
但进一步改变 seek 起点发现，约播放 3.45 秒后，tap 的时间戳仍前进，PCM 却持续归零。
正常播放对照消除了这种过早归零，说明静音状态影响了 tap 数据；不能把它当作源曲静音。
这个例子也说明，两路结果相同、声道数正确、时间戳连续，都不足以证明节目音频完整。

最初的 `summary.json`、`repeat-verification.json` 和 `preroll-verification.json` 保留为诊断记录，
已被最终结果取代；其中静音生成的两个 CAF 已删除，避免误作声场基准。
原始 `pre` / `post` 的静音数据保留用于复核这个现象。删除的中间数据有路径、大小和哈希记录。

## 对当前 VBAP 对齐工作的含义

对齐应先在 **对象 → 7.1.4** 这一层进行，并把两边的 7.1.4 送入同一个系统空间化路径。
这样不会把对象 panning、设备空间化和最终听感混成一个不可辨别的问题。

当前 SAF 路径在 `vbap_renderer.cpp` 调用 `generateVBAPgainTable3D_srcs`，并把 width / height / depth
通过 `60 / 45 / 20` 的经验系数、距离缩放和范数压成单一 spread 角。
本轮观察到的 Dolby 路径有独立 point / size panner；存在实质算法差异的可能，不能靠整体增益或均衡保证对齐。
这些差异目前是后续测量目标，尚未量化各自对本曲声场的贡献。

下一步适合做受控位置／尺寸探针：固定源信号，逐个改变对象坐标、size 和运动，
通过 AVPlayer 实时 tap 提取每个输出声道的增益与变化包络，再与 SAF 比较。
先校准声道顺序、节目时间、dialog normalization / DRC 和布局，随后测点源，再测尺寸和移动。
JOC 的联合编码及对象聚类可能改变对象表示，需要同源测试素材或记录实际解码对象参数，
不能把任意 ADM 与成品 JOC 的全曲差值都归给 panner。

项目已经提供 `--speaker-geometry apple`，可作为几何差异的受控对照；
但 CoreAudio 对标签的坐标展开不等于已经验证 Dolby OAR 内部使用相同的几何与增益公式。
如果测得稳定差异，再决定增加独立的兼容 panner，或改进现有 spread 实现。

本轮没有验证其他 macOS 版本、AirPods 输出、HDMI/MAT、TrueHD Atmos 或完整的离线能力边界。
它确认的是这次实时 AVPlayer 路径，不能外推为所有苹果 Atmos 播放场景都固定使用 7.1.4。
