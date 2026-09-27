# Renderer 5.5 的 ADM 等尺寸对象：系统识别记录

后续已通过实际调用链定位 OMO 的尺寸增益回调、四路滤波、原对象/固定位置分支及 OAR 消费者，
并用两布局的真实 PCM 验证。入口和字段说明见 [DAR_GAIN_CALL_CHAIN.md](DAR_GAIN_CALL_CHAIN.md)。
下文保留黑盒识别的历史证据。后续独立内核及已接入的等尺寸离线 CLI 见
[DAR_NATIVE_SIZE_ALIGNMENT.md](DAR_NATIVE_SIZE_ALIGNMENT.md)。

对照对象是 Dolby Atmos Renderer 5.5 直接读取同一最终 ADM BWF 后导出的 7.1.4／9.1.6 PCM。所有探针的对象位置和 `width=height=depth=size` 都在最终 AXML 中核对；输入 PCM 逐样本核对并记录哈希。比较保持已冻结的声道映射、零采样时延和单位全局电平，不逐案例对齐或缩放。

## 已确认的行为

- **参考可重复。**同一 4 秒 PRBS ADM 的三次独立 7.1.4 导出逐字节相同。证明位于 `local/dar-size-prbs-long-20260925/repeatability.json`，每次导出都核对了设置恢复。
- **初态与稳定态不同。**孤立脉冲在 size=0.25 前方的四次响应相差不到 0.2%，但其响应预测连续 PRBS 的误差约 15.7%。从两段相同 PRBS 中相减提取稳定态脉冲响应后，预测另一份独立 PRBS，在源开始 0.5 秒后的相对 PCM 误差约 0.016%。稳定响应含短延迟分量，前方案例的延迟部分主要落在两个共享信号模式上。报告分别位于 `local/dar-size-identification-20260925/impulse-report.json` 和 `local/dar-size-warm-impulse-20260925/size_warm_impulses/measured-warm-impulse/warm-impulse-report.json`。
- **持续信号近似线性，且结果确定。**同一 PRBS 分别以 −18／−30 dB 发声，归一化输出的相对差约 0.00049%；移动开始时间 512 采样后约差 0.000024%。两个独立 PRBS 的相加输出与单独输出相加相差约 0.068%，三分之一倍频程能量差最大约 0.003 dB。报告位于 `local/dar-size-transfer-20260925/size_prbs_transfer/measured-transfer/transfer-report.json` 与 `local/dar-size-superposition-20260925/size_prbs_superposition/measured-superposition/superposition-report.json`。
- **对象槽位独立。**两个等尺寸对象分别发声和同时发声时，同时输出与单独输出之和仅差约 0.00027%；单独对象换槽位后输出逐样本相同。报告位于 `local/dar-size-two-object-20260925/size_two_object_superposition/measured-two-object/two-object-report.json`。
- **尺寸路由连续变化。**在固定内部位置的 9.1.6 扫描中，宽声道和顶部中间声道的归一化输入功率之和从 size=0 的 0.487，降为 size=0.01 的 0.481、0.10 的 0.122、0.18 的 0.00029，约到 0.20 为零。因此“任何非零 size 都折叠到 7.1.4”是早期探针范围不足造成的错误结论。原始扫描见 `local/dar-size-route-scan-20260925/size_route_scan/measured-route-scan/motion-report.json`。
- **时间滤波器可共用。**八组训练位置／尺寸的稳定态延迟响应合起来只有四个显著时间模式；仅用它们重建另一组八个位置时，延迟响应的相对误差约 0.06%–0.09%。各非中央扬声器的延迟响应能量与直接增益之比约为 0.420，左／右声道符号相反。独立位置报告位于 `local/dar-size-warm-geometry-20260925/size_warm_bank_geometry/measured-warm-geometry/warm-bank-report.json`。
- **静音收尾有固定窗口。**连续 PRBS 结束后，参考的延迟输出只延续 512 采样。用两组信号拟合的单条、全声道共用收尾曲线，在第三组信号上的衰减曲线误差约 0.015 dB。曲线和留出分数位于 `local/dar-size-prbs-long-20260925/tail-gate-fit.json`。
- **时间模型的上界实验通过。**以另一份 ADM 测得的直接增益为输入，加上四个共用滤波器及静音收尾曲线，研究候选在三组独立长 PRBS 的两种布局上同时通过能量、功率、频谱、相关性和尾部门槛。分数位于 `local/dar-size-prbs-long-20260925/oracle-probe-measured-gate/size-score.json`。这项实验刻意使用测得的直接增益，不能证明自有空间声像器已达标。

## 早期候选状态（保留失败证据）

纯增益残差包含随输入持续时间建立的短时滤波响应，不能用冷态单脉冲或固定纯增益解释。四模式 FFT 滤波与跨处理块的状态管理已有独立内部内核和回归测试；由于空间增益函数尚未通过独立测试，该内核未接入 CLI 的非零尺寸路径。用共享的 XYZ、size、扬声器几何函数拟合空间增益后，训练点最大直接增益误差约 1.1%，24 个新随机交错点的能量占比误差最大约 31%，全部未过 5% 门槛。失败案例位于 `local/dar-size-spatial-validation-20260925/shared-field-validation-score.json`。兼容模式继续对非零尺寸返回 `unsupported`。

旧的 `measure_size_sequence.py` 报告中 `energy` 字段实际为相对输入的 RMS 幅度。新的长 PRBS 测量分别报告纯增益、RMS 幅度、真实声道功率、归一化能量占比、频谱矩阵、输入输出相干性和静音尾部；对照输出在 `local/dar-size-prbs-long-20260925/size_prbs_identification/measured-field2/field-report.json`。
