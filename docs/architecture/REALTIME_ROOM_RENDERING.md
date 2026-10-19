# 原生实时房间渲染

状态：v1 功能已实现，提供精确与实时两档；48 kHz 三平台数值与参考波形回归通过。
新增 96 kHz 处理和距离集合入口，独立证据见 [原生 96 kHz 房间](ROOM_NATIVE_96K.md)。
实时容量按静态、头姿、换房和密集运动分别报告，档位本身不保证任意负载都满足截止时间。
既有证据见 [验证记录](REALTIME_ROOM_VALIDATION.md)，档位对比见 [第一版档位验证](ROOM_QUALITY_V1.md)。
Windows 实时档的 [设备长测](ROOM_WINDOWS_DEVICE_V2.md)已覆盖文件播放和高密度 Scene 输入；
所测正常播放负载通过十分钟零欠载验收。每 5 秒换房叠加高密度运动的结果单列为压力余量边界。
传感器到耳机的物理延迟未测。

## 契约

`room-binaural` 使用项目自有 Rust 房间内核。离线渲染、文件流式渲染和 Scene 流式渲染
共享该内核，直接接收 Objects / DirectSpeakers；不经过固定扬声器中间总线。
支持 48/96 kHz、固定监听位置、3DoF、worker 头追、CLI / C API，不包含生产 HOA 或 GUI。

输入默认按房间几何自动映射；显式 `metres_per_unit>0` 使用固定米数比例。Scene `(x,y,z)` 转为房间 `(y,-x,z)`。
自动模式以听者为原点，各轴正负 1 对应保留留距后的墙面；详见 [坐标映射](ROOM_COORDINATE_MAPPING.md)。
沿射线把源限制到距边界 0.15 m 的室内，最近距离 0.25 m，并报告实际位置。
传播按实际距离计算，以 2.1 m 为单位增益参考。位置 / 头姿滤波过渡持续 10 ms；
没有变速读延时线或多普勒模型。单半径 HRIR 的距离使用属于近似。

首个房间保留 7 × 6 × 3.2 m、监听点 `(3.4,3,1.2)`、声速 344.02 m/s、
余弦平方源指向性、四阶镜像路径、15–25 ms 早反射渐退、18 ms 晚场预延时、
32 个世界方向有限晚场和共用 EQ。0.94 为混合模型的早反射能量吸收参数，不代表实际建材。
频带材料 FIR 保留完整因果支持：运行时不提前移走其 `band_filter_offset`，同时给晚场和
headLocked 分支加入同样的保护延迟。参考包为 256 帧（5.33 ms），单独通过
`material_filter_guard_frames` 报告；这与 256 帧处理 FIFO、HRIR 原有保护延迟分别记录。
参考重建仅在验证端扣除该声明的延迟，真实播放保留它，近距离不裁前端也不循环移位。

个人 HRIR 使用原球面网格、每耳附加时延和连续时延对齐插值；不做逐耳或逐方向归一化。
辅助最小相位响应只用于估计时延，不替换个人 HRIR。

`headLocked` 使用头部坐标的干声 HRIR，保持真实距离传播和 `2.1/r`，不经过世界旋转、房间 EQ 或反射。语义 LFE 输入保持独立，使用共用核，
不附加 +10 dB、分频、低频重定向或 split-power。extent / divergence / diffuse
仍为声源语义，不能把 diffuse 当作房间湿声旋钮。

## 精确与实时档位

默认 `exact` 保留现有标量 FFT、控制更新和跨平台数值基线。显式选择 `realtime` 时，
房间实例启用原生 SIMD FFT，并在连续声源几何斜坡期间将早反射／晚场发送更新改为
1024／2048 帧周期（48／96 kHz 下均约 21.33 ms），按声源 ID 错开相位。直达仍按原更新点处理；
跳变、离散语义、字段清除等立即刷新。实时策略 `room-realtime-v2` 对纯头姿更新允许
早反射目标最多保持一个反射周期、相对输入偏差最多 0.5°；较大头动立即刷新。
头姿变化若需要补齐声源几何，仍按原时刻完整更新。直达和晚场头姿解码保持原更新点。
这些上限不包括既有 10 ms 滤波器淡化。镜像阶数、所有有效路径和 FIR 支持不变。

两档可同时使用；档位在会话创建时固定，换房和 epoch reset 保留它。实时档位保持
同一二进制／CPU 能力下的分块位一致性，允许不同架构的 PCM 位差。seek 预热增加一个
反射周期，处理 FIFO 与有限尾声不变。详见 [ADR 0020](../adr/0020-room-quality-tiers.md)；
性能、波形误差和当前边界见 [第一版验证](ROOM_QUALITY_V1.md)。
v2 的参数对照与头姿误差见 [头姿预算验证](ROOM_POSE_BUDGET.md)。2026-10-18 采用了
[准备并行与活动缓存优化](ROOM_PREPARATION_OPTIMIZATION.md)。同轮 v3 算术候选因 Windows
短测失败而撤回，运行时仍为 v2。

```sh
build/release/mradm room-check /path/to/room.json --quality realtime
build/release/mradm render -i programme.wav -o binaural.wav --renderer room-binaural \
  --room /path/to/room.json --room-quality realtime
```

C++ 使用 `RoomConfig::quality = RoomQuality::realtime`。C ABI v1.48 提供：

- 文件渲染／文件流式渲染：`adm_render_options_set_room_quality(options, ADM_ROOM_QUALITY_REALTIME)`；
- 检查：`adm_room_inspect_with_quality(context, config, quality, out_json)`；
- Scene 流式渲染：`adm_create_scene_stream_with_room_quality(context, scene, room, quality, screen_json, out)`。

原有入口仍为精确档位，`adm_room_config_t` 大小不变。`set_room`／`clear_room` 保留 options
选定的档位。检查 JSON 报告 `quality`、`processing_policy`、`fft_policy` 和
`reflection_update_frames`，以及 `reflection_pose_limit_degrees`／`reflection_pose_update_frames`；
状态 JSON 报告档位与几何周期，能力 JSON 在 `qualities` 中列出两档。
此档位名不表示所有主机都能满足实时预算，尤其频繁头姿仍可能触发全部声源反射重建。

## 资源与处理

资源清单使用版本化 JSON，数值组件为指定形状的小端 IEEE f32 / f64 或 u32 文件，
逐文件记录 SHA-256。房间与 HRIR 清单分别存储，可共享组件；不写入开发机器绝对路径。
准备完成后全部使用内存资源，处理不访问外盘。运行时不依赖 Python、NumPy 或 PRA。

`RoomProfile` 为房间数据，`PreparedRoom` 为不可变计算数据，`RoomSession` 保存播放状态。
内部 256 帧调度独立于外部提交大小。短响应使用 256 帧分区，长 FIR 的前 2048 个样本
使用 256 帧分区，后段使用 2048 帧分区。晚场在世界坐标生成后按当前头姿双耳化。各方向共用同一输入 FFT 历史，方向滤波与累加顺序不变。
切换中的两个房间分别保留有限晚场历史，按相同世界方向合并后使用一个持续的 HRIR 解码器；
这样头姿始终作用于两份尾声，切换不重新初始化解码历史。v1 切换要求晚场方向基一致
（方向数量、顺序及 f64 数值相同）；包生成器继承模板的方向基。
冻结 EQ 有 16384 taps，应独立处理并保持历史，不能在每次头姿变化时重建长 BRIR。
路径按整数传播延时分段合成：每段 FFT 包含完整 HRIR、材料 FIR、分数传播 FIR 与段内延时，
逆变换后再加到绝对样本位置。参考包使用 2048 点查询 FFT、512 帧延时段；所有有效镜像路径
仍参与计算。段内按声源采样与镜像编号累加，段间按延时段编号累加，明确固定 f32 舍入顺序。
这一重排需要重新做三平台逐位回放，并以旧 BRIR 的 2e-4 门槛检查波形；不是能量裁剪。

## 切换与时间线

后台准备，确定样本位置提交，默认 100 ms 过渡。直达线性交叉淡化；反射 / 晚场逐渐
转移输入激励，旧历史自然排尾，旧晚场仍接收最新头姿。最多两个活动房间实例，
后续请求合并为最新待处理目标。加载失败保持当前房间，相同配置选择为 no-op。

声源身份为 `(element_id, generation_id)`。新 generation 即使重用相同 element ID，也新建激励历史，
旧 generation 保留原位置的尾声。源移除不截断历史；seek / epoch reset 清空历史和过渡、保留配置，文件 seek 预热有限历史。
完整输出追加有限 FIR 尾声，精确裁剪仍按显式窗口长度。素材、尾声及缓冲帧分开计数，
排尾和输出消费结束后报告 EOS；不按浮点能量阈值缩短输出。

## 验收边界

固定输入资源、声源 PCM / 状态、事件样本位置与初始状态，比较房间输出，
不要求本任务处理上游或设备侧的一致性。几何 / 时延为 f64，PCM / 卷积为 f32，
exact 固定归约顺序、分别舍入乘加；realtime 候选允许 FMA 与固定分组重排，须通过 ADR 0020 的
舍入误差和性能门槛才可采用。超越函数使用锁定 libm。C 边界为房间操作建立 RNE / 渐进下溢
环境，并恢复调用方的浮点控制寄存器；不修改其它后端的数值策略。

22.2 的 22 个参考全频位置和 6 补点是回归数据，不是运行方向网格。
校准后重建 BRIR 的初始相对 L2 门槛为 2e-4；原来的声学门槛不放宽，正下方补点
原有部分 RT 超限记录保留。真实距离运行不自动施加参考测量的增益 / 延时校准。

需覆盖距离、墙边、极点、方位接缝、组合旋转、频繁重定向、切换失败、快速切换、
源移除、LFE、headLocked、碎片分块、seek、epoch、EOS 和裁剪。Release 进行
macOS arm64 / Windows x64 / Linux x64 逐位比较及 1 / 16 / 32 源性能测试。
32 活动点源为首版实时容量目标；不得通过按负载删反射改变输出来通过测试。

过渡采用 10 ms 有限窗口的目标滤波器平均，新目标从零权重进入，避免连续重定向
引入无限处理历史。每个动态滤波器预分配 16 个窗口内目标，超出更新容量明确报错。
每个声源只在自己的更新点重新计算几何与目标滤波器：块起点、头姿变化、声源集合变化、
输入结束，以及该声源自己的几何事件、初始命令和实时覆盖变化（ADR 0019）。其他声源的事件
不会让它重新取样斜坡，所以它的输出与其他声源的事件时间无关，目标数也不随声源数增长。
head-locked 干声分支同样处理，只是头姿变化不作为它的更新点。
球面公共边和顶点的三角面选择不依赖上次查询缓存，以保证 seek 重建。

依据：本会话确认的实施方案，以及 ADR 0005、0007、0014、0015、0018。


## 接口与新增房间

CLI：

```sh
build/release/mradm room-check /path/to/room.json --hrir /path/to/hrir.json
build/release/mradm render -i programme.wav -o binaural.wav --renderer room-binaural \
  --room /path/to/room.json --room-hrir /path/to/hrir.json --room-metres-per-unit 1.5
```

直接传 `--sofa personal.sofa` 会在创建会话时由原生 Rust 完成解析、按房间采样率完整支持重采样、
时延处理、时延估计及网格准备，不需要 Python 或预先导出 `hrir.json`。`--room-hrir` 也接受
SOFA，原 JSON 入口继续兼容。两者均省略才使用包内默认绑定；显式 HRIR 与 SOFA 同时指定时
仍核对来源哈希。原生导入独立于其它双耳后端的 SOFA 可选构建开关；它不访问处理回调，
数据以弱缓存复用于活动会话，不写磁盘缓存。距离比例范围为 0.01–100 m/unit。
文件实时编辑支持 gain/mute、extent、diffuse、divergence 与 headLocked；这些控制仍作用于声源。

`--screen-config screen.json` 为房间后端指定制作／重放屏幕，例如：

```json
{"schema":"mradm.screen-config","version":1,
 "reproduction":{"coordinates":"polar","centre":[0,0,1],"width":90,"aspect_ratio":1.78}}
```

`coordinates` 为 `polar` 或 `cartesian`；polar 的 centre 是方位角、仰角、距离，width 为角度；
cartesian 使用 ADM x/right、y/front、z/up，width 为 X 宽度。每个定义需 centre、width、aspect_ratio。
可选 `reference` 显式覆盖制作屏幕；否则文件按对象所属 Programme（包括父对象继承）读取，
缺省使用 58°／1.78 的标准制作屏幕。多个 Programme 定义不一致时需显式覆盖。
未配置 reproduction 时保持原来的物理位置，screenRef 会给出一次缺少重放屏幕的诊断。

映射使用 BS.2127-1 §7.3.3 的极坐标屏幕边界，参考 EBU EAR Python 2.0.0
（`10149bb07eeb8a92c34a5e10d2f0597e3bcdfbde`）。Room 接口中的位置是物理射线，
所以在原始控制插值之后改变方向、保留半径，再执行房间边界约束；不套用扬声器布局补偿。
screenRef 开关按事件样本生效，独立位置渐变不重启。headLocked 继续使用头部坐标的干声分支。
C ABI v1.47 的 `adm_render_options_set_screen_config_json` 和 `adm_create_scene_stream_with_room_ex`
复制同一配置；Scene 未显式指定 reference 时使用标准制作屏幕。屏幕固定到会话，换房／epoch
保留，更换屏幕需新建会话。旧入口和所有公开 POD 大小保持不变。

C ABI v1.46 增加 `adm_room_t` 和独立配置，旧 Scene 嵌套结构大小不变。例如：

```c
adm_room_t *resource = NULL;
if (adm_room_load(context, "room.json", &resource) != ADM_ERROR_OK) return 1;
adm_room_config_t room = {sizeof(room), 0, resource, NULL, 1.5};
adm_scene_stream_config_t scene = {0};
scene.struct_size = sizeof(scene);
scene.rendering.struct_size = sizeof(scene.rendering);
scene.rendering.renderer = ADM_RENDERER_ROOM_BINAURAL;
scene.input_sample_rate = 48000;
scene.output_sample_rate = 48000;
adm_scene_stream_t *stream = NULL;
adm_error_code_t result = adm_create_scene_stream_with_room(context, &scene, &room, &stream);
adm_room_destroy(resource); /* successful creation retained the resources */
if (result != ADM_ERROR_OK) return 1;
```

文件渲染用 `adm_render_options_set_room`；Monitor / Scene 用 `*_switch_room` 提交后台准备请求，
用 `*_room_status_json` 查询 preparing / transitioning / draining / applied / failed。实际受边界
限制的位置用 `*_room_source_json` 查询。JSON 字符串由 `adm_free_string` 释放。重复选择同一资源
不重新淡化。切换保持 HRIR 绑定、坐标比例、晚场方向基和共同算法保护延迟；不兼容包返回失败并保留当前房间。

新预设通过 [房间包工具](../../scripts/room/README.md) 的定义模板生成，不修改 Rust 代码。
`kind` 的新算法需要独立实现和新的数值、生命周期回归；当前只实现混合长方体算法。

## 工作线程和资源所有权

每个会话独立持有历史和动态目标。准备数据用弱缓存共享，缓存不会延长不再使用资源的生命周期。
固定工作线程只计算各声源 / 晚场方向的独立结果，随后按声源 ID、generation 和方向编号顺序累加。
不使用按线程完成顺序的浮点归约，不按负载删反射。独立频率用平面存储与安全 SIMD 分派；
每个频率内的乘加仍分别舍入，不使用 FMA，FFT 保持同一标量算法规划。固定任务缓冲避免工作线程任务提交时分配。
macOS 的惰性互斥量 / 条件变量在构造时对实际的 mutex/condvar 执行零时长等待，
不依赖任务窃取与定时休眠碰巧触发首次等待。Rayon 的私有 worker parking 状态由
[`rayon-core` 本地补丁](../../rust/vendor/rayon-core/PATCHES.md)准备，房间邮箱同样显式准备。
冷启动回归直接检查首次等待和唤醒；房间测试仍从第一个 DSP 块开始检查全部工作线程分配。
后台准备公共 LFE 核使用独立串行计算，避免占据正在播放的房间工作池。

每个内部块只向房间工作池交接一次，与声源数量无关。晚场在块内只做一次方向遍历：
每个世界方向依次渲染当前房间（以及切换中的新房间）的晚场 FIR，按排尾截止合并，
再按头姿分段解码。只有头姿变化的样本才切分解码；声源几何事件不切分晚场，
因为不变的头姿既不更换目标，也不改变子区间的逐样本求和顺序。各方向结果仍在遍历后
按方向编号顺序累加，输出与原来的"晚场 → 合并 → 按每个控制标记解码"三段流程逐位相同。

Room 切换在 FFT / 内存准备后，还会在 worker 中预热新直达分支，再开始 100 ms 过渡；
这段准备时间与交叉淡化时间分别体现在状态中。新房间尚不可闻时可推迟动态滤波目标计算，
但 EQ 与原始输入卷积历史继续推进，并在淡化前恢复完整的 10 ms 目标历史；输出逐位不变。
旧房间的直达淡化结束后停止计算该直达分支，反射与晚场仍按原有限支持完整排尾。
短卷积按路径长度和各 FIR 声明长度确定有效分区，跳过的是数学支持范围以外的空间，
不使用能量门槛、不减少反射阶数。追踪继续沿现有 worker 调度，处理 FIFO 为 256 帧，
现有追踪预渲染上限为 2048 帧；这些数字不是传感器到耳机的端到端测量值。

## 可重复验证

- `tests/unit/room_fixture.py` + `mr_adm_room_tests`：公开接口、资源持有、离线 / 文件流式一致性、
  碎片分块、seek、精确窗口、Scene 的 48 kHz / 44.1 kHz EOS、后台切换及失败回退。
- Rust `tests/room.rs`：距离、边界、节点、接缝、头姿、headLocked、LFE、generation、早期支持、
  尾声、连续事件预热，以及各声源更新点互不影响和 32 个错开斜坡。`room_no_alloc` 同时覆盖
  32 个晚场方向、双房间过渡和各声源错开的斜坡事件。
- Release `room_replay`：准备系数、HRIR 查询、直达 / 早反射分支与动态会话 PCM；同时检查
  固定 / 碎片调用逐位一致。三平台使用同一包和同一事件样本位置比较 `manifest.json`。
- Release `room_bench`：1 / 16 / 32 点声源、连续头姿、双房间与各声源错开的位置斜坡的块耗时、
  P99 和计算期限超时；
  单独运行，不与构建竞争 CPU。`worker_wall_budget_percent` 是墙钟处理预算占比，不是总 CPU 占用。

当前交付状态见文首；测试通过与标准认证、真实设备延迟测量分别报告。


性能工具使用现有默认缓冲策略，头追仍受 2048 帧上限约束。`MR_ADM_ROOM_WORKERS=1..32`
仅供准备阶段的诊断对照，默认最多 8 个线程，不在运行中按负载改音质。运行时资源缓存保持弱引用。双房间期间可复用完全相同的头部相对方向查询；
每声源最多保留 32 条查询结果，命中条件是方向位模式和 FFT 配置相同，失配仍完整计算。
它只减少重复计算，不做角度量化，也不减少反射。过渡时同一声源在两个房间的工作
相邻执行，以复用仍在处理器缓存中的查询数据；完成后仍按原固定声源顺序归约。
`room_stages` 可分别检查 HRIR 查询、源响应合成、目标滤波准备和短卷积的成本。

Windows 的房间 DSP 池和流式房间 worker 使用系统的 Audio MMCSS 任务，离开房间后端时
释放流式 worker 的任务关联；不设置进程优先级或更改全局定时器分辨率。准备诊断中的
`windows_mmcss_workers` 报告实际成功注册的 DSP 线程数。房间输出队列的 1 ms 等待使用
线程独有的高精度 waitable timer，失败时退回普通等待。数值事件仍按样本位置，OS 调度不参与
逐位比较；是否满足实时容量以 Release 实测为准。实现依据为 Microsoft 的
[MMCSS](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service) 与
[waitable timer API](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)。

原生 SOFA 导入的三平台数值和三种渲染流程验证见
[原生 SOFA 验证记录](ROOM_NATIVE_SOFA_VALIDATION.json)。
