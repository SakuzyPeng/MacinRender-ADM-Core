# 外部 SOFA 一致性检查

2026-10-09，扩充现有三平台 Release A/B 与诊断矩阵。原二期和后续覆盖的历史证据保持原样；
本次把固定的外部 SOFA 文件纳入新的检查范围，不将有限样本的通过解释为任意 SOFA 都逐位相同。

## 输入与范围

`phase2_common.SOFA_FIXTURES` 锁定三份项目自有合成文件的 SHA-256，二进制也进入源码/资源指纹。
采集器将文件复制进证据目录；比较器重新核对固定哈希，不接受仅修改 manifest 后重新标记的文件。
CI 不下载数据集，也不依赖 `MR_ADM_TEST_SOFA_PATH` 或 Python NetCDF 包。

| 文件 | 数据 | 覆盖 |
|---|---|---|
| `simple.sofa` | 48 kHz、6 方向、16 tap、球坐标 | SimpleFreeFieldHRIR、未压缩 FIR |
| `general-compressed.sofa` | 44.1 kHz、6 方向、16 tap、笛卡尔坐标 | GeneralFIR、压缩 FIR、实时 HRIR 重采样 |
| `off-axis-compressed.sofa` | 48 kHz、26 方向、64 tap、非轴向笛卡尔坐标 | GeneralFIR、非平凡坐标转换、shuffle/deflate、最后一个不完整数据块 |

第三份文件由 `tests/tools/make_consistency_sofa.py` 生成。坐标与 FIR 使用精确二进制数和整数 LCG，
避免将平台三角函数混入输入生成；HDF5 文件只在维护时生成并提交，CI 使用相同原始字节。
本组 Data.Delay 均为零，未扩展对其他 SOFA convention、距离维或非零延迟的承诺。

- 6 个离线用例：两份 48 kHz SOFA，各覆盖 point、cloud 和 saf-spreader。比较 CLI 与 RenderService
  入口、同进程重复和新进程重复的最终 float32 PCM。
- 12 个 Scene 配置、24 份 PCM：三份 SOFA × point/cloud × fixed/fragmented 分块，各含正常 epoch
  和 seek/preroll 后的 33 帧短尾。三个速率组合分别覆盖原速率 HRIR、44.1→48 / 48→44.1 HRIR
  重采样和输出重采样。继承头追、metadata、generation、policy、EOS 与无欠载检查。
- 新版 v3 回放在样本 8192 从外部 SOFA 切换到内置 KEMAR，第二个 epoch 恢复外部文件。
  v1/v2 的原有脚本参数保持不变；只改 JSON 声明而未执行的热切换会被拒绝。
- 27 份 Rust 内核测量：每份文件记录实际生产解析器的尺寸/采样率、方向、HRIR，再记录生产
  网格权重/索引、频谱以及量化/连续插值查询。各阶段均纳入 `sofa-*` 逐位相等门禁。

SOFA 扩展后的矩阵为 40 个离线用例、70 个 Scene 配置、180 份 PCM、160 份内核测量。
随后加入 SAF VBAP Scene 流式渲染 22.2 的 48/96 kHz、fixed/fragmented 四个配置（各两个 epoch），
当前共 74 个 Scene 配置、188 份 PCM；这 8 个新增 PCM id 同样精确登记为逐位相等门禁。
新增 30 个 PCM id 均精确登记到 `phase2-gates.json`，9 个内核模式覆盖 150 份文件。
原有 10 个观察项仍保留。新增 SOFA 输出还必须为非静音双声道，双耳都为零或缺失一耳不能以相等通过。

## 执行与维护

Consistency 的 macOS arm64、Windows x64、Linux x64 六个 A/B 作业统一启用 SOFA。
采集器拒绝 `SOFA=OFF`；比较器要求 CMake 启用 SOFA，且 Cargo 实际解析的 `mradm-dsp` / `mradm-ffi`
features 都包含 `sofa`。诊断版本必须保持最终 PCM 和全部内核测量不变。

本地复用现有 Debug/Release 构建树与 Cargo 缓存。Release 收集命令与二期相同，配置改为
`-DMR_ADM_ENABLE_SOFA=ON`。缺文件、哈希变化、配置错误或信号无效均直接失败，不跳过用例。

本地 macOS arm64 Release A 已完成 180 份 PCM / 160 份内核测量的重复性与完整性检查，诊断版本
全部逐位不变。相关 Debug CTest 5/5、C++ format/tidy/cppcheck、Rust fmt/Clippy、许可证与冻结参考
校验通过。三平台 A/B 验收结果由推送后的 Consistency 四份比较报告记录。
