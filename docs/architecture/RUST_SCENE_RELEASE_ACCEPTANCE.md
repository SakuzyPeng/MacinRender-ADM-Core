# Scene 流式渲染三平台验收（一期 1.6）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)一期 1.6，规则见 [ADR 0018](../adr/0018-scene-numeric-replay-rules.md)。
> 前序记录：[1.2](RUST_SCENE_RESIDUAL_MIGRATION.md)、[1.3](RUST_SCENE_PORTABLE_MATH.md)、[1.4](RUST_SCENE_OUTPUT_DSP.md)、[1.5](RUST_SCENE_TIMING.md)。

## 范围

1.6 不改变数值实现，只补齐验收条件：

- 一致性矩阵改用发布构建的配置，使“三平台相同”指向实际发布的二进制；
- 在设备侧 DSP（HpTF 切换、峰值保护、音量）前加上变采样，并分别接外部 SOFA 和 VBAP 立体声馈送；
- 为设备回调加 C++ 分配计数测试，核对回调路径的锁与 I/O；
- 核对公开 C ABI。

| 提交 | 内容 | Linux x64 config A |
| --- | --- | --- |
| `ad4ef3c` | 设备回调 C++ 分配测试；Rust HpTF 分配测试加入排队目标 | 无输出变化 |
| `eedf2fe` | config A 采用发布配置，构建并校验 C ABI bundle；Windows 发布构建不再使用已安装依赖 | 原有 431 个普通文件逐位不变 |
| `d85bc79` | 新增 4 个设备 DSP 回放配置（8 个 PCM id），全部设为门禁 | 新增 12 个文件，其余不变 |
| `0512770` | 分配测试也统计对齐分配与 nothrow 形式，并先自检计数器 | 无输出变化 |

## 发布配置

1.5 之前，Consistency 用 `consistency-a` 构建，发布用 `release` preset 加一组命令行选项，两者在两处不同：

- 发布打开 `MR_ADM_BUILD_CAPI_BUNDLE`。它让全部目标以位置无关代码编译（Linux 上为 `-fPIC`），
  Rust 也用 `-Crelocation-model=pic`。位置无关代码会改变寄存器分配和指令选择，理论上可能改变浮点结果；
- macOS/Linux 发布关闭了已安装依赖的查找（`MR_ADM_CORE_USE_INSTALLED_DEPS=OFF` 及 fmt/spdlog/CLI11），
  Windows 发布没有，依赖来源取决于 runner 镜像。

现在：

- `consistency-a` 与发布构建使用同一组选项，Unix 与 Windows 的采集都构建 `mradm_capi_bundle` 并运行
  `check-capi-exports.py`；
- Windows 发布构建加上 `-DMR_ADM_CORE_USE_INSTALLED_DEPS=OFF`，与另两个平台一致；
- `consistency_build_test.py` 的 `ReleaseParityTests` 从 `release.yml` 与 `consistency.yml` 的 Configure 步骤
  解析 `-D` 选项，与展开后的 preset 和 CMake 默认值比较：发布的每个选项都必须以相同值进入 config A；
  只在一致性构建出现的选项必须等于 CMake 默认值（`EAR_SIMD` 例外，它不影响 Scene）。
  两侧任何一处改动都会让测试失败。

在 Linux x64 上，发布配置采集的原有 188 个 PCM id 与 1.5 的非 PIC 采集逐位相同（431 个文件，0 个差异）。
config B 仍只在 config A 之上打开严格浮点选项。

## 覆盖

锁定矩阵现有 78 个 Scene 回放配置，每个配置 2 个 epoch，加上 40 个离线用例，共 196 个 PCM id，全部设为三平台门禁。
1.6 要求的各项对应如下：

| 要求 | 回放配置 | 说明 |
| --- | --- | --- |
| 内置 HRTF | 双耳 26 个 | 48/44.1/96 kHz，含 cloud 输入、元数据更新与后端切换 |
| 固定外部 SOFA | 14 个 | 仓库内 3 份 SOFA（SHA-256 校验），第 8192 帧切换到内置 KEMAR |
| 变采样 | 38 个 | 44.1↔48、48↔96 kHz；双耳、VBAP、Triple Balance 与设备 DSP 均有 |
| HpTF 切换 | 6 个 | 旁路 → 峰值 EQ（自动衰减）→ 高架 → 旁路，每次相隔 2048 帧 |
| 峰值保护与音量 | 6 个 | 信号幅度 4.0 使保护持续工作；音量 0.875 → 0.5 |
| 多声道输出 | 24 个 | VBAP `4+7+0`、22.2（`9+10+3`），Triple Balance 7.1.4、9.1.6、22.2 |

1.5 之前，设备 DSP 只在内置 KEMAR、48 kHz 输入输出下回放。`d85bc79` 由 `scene-device-dsp-fixed` 派生出 version 4 配置，
控制与输出事件不变，分块各有固定和碎片两种：

- `scene-device-dsp-sofa-simple-48000-44100`：`simple.sofa`，48 → 44.1 kHz，第 8192 帧切换到内置 KEMAR；
- `scene-device-dsp-vbap-44100-48000`：VBAP 立体声，44.1 → 48 kHz，第 8192 帧切换到双耳。

这样 HpTF 与峰值保护的输入来自分数比率重采样之后，并覆盖外部 SOFA 和扬声器后端的立体声馈送。
回放器只要求 SOFA 配置使用双耳后端；旧 version 1–3 配置的冻结摘要不变。

## 准备后的处理：分配、锁与 I/O

**C++ 分配。** `mr_adm_scene_callback_alloc_tests` 替换全局 `operator new`/`delete`（含对齐与 nothrow 形式），
只在 `CaptureDevice::pull` 调用期间计数，测试线程的准备工作不计入。每个会话的流程：

- 播放 6 个回调；
- 在立体声保护路径上发布一个 HpTF 目标，再在淡化期间发布第二个使其排队，并调整音量；
- 耗尽输入，至少触发一次欠载（否则测试失败）；
- 恢复提交，直到 EOS。

覆盖三种会话：双耳 48/48 kHz 带保护、VBAP 立体声 44.1 → 48 kHz 带保护、VBAP `4+7+0` 无保护（音量在 Rust 中相乘）。
三种会话的回调分配均为 0。计数器先自检普通、数组、对齐、带尺寸释放和 nothrow 各形式。
诊断构建在回调中写检查点，测试在该配置下跳过。

**Rust 分配。** Rust 通过 `malloc` 分配，不经过 C++ `operator new`，由 `mradm-dsp` 的 `realtime_allocations.rs` 约束。
HpTF 用例在 `ad4ef3c` 起也覆盖排队目标和 reset 的两种形式。

**锁与 I/O。** 回调路径为 `SceneOutputSession::pull` / `pull_stereo` → `SceneStreamEngine::pull` → 环形缓冲与 Rust DSP：

- 只使用原子量，没有互斥锁、条件变量、日志或文件 I/O；`stream_->status()` 同样只读原子量；
- HpTF 目标经无锁信箱交换，`HptfProcessor` 中的互斥锁只在控制线程使用；
- 峰值保护输入缓冲在会话创建时分配；
- 尚未缓冲到启动水位时，`pull` 会读取时钟来判断头追有效期。默认是 `steady_clock`；
  调用方注入的 `config.clock` 也会在回调线程执行，因此它必须实时安全（回放器的虚拟时钟只读原子量）；
- `SceneOutputSession::status()` 会加锁，但不在回调路径上；
- 诊断构建在回调中写检查点文件，这只用于一致性测量。

`adm_scene_stream_pull` 只拷贝结果。miniaudio 在回调返回后执行自身的限幅和设备格式转换，这在一期边界（应用侧 PCM）之外。

## 公开 ABI

一期各切片都没有修改 `include/`。C ABI bundle 恰好导出 157 个 `adm_*` 函数，与 `c_api.h` 一致，无私有 `mradm_*` 符号。
较早文档中的 139 个，增加部分全部来自 ADR 0019 房间渲染新增的 API。
`check-capi-exports.py` 现在除 ci/release 外，也在每个平台的一致性采集中运行。

## Scene 链路中剩余的 C++ 运算

审计要求一期结项前登记 C++ 中仍直接决定 Scene 样本值的运算。逐文件核对后：

| 类别 | 位置 | 结论 |
| --- | --- | --- |
| 拷贝、补零、单位增益路由 | `SceneStreamEngine` 去交织、切片拷贝、环形缓冲；三种 live renderer 的状态打包 | 不舍入 |
| 整数与有理数帧计数 | 策略渐变长度、preroll、时间线累加、EOS 余数、短尾 | 不改变样本值；规则见 1.5 |
| LFE 功率分配 | `live_vbap_renderer.cpp`：`gain * k_lfe_split_power_gain`，`gain` 为常量 1 | 乘积精确等于常量 |
| 语义策略 JSON 数值 | nlohmann 解析数字（C 运行时 `strtod`），再窄化为 `float` | 审计项 N01；依赖运行时正确舍入，回放矩阵三平台相同 |
| 头追有效期与切片上限 | `head_tracking_recent` 按时钟选择 512 或 1024 帧 | 回放器使用虚拟时钟；墙钟会话不在逐位承诺内（1.5） |

`direct_speakers_matrix` 的 `sqrt` 不在 Scene 路径上（离线 EAR 使用）。诊断构建中的检查点运算不影响普通构建输出。

## 三平台验收

`0512770` 的 Consistency 38677291567、CI 38677293973、Quality 38677296369 与 Release Build 38677316398 正在运行，结果待补。

本机 Linux x64 config A：发布配置的普通采集与 1.5 相比，原有 431 个文件逐位相同、新增 12 个；
诊断采集与 1.5 相比，原有 23946 个文件逐位相同、新增 1049 个，且与普通采集无扰动。

## 一期结项

待三平台结果确认后填写。
