# Dolby Atmos Renderer 5.5 房间点源兼容实验

本页保留点源阶段及早期尺寸基线。后续已实现的等尺寸内核和当前支持范围见
[DAR_NATIVE_SIZE_ALIGNMENT.md](DAR_NATIVE_SIZE_ALIGNMENT.md)。

目标是让离线 CLI 的 7.1.4／9.1.6 Cartesian Objects 输出靠近 Renderer 5.5 直接读取同一 ADM BWF 的 re-render。当前入口是独立后端 `--renderer triple-balance`；本文中的 room-compat 为早期实验名称。它是实验模式，不是 Dolby Renderer 的通用替代品。

## 已验证的部分

- 声道顺序先用同一 ADM 的带名称 multi-mono 与 interleaved PCM 逐样本核对。9.1.6 顺序相同；7.1.4 的 side/rear 两对槽位需交换。冻结映射在 `local/dar-compat-suite-20260925/channel-map.json`。
- 静态点源采用 XYZ 房间网格。X/Y 各 155 整数步，高度 75 步；层内、行间、层间使用等功率插值。算法位于 `src/adm_render_triple_balance/panner.cpp`，没有逐曲或逐声道补偿表。
- Release 渲染与 Renderer 的 54 对象最终留出集对照：7.1.4 最大带符号增益向量相对 L2 误差 0.0104%，9.1.6 为 0.0205%；两者均无静音点，最大绝对增益误差分别约 0.000098／0.000191，总功率误差小于 0.000002 dB。训练集和交错网格也通过 1%／0.02／0.1 dB 门槛。报告在 `local/dar-compat-bank-20260925/automated-points-summary.json`。
- 运动目标每 512 采样更新，XYZ 用 1200 采样时间常数平滑，块内线性插值扬声器增益。阶跃和密集轨迹的最大包络相对 RMSE 分别不超过 0.0111% 和 0.0055%，主要变化时刻偏差为 0 采样。报告在 `local/dar-compat-motion-20260925/automated-motion-summary.json`。
- 参数冻结后的另一条 6 事件独立运动轨迹包含非控制块对齐事件、阶跃和显式 ramp；7.1.4／9.1.6 的最大事件包络相对 RMSE 分别为 0.1558%／0.1737%，主要变化时刻偏差仍为 0 采样。报告在 `local/dar-compat-motion-holdout-20260925/blind-validation-summary.json`。
- Renderer 5.5 直接 ADM re-render 在本批测试中忽略 `audioBlockFormat/gain`：静态 0、−6、−12 dB 对象的输出增益仍全为 1，变增益事件也没有产生电平变化。兼容模式仿照这个**观察到的**行为；普通 VBAP 仍应用该 gain。静态和运动 Release 复验报告分别在 `local/dar-compat-gain-20260925/automated-gain-summary.json` 和前述运动总结。后续对 `audioObject/gain`、mute、生命周期和 float 平滑边界的更新见 [对象语义对齐](DAR_OBJECT_SEMANTICS.md)。

## 当时尚未通过的尺寸基线

尺寸不能复用点源算法。早期 0.1–0.3 秒测量窗口在 size 0.25／0.5／1 时得到总功率约 0.91／0.85／0.74；后续 4 秒持续信号表明存在短暂预热，前方稳定功率约为 0.933／0.871／0.758。不同位置与尺寸的输出有约 16%–37% 的**纯增益模型残差**，不能仅凭此断言信号被随机去相关。9.1.6 的额外宽声道和顶部中间声道随 size 增大逐渐衰减：在已测内部点，size 0.01 仍有输出，约 0.20 才归零；size 0.25 及以上的已测案例仅用与 7.1.4 相同的 11 个非 LFE 声道。简单 XYZ 扰动或方向锥积分的声道能量误差仍远超 5% 门槛，因此默认尺寸路径返回 `unsupported`。显式 `--speaker-spread-mode none` 可忽略 size/diffuse 并渲染点源，不代表尺寸对齐。

自动化还把关闭 size 的点源输出作为失败基线：训练集的 18 个非零尺寸案例、留出集的 15 个案例，在两种布局下**全部**不满足 5% 能量分布／0.1 dB 总功率门槛。留出集最大能量分布相对误差为 7.1.4 约 99%、9.1.6 约 139%。逐案例误差在 `local/dar-compat-size-seq-20260925/size-point-baseline-summary.json` 指向的 JSON 中。这个数字只衡量“关闭尺寸”的基线，不是对正在开发的尺寸模型的验收分数。

Atmos Conversion Tool 把非零 DAMF size 同时转成 ADM `diffuse=1`。为隔离变量，从最终 ADM 构造了只把 18 个 `diffuse` 字段改成 0 的等长度 BWF，PCM、XYZ、size 和时间不变。Renderer 两种布局的导出分别与原版**逐字节相同**，所有测量向量相同。构造与哈希在 `local/dar-compat-size-isolate-20260925/manifest.json`，能量报告在同目录 `size-energy.json`。因此上述残差不能归因于 ADM diffuse 字段，仍需识别 Renderer 的 size 内核和去相关处理。

这批试验只给 7.1.4／9.1.6 的点源与运动建立数值门槛。尺寸未通过前不扩展 22.2，也不宣称 22.2 数值复刻。历史实验入口现已独立为 Triple Balance 后端；实时接口仍不支持。
等尺寸对象后续的脉冲、长 PRBS、双对象与空间随机点结果见 `docs/architecture/DAR_SIZE_IDENTIFICATION.md`；其中已修正早期“任何非零 size 都折叠到 7.1.4”的判断。

## 重跑

`scripts/research/dar_layouts/run_compat_suite.py` 可从 AC-4 项目的 DAMF writer 生成新 suite，或验证并复用已有同源 ADM 与 Dolby 参考；它只用 Release `mradm` 生成比较音频，逐案例输出测量和阈值报告。具体命令与前置条件见 `scripts/research/dar_layouts/README.md`。目录 `local/` 的 PCM 与报告是研究记录，不纳入源码版本控制。
