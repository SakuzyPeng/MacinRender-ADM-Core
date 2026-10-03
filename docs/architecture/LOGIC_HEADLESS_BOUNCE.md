# Logic Pro 后台 ADM 并轨

2026-09-26；Logic Pro Creator Studio **12.3.1 (6682)**；macOS **27.0 / 26A428**；arm64。

## 能力和边界

已实现 **ADM 路径 → 无窗口临时工程 → Apple Renderer / Music → 双声道 Float32 WAVE → 恢复与清理**。
正常运行不需要点击 Logic 界面、快捷键或并轨/保存面板。另保留已打开工程的直接并轨模式。
音频由 Logic 的真实工程、自动化和 Dolby/Apple 监听链生成，没有替换为外部 SpatialMixer 宿主。
命令及选项见 [脚本说明](../../scripts/research/logic_bounce/README.md)。

仍需要驻留的 Logic 主进程、已初始化的宿主工程和图形登录会话。
没有实现脱离 Logic 进程的独立服务器，也没有验证无人登录或锁屏时的全部运行条件。
只允许固定版本及二进制哈希；工具不修改应用二进制或系统调试权限。

## 完整流程

```text
核对版本/哈希、输入 ADM、新输出路径、磁盘空间，锁定单个客户端
  → 首次用 LLDB 加载优化桥，立即分离；以后使用驻留命令队列
  → 保存原文档状态和渲染器选项
  → makeUntitledDocumentOfType 创建无窗口工程，设置唯一 autosave 名称
  → preSaveNewDocument + ADMImporter::importADMIntoSong
  → 激活音频工程，设置 Apple Renderer / Generic / tracking off / Music
  → 等待 Atmos 更新状态归零且音源/上下文更新标志清空，连续两次确认
  → 按源帧数求并轨 clock，调用原生并轨入口
  → 最多三遍，取得连续两遍完全相同的 PCM SHA-256
  → 恢复原文档、渲染器及并轨偏好，等待原渲染器恢复，删除临时工程
  → 通过全部核对后发布 WAV 和 run.json
```

`--expected-document` 模式不导入工程、不自动修改渲染器，直接继承当前设置并轨；
上述重复 PCM 门槛默认应用于 `--adm` 参考音频流程。
每个请求有过期时间和取消标志，超时未执行的请求不会在很久后开始。
已开始的原生操作不强行打断；`--recover` 只恢复工具自己保留的临时工程/原渲染器。

## 配置

- ADM 中的 7.1.2 床层及对象轨由 Logic 原生导入。
- Atmos Monitoring Format = **Apple Renderer**；**Music**；Generic HRTF；Head Tracking 关闭。
- Output 1-2，环绕声并轨开启；48 kHz、32 位浮点、间插 WAVE。
- 自动模式；正常化、抖动、音频尾音关闭；包含速度信息。

实际写出双声道，符合用户所述继承 Atmos 插件监听选择的行为。
本轮六秒输入的默认项目范围会补齐为八秒，因此 ADM 模式从源帧数反求精确 clock，
不把整数小节长度当成源文件长度。

## 验证

### 与界面正常并轨对照

同一原音乐工程、相同四秒范围，GUI 和无对话框入口均输出 **192,000 帧 × 2 声道 Float32**。
384,000 样本只有 9 个不同，最大绝对差 **8.407790785948902e-45**，差异两侧均为非正规数。
其余样本完全一致；未修改 PCM 来消除这些数值。

对应 `current-gui-4s.wav`、`current-headless-runloop-4s.wav`、`validation-summary.json`。
在连续完整 ADM 任务后，再次并轨原工程四秒与之前驻留桥的结果逐字节相同，
见 `final-state-pcm-comparison.json` 的原工程对照项。

### 独立输入与重复运行

使用 AC4 项目已有 Release CLI 生成同一轴向对象的两份合成探针，输入幅度相差 6 dB。
两者都是 RF64、11 路 24-bit PCM、288,000 帧；dbmd 段 7/9/10 长度和校验通过。

修复就绪检查后，按基线、低 6 dB、基线、低 6 dB 顺序的四次完整运行全部成功：

- 输出均为六秒、双声道、48 kHz、Float32，样本有限且非静音。
- 同一输入的两次输出 PCM 哈希完全相同。
- 最小二乘幅度比例 **0.5011872344667929**，即 **−5.99999998545 dB**；
  目标为 `10^(-6/20)`；拟合残差 RMS **3.3499905426e-08**。
- 创建/导入/并轨阶段的临时工程 window controller 数均为 0。
- 原文档名称、路径、未保存标志、活动音频工程和渲染器选项的恢复检查通过，临时路径不再存在。

数据为 `resident-ready-*.logic-bounce/` 和 `resident-validation-summary.json`。
这是有界探针验证，不能推广为所有插件、任意项目和任意 OS 构建的保证。

### 首遍状态残留与交付门槛

额外切换/恢复测试发现一次首遍异常：`headless-final-base.wav` 相对稳定基线，
开头约 0.25 秒出现衰减状态残留，最大差 **0.0582502603**；0.5 秒后的比较区间相同。
这不是可以忽略的非正规数误差，不能把这一遍交付为干净参考。
该文件保留作失败诊断，见 `final-state-pcm-comparison.json`。

同一临时工程连续三遍测试中，第 2、3 遍 PCM 完全相同；第 1、2 遍另有
11 个极小样本差异（最大约 `3.01e-25`）。因此最终 ADM 流程保留最多三份候选，
必须有连续两遍 **原始 PCM 字节相同** 才发布；若不收敛则失败并保留诊断。
不修改音频、不以宽松阈值掩盖差异。输出与选中的候选使用硬链接，避免重复占用空间。

这是一项实测的参考音频核验措施，不声称已经定位并修复 Logic DSP 历史状态的根本原因。
最终的两档电平以及全部六个语义夹具都通过此门槛；各使用三遍，选择一致的第 2、3 遍。
基线最终 PCM 哈希与早期手工导入、保存探针工程后的重复并轨哈希一致：
`725fde5aff22d7ce0bc3616da903c5953c12e8c31e95ccaf97589edf070b783f`。

### diffuse / divergence / size

六个 matched-PCM Cartesian ADM 夹具已通过完整后台链路。缺省 size 的
control / diffuse=1 / divergence=1 输出相同；size=0.4 的 diffuse=0 / diffuse=1 /
divergence=1 输出也相同，而两组之间不同。源 PCM 和 dbmd 哈希相同，原输入未修改。
详见 [语义调查](LOGIC_ADM_DIFFUSE_DIVERGENCE.md)。

### 错误恢复

过期的创建工程请求返回 cancelled，确认没有留下临时工程。
还主动创建了无窗口、尚未导入的临时工程，通过 `--recover` 恢复并清理，所有恢复核对通过。
该场景揭示原渲染器也可能异步重建，最终恢复流程会等待它完成。
数据在 `resident-recovery-check-final/`。

## 两个不同的异步问题

早期每一步都附加 LLDB，使 Logic 的实时音频线程反复暂停；曾遇到 `MD::Idle` 中的 NSAlert，
并在锁屏时无法从界面确认具体错误。原生栈证明存在错误模态框，但未记录到其精确文本，
不能把错误原因写成已确认的调试器过载。现用一次安装、驻留文件队列避免反复暂停。

另一个已定位的问题是 Atmos 音源更新状态机。导入返回成功时仍可能处于：

```text
sources_pending=1 → update_state=1 → 2 → 3 → 0
```

提前并轨曾生成合法格式、正确帧数但全静音的 WAV。最终桥读取受 mutex 保护的状态，
检查待更新标志与活动工程，并让主循环和音频线程继续工作后再检查；没有用固定延时假设就绪。
这解决了本轮重复导入的静音问题，但不替代上面的 PCM 重复核对。

## 固定 ABI

文件虚拟地址，运行时加已加载框架基址；框架哈希列于 `logic_12_3_1_profile.json`：

- Logic 并轨参数构造 `0xfc76f8`，实例大小 `0x6470`；析构 `0xfc8a8c`。
- Logic 并轨入口 `0xfc8d44`，提供原生文件描述时跳过设置/保存面板。
- 输出通道 `0x2c8a10`；clock→sample `0x7abc8c`；活动音频工程切换 `0x82154c`。
- MAAudioEngine `_sinks` 注册表 `0xf7b040`；参数位置读/写 `0x89e5fc` / `0x89f450`。
- `AUSinkPlugIn::Idle` `0x875e28`；更新状态在实例 `+0x2e0`，mutex `+0x2e8`；
  source/context 更新标志 `+0x328` / `+0x32a`。

CFileRef 由 MAFiles 原始构造/析构管理；bridge 用 `-O2` 编译。
没有使用 Debug MacinRender 输出作为听感或音频对照。

## 本地制品

目录：`local/logic-bounce-20260926/`。

- `original-session-preserved.logicx`：原未保存工程的保留副本，包含其音频，仍保持打开。
- `logic-axes-probe.wav`、`logic-probe-minus6db.wav`：两份输入；`logic-axes-probe.logicx`：早期 GUI 对照工程。
- `verified-reference-base.wav` 及其任务目录：启用重复 PCM 门槛的完整后台参考。
- `semantic-probes/manifest.json`、`semantic-probes/validation.json`：字段夹具及 PCM 实测。
- `semantic-probes/repeat-verified-validation.json`：六个夹具使用最终重复门槛的复核结果。
- 各次失败的静音/首遍残留文件和反汇编保留诊断，不作为通过的参考。

原项目未保存标志保留；恢复检查不是全工程内部数据逐字节比对。
没有修改 MacinRender 的产品渲染路径，也没有创建独立工作树或大型构建缓存。
