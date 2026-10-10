# Rust 二期：基线与分歧定位

> 二期已按锁定矩阵结项，当前范围和维护规则见[结项记录](RUST_PHASE2_CLOSEOUT.md)。
> 本文保留本切片当时的测量、门禁状态和阶段边界。

本批只建立当前实现的 Release 数值证据，不改变 DSP 算法，不新增跨平台位相等承诺。
Scene 继续采用 `scene-separate-v1`。dr_flac、历史参考和冻结记录保持原有边界。

## 覆盖与复现

同一源码指纹下比较 macOS arm64、Windows x64、Linux x64。配置 A 为默认数值行为；
配置 B 只对 C/C++ 开启严格浮点控制。两组 RustFFT 均保留自动分派，不能把 B 当作确定性参考。
统一使用 vendored FLAC/Opus，IAMF/SOFA OFF，内置 KEMAR，固定 Rust 1.98.0 与 Cargo.lock。

`phase2_common.py` 的版本化清单包含：

- 34 个离线场景：原 16 项矩阵、17 项 Scene 算术对照按输入和参数去重，再增加 −23 LUFS
  响度归一化和 −6 dBTP 峰值归一化。覆盖 EAR、VBAP、Triple Balance、HOA、双耳 point/cloud/spreader，
  DirectSpeakers/HOA 输入、多轨、较高声道布局、窗口及 listener 旋转。
- 22 个 Scene 配置：VBAP 立体声和 7.1.4、双耳 point/cloud，48→48、48→44.1、44.1→48 kHz；
  多声道 VBAP 采用同速率，转换另由立体声 VBAP 覆盖。设备 DSP 的两个配置覆盖音量、HpTF
  启用/热切换/旁路与峰值保护。每个配置分别输出正常 epoch 与 seek/preroll 后 33 个输入样本的短尾。
- 固定 512 帧和 `[1,7,127,511,1024]` 分块。位置、增益、headLocked 更新由整数样本偏移定义；
  队列边界执行 pose、generation、policy 和双声道 backend 切换。不同分块之间的位差只记录。
- 49 个 Rust 内核测量：FFT 256/512/1024/2048/4096、Scene 几何、EAR FIR、实/复 OM 协方差混合、
  重采样与 HpTF 系数。每个内核输入与输出均按原始位模式保存。

离线和 Scene 合计 78 份 PCM。每项检查同进程和新进程重复性；离线额外核对 CLI 与
RenderService 回放入口。输入使用整数 LCG 和精确二进制缩放，不以平台三角函数生成输入。

本地复用现有 Release 构建树，Windows 复用 canonical MSVC/Ninja 树。不要在空间紧张的机器上
另外建立 consistency-a/b checkout 或依赖缓存。示例（原配置须在完成后恢复）：

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DMR_ADM_CORE_USE_INSTALLED_DEPS=OFF -DMR_ADM_FLAC_PROVIDER=VENDORED \
  -DMR_ADM_OPUS_PROVIDER=VENDORED -DMR_ADM_ENABLE_IAMF=OFF -DMR_ADM_ENABLE_SOFA=OFF \
  -DMR_ADM_STRICT_FP=OFF -DMR_ADM_CONSISTENCY_DIAGNOSTICS=OFF
cmake --build build/release --target mr_adm_phase2_tools
python3 scripts/consistency/run-rust-phase2.py build/release local/phase2-a --config a
cmake -S . -B build/release -DMR_ADM_CONSISTENCY_DIAGNOSTICS=ON
cmake --build build/release --target mr_adm_phase2_tools
python3 scripts/consistency/run-rust-phase2.py build/release local/phase2-a-diagnostic \
  --config a --baseline local/phase2-a
```

B 组在同一构建树顺序设置 `MR_ADM_STRICT_FP=ON`，用独立输出目录及 `--config b` 重复上述步骤。
收集器拒绝 Debug、陈旧构建指纹、混合输出目录或不匹配的配置。构建戳同时绑定 CMake cache、
编译命令和依赖记录；仅重新 configure 后必须重新构建工具，旧版缺少配置指纹的构建戳也需重建。
诊断采集在开始前检查 `--baseline`，要求其清单和构建记录均明确关闭诊断。生产默认仍关闭诊断。

## 实时与检查点边界

回放工具驱动生产 SceneStream 和 SceneOutputSession，通过已有设备注入接口手动拉取 PCM。
私有时钟每个 epoch 从 `epoch*2s` 起步，按输入样本时间推进，在样本 10240 额外前进一秒，
覆盖头追踪 750 ms 活跃窗口的失效。时钟保持单调；生产默认时钟和分块策略不变。
v1 回放使用固定的信号生成器、epoch 和控制脚本；修改这些描述字段会在写入事件或 PCM 前失败，
不能通过仅修改清单声明另一次未执行的实验。metadata、partition 和 renderer 参数仍由清单驱动。

私有 worker fence 确认已提交输入与控制完成；它保留输出背压，输出环满时允许超时。
回放先消费可用媒体，再等待 fence。结束标记仅代表开始 draining，必须等待实际 production_complete
才能读取末尾和 EOS。意外欠载、短读、失败、超时或有理帧数错误均使采集失败。
硬件补零不进入 PCM 比较，也不能通过丢弃补零掩盖一次无效回放。

诊断构建包含当前 C++→Rust 边界的有效语义、增益、HRTF、混音、spreader 输入/输出、
重采样、Scene 过渡、HpTF、峰值保护输出及 WAVE 写入前 PCM。
Scene 回放另记录策略前后的完整状态、三种 Scene 渲染器交给 Rust 的命令与系数、后端切换混合前的两路输出
和 HpTF 系数，清单与命名见 [ADR 0018](../adr/0018-scene-numeric-replay-rules.md)；收集器按后端检查这些文件是否齐全。实时帧始终声明已解析的状态完整，只有 generation 首帧携带初始快照；四个活跃时间窗必须有信号。
设备回调的分段按媒体偏移重组为每个 epoch/阶段的连续 PCM，缺口、重叠及总长度不符均失败。
只采集首轮回放，避免第二轮不同回调分段混入；合并保留每个样本的原始位模式。
键来自 epoch、generation、
媒体位置、group/lane；不以 worker 到达顺序编号。WAVE writer 顺序仅用于单进程 CLI 的串行写入阶段。
Rust `diagnostics` feature 仅在显式内核 scope 中启用细节检查点，包括 EAR 随机相位角、FFT 前后
频谱及 SVD 奇异值；普通处理没有 scope，不新增运行时 DSP 后端。

所有诊断输出都要与同源码、同配置的非诊断 PCM 和内核结果逐位相同。多轨 spreader 分别改变
worker 数量和分组预算，保存真实生效的 pool/group 信息，避免把请求值当成实际配置。

## 证据与解释规则

构建 target 在全部测量工具构建完成后记录源码/资源指纹与二进制哈希。记录实际 rustc/LLVM、target、
Cargo features、编译参数及 CPU 能力；RustFFT 不公开实际选中的 backend，记录为 unknown，不能从 CPU 能力推断。

比较器先验证版本化清单、输入、事件、产物哈希、Release 配置、重复性和 Scene 健康记录，
再比较 float32/float64 位模式。保留符号零与 subnormal，拒绝 NaN/Inf、截断及无效尺寸。
报告每个用例的首个 PCM 差异、位模式、ULP、最大绝对误差与首个观测分歧检查点。

检查点按已知计算阶段排序；未采样的内部细节和分支拓扑变化明确保留为未决。
“最早观测分歧”不等于根因：只有相同内核输入产生不同输出，或单变量实验验证了影响，才能
确认该边界。FFT、数学函数、系数生成、归约顺序等后续替换必须依据本批证据独立评审。

Consistency CI 保留三平台 A/B，各自顺序构建诊断版本。输入完整性、重复性、诊断无扰动性及帧数是硬门禁；
跨平台数值差异仍是测量。没有自动填充任何跨平台位相等清单，也没有退役历史参考。
完整 PCM/checkpoints 存 CI artifacts，仓库只保存验收摘要、指纹和必要的最小复现。

不覆盖 Apple 系统渲染器、物理设备或系统混音、外部 SOFA、独立 Monitor API 和编码文件字节一致性。
这些测量不用于吞吐、RSS、实时延迟或主观听感结论。

## 本轮验收

[三平台 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37530745999) 在
`00a70e4` 上完成六组 Release 采集及全部汇总。源码/资源指纹、测试范围和结果见
[验收记录](evidence/rust-phase2/validation.json)，逐用例首次观测点见
[A 组](evidence/rust-phase2/comparison-a.json)及 [B 组](evidence/rust-phase2/comparison-b.json)。

| 比较范围 | 逐位相同 | 有差异 |
|---|---:|---:|
| A：三平台共同结果 | 29/78 | 49/78 |
| B：三平台共同结果 | 29/78 | 49/78 |
| 每个平台内部 A 对 B | 78/78 | 0/78 |
| Linux x64 对 Windows x64（A、B 均相同） | 52/78 | 26/78 |

所有六组均通过输入/事件完整性、同进程与新进程重复性、有效信号、帧数、无欠载及诊断无扰动门禁。
本机 Debug 全套 70/70；最终回放修正后相关 6/6 和工具测试 16/16；Rust fmt/Clippy（含全部 features）、
C++ 质量、许可证与冻结参考校验通过。公开 C ABI 保持 139 个导出，私有 FFI 209 个函数/65 个结构体一致。

可以确认的分歧边界：

- **FFT**：五种长度的输入相同，ARM64 对 x64 的正变换频谱和逆变换输出均有位差；本组
  Linux/Windows x64 FFT 探针相同。48 kHz 双耳用例可在原始 HRIR 相同、HRTF 频谱不同处观测分歧。
  这证明当前 FFT 路径需要收敛，未据 CPU 能力推断具体选中了哪个 backend。
- **重采样**：48→44.1 和 44.1→48 kHz 的相同输入在所有平台对上产生位差，包含 Linux/Windows x64。
  同速率旁路一致。非 48 kHz 双耳还可能在重采样后的 HRIR 即出现分歧。
- **EAR**：固定随机相位角相同，部分 double 频谱/逆变换中间值不同，但采样到的最终 float32 FIR
  全部相同。四个 EAR extent/cartesian/window 用例仍有最终 PCM 位差，现有前级采样尚未分开
  混音与后处理的全部内部贡献；不能直接把中间 double 差异当成最终 PCM 的原因。
- **其余探针**：本组 Scene 数学、OM/SVD 与 HpTF 独立输出相同；不扩展为模块的全输入域保证。
- **分组实验**：本机固定三组时 1/2 worker 逐位相同；将三组并为一组会改变 PCM，最大绝对差
  `4.76837158203125e-7`。不同输入分块的 Scene 对照也存在差异，另列为状态/分块行为研究，
  不把它们自动归因于跨平台浮点误差。

基线提出的后续顺序现已完成：[FFT 收敛](RUST_PHASE2_FFT.md)同时解决了所测 EAR 后处理分歧，
[重采样收敛](RUST_PHASE2_RESAMPLER.md)使结果达到 76/78，[OM spreader 收敛](RUST_PHASE2_SPREADER.md)
补齐到 78/78，随后完成[保持位模式的性能回收](RUST_PHASE2_PERFORMANCE.md)。门禁由最初的测量清单
逐步扩大到全部 78 个精确 PCM id；本页的 29/78 是原始基线。未覆盖边界和 dr_flac 等后续任务见结项记录。

两轮早期 CI 及其 Scene 数据不作为最终验收：第一轮发现 Windows 行尾影响锁文件原始哈希；
第二轮暴露回调分段和混合重复采样的问题。回放状态完整标志、有效信号断言及设备检查点重组
均已在最终提交中修正，未改写生产 DSP 算法。
