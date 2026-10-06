# Rust SAF 子集替换与二期确定性路线

> 2026-10-04：一期实现及三平台验证已完成，采用 Rust DSP 和纯 Rust SOFA 解析。跨平台逐位一致按用户决定留到二期。

## 1. 一期范围

生产路径不再链接 SAF、OpenBLAS/LAPACKE 或 libmysofa。现有 `saf`、`saf-binaural`、`saf-spreader` 参数、backend 标识和 C ABI 枚举值继续兼容。C++ 持有场景、编排、线程池及公开 C ABI；libear、ADM/BW64 和编解码职责不变。一期保留的 C 计量库和 libsamplerate 已分别在后续 [Rust Meter 迁移](RUST_METER_MIGRATION.md)与 [Rust 重采样迁移](RUST_RESAMPLER_MIGRATION.md)中替换。

实现位于同仓库 Cargo workspace：`mradm-dsp` 是禁止 unsafe 的算法库，`mradm-ffi` 负责内部 C 边界。C++ 通过私有 `MacinRender::ADMDsp` target 使用它，不向公开头文件暴露 Rust 或第三方类型。FFI 使用显式长度、浮点数组、状态句柄和调用方错误缓冲；句柄由创建方释放，panic 不穿过 C ABI。C++ FFT 句柄采用 RAII。

| 模块 | 一期实现 |
|---|---|
| FFT | RealFFT 3.5.0 / RustFFT 6.4.1；保留 N/2+1 频谱和逆变换 1/N 缩放，scratch 在准备期分配 |
| VBAP / MDAP | Rust 二维/三维增益、虚拟扬声器、九点 MDAP；不可变布局几何复用，保留现有 ADM extent 映射 |
| HRTF 几何 | Rust 增量凸包、小矩阵求逆、稀疏一度网格及球面 Voronoi 权重；几何扰动使用实例 PCG32 |
| HRTF 准备 | Rust 批量 HRIR FFT；后续[HRTF 迁移](RUST_HRTF_MIGRATION.md)已接管幅度/相位插值、连续方向查询、频谱及共享网格所有权。[双耳 DSP 迁移](RUST_BINAURAL_DSP_MIGRATION.md)已将卷积、滤波过渡和 diffuse 输入混合迁入 Rust |
| afSTFT | 实际使用的 128 hop、512 frame、133 hybrid bands 配置，滤波器组延迟 1536 样本 |
| 去相关 | 两耳格型全通滤波器、频带延迟、能量补偿；种子按音轨身份及 lane 派生，不依赖全局 rand |
| OM spreader | nalgebra 固定 2×2 实数/复数 SVD、正则化、协方差平滑、残差混合和系数插值；仅移植已用 OM 模式 |
| SOFA | sofar 0.3.0 最小本地补丁公开 RawHrtf；关闭其 DSP/重采样 features，由项目验证原始数据 |
| HOA 计量 | 固定三阶 7.1.4 AllRAD/max-rE 矩阵，已适配 SN3D，运行时不再重复转换基底 |

后续项目 DSP 迁移还包括 [HpTF 耳机补偿](RUST_HPTF_MIGRATION.md)：系数设计、频响与自动衰减、
级联状态及热切换由 Rust 接管，参数导入、并发发布和状态查询继续由 C++ 管理。

[输出保护与实时增益迁移](RUST_OUTPUT_DSP_MIGRATION.md)进一步将设备立体声峰值保护、
单通道渐变和多通道增益状态迁入 Rust。六类后端使用批量处理，保留各自的控制及 seek/reset 语义。

[共享 PCM 混音迁移](RUST_PCM_MIX_MIGRATION.md)将通用/EAR 数值增益表、插值游标和工作缓冲，
以及监听固定矩阵迁入 Rust；准备表可共享，渲染实例独立，C++ 保留元数据/布局算法与调度。

[EAR 后处理迁移](RUST_EAR_POST_MIGRATION.md)进一步接管去相关 FIR、补偿延迟和双总线求和，
并修复连续短块丢失未输出尾音的问题。后续 [EAR 算法迁移](RUST_EAR_MIGRATION.md)
已接管布局、增益计算和 FIR 设计，libear 仅用于显式启用的历史对照。

[Triple Balance 完整数值状态迁移](RUST_TRIPLE_BALANCE_MIGRATION.md)接管固定房间几何、
点源/尺寸空间算法、四路去相关、运动与过渡状态及数值快照；C++ 保留语义、I/O、调度和 LRU。

[HOA 编码与计量前处理迁移](RUST_HOA_MIGRATION.md)接管三阶系数、插值、PCM 累加、
diffuse 历史及 LFE 分离/计量解码；C++ 保留语义和调度，并补齐容量检查及异常短读错误处理。

[Monitor DSP 迁移](RUST_MONITOR_DSP_MIGRATION.md)接管后端切换淡化、seek 消点击过渡及
Peak/RMS；worker 与回调分别独占数值实例，C++ 保留队列、设备和控制调度。

[Live VBAP 迁移](RUST_LIVE_VBAP_MIGRATION.md)接管 Live Scene 的声像/电平独立渐变与 PCM 混音，
按整帧提交数值命令，C++ 保留语义与路由并补齐可恢复错误的事务边界。

[Scene 数值迁移](RUST_SCENE_NUMERIC_MIGRATION.md)接管共享空间/姿态数学、Scene 外层过渡和
Live 双耳的字段渐变、cloud 合成及信号状态编排，并补齐双耳整帧错误原子性。

内置 KEMAR、原型和格型系数及 HOA 矩阵是约 1.7 MiB 的已提交二进制资源。`assets/manifest.json` 记录尺寸、来源和 SHA-256；`assets/NOTICE.txt` 保留 ISC/MIT 声明及数据提供者归属。正常构建不从 SAF 提取数据。

## 2. 行为边界

- 一期不复制旧 SAF 位模式，也不以新实现的跨平台位相等作为门禁。RustFFT 使用默认 SIMD，数学函数和 SVD 的跨平台确定性尚未承诺。
- 同平台、同配置、同输入的重复渲染必须一致；分块、窗口、尾音和实时连续性继续满足原有契约。
- 三维共面布局的三角化可以改变，测试约束方向、功率、有效权重及连续性，不再约束旧三角形索引。旧输出与新输出的差异不应全部解释为舍入误差。
- spreader 的随机序列改变，因此波形对照采用能量/协方差和状态验收；不能以低波形相关度直接判断算法错误。
- 保留 512 样本适配缓冲，spreader 总补偿延迟仍为 2048 样本。实时 Scene 原有 cloud 路径和 spreader 使用限制保持不变。
- SOFA 支持 SimpleFreeFieldHRIR、GeneralFIR，要求两耳及现有坐标单位。原始 HRIR 不被归一化或隐式重采样；离线采样率限制保持原样。实时转换随后迁入 Rust，数值边界见重采样迁移记录。
- 滤波器频率按实际采样率计算，不沿用 SAF 在空 STFT handle 下对非 44.1 kHz 输入回退到 48 kHz 频率表的细节。

## 3. 构建与发布

固定 Rust 1.98.0、Corrosion 0.6.1 和 Cargo.lock。Debug 对应 Cargo dev，Release/RelWithDebInfo 对应 release，MinSizeRel 对应 minsizerel。显式设置 PIC，Cargo 产物集中在一个共享根目录；不建立额外工作区。Windows 允许通过 Rust_COMPILER/Rust_CARGO 使用同版本独立 MSVC SDK。

生产构建始终使用 Rust。`MR_ADM_BUILD_SAF_REFERENCE_TESTS=ON` 仅获取、构建历史 SAF 数学/信号参考工具，不改变 renderer；默认 OFF。它用于维护和比较，禁止作为运行时静默回退。`MR_ADM_ENABLE_RUST` 是旧设计中的未实现开关，当前不使用。

共享 C ABI bundle 在 Mach-O/ELF 上只导出 `adm_*`，避免 Rust 分配器、panic 入口和私有 DSP 符号被其他模块绑定。Windows 继续显式编译 C API 源文件供自动导出扫描。所有跨边界内存由原分配方释放。

Cargo.lock 全部第三方包（含平台/feature/构建依赖）登记到现有 manifest/SBOM/许可证包；实际编入范围由 Cargo 决定。CMake 输出 rust-dependencies.json 供源码和许可校验，数值构建记录包括 Cargo.lock 哈希、工具链配置和 FFT 分派策略。

## 4. 验收与复现

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-licenses.sh --build-dir build/debug

# 历史数值/信号参考只使用 Release。
cmake --preset release -DMR_ADM_BUILD_SAF_REFERENCE_TESTS=ON
cmake --build --preset release --target mr_adm_saf_reference_tests
build/release/mr_adm_saf_reference_tests
```

Rust 单元测试覆盖独立 double DFT、实际 FFT 长度、凸包/Voronoi、增益和网格、矩阵残差与退化输入、滤波器重构及固定延迟、SOFA 原始值、重复初始化和错误边界。独立分配器测试约束准备后的 FFT、运动控制更新和 spreader 处理不分配内存。

原生回归覆盖现有全部 renderer、C ABI、实时/窗口/分块语义及新 SOFA fixture。SOFA ON/OFF 都需验证；Windows 使用原 canonical MSVC/Ninja 构建目录。

本轮 macOS arm64 已记录：

- 52 项 Debug CTest 全部通过，包括 Rust 单元/分配检查。
- Release SAF 对照 FFT 相对误差约 1.1e-7～2.3e-7；规则布局 VBAP/MDAP 对照通过；内置 KEMAR 样本逐位保留。
- OM 稳态噪声能量差约 0.019 dB；16 个 Release ADM 用例有效；单/多轨同进程重复输出一致。
- 五轮交替执行的小型 fixture 中位耗时比（Rust/SAF）：ear-extent 1.059, vbap-point 1.037, binaural-point 0.560, binaural-cloud 0.577, binaural-spreader 0.241；对应 RSS 比值 1.023, 1.010, 1.035, 1.029, 0.536。

这些是包含初始化的小型合成 fixture 数据，不代表长节目吞吐或主观听感结论。Windows canonical Release 在 SOFA ON/OFF 下各通过 51 项测试；Linux CI 通过 51 项，macOS/Windows Debug CI 同样通过。两台原生验证主机均保留全部 139 个公开 C ABI 入口，Rust 私有符号不外泄。

[完整 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37194085996)验证代码提交 `6ae10d5`；[机器可读验收记录](evidence/rust-saf-phase1/validation.json)、[五轮性能明细](evidence/rust-saf-phase1/macos-release-performance.json)和[音频差异记录](evidence/rust-saf-phase1/macos-release-audio-comparison.json)保留测量范围。实时双耳/VBAP 各运行 10 秒均无欠载，worker p99 分别约 181/39 微秒，块预算为 10 毫秒。

重复全量验证还复现了原 SceneStream 空闲关闭丢唤醒：析构更新 quit 时未持有 queue_mutex。现已在同一锁内发布退出谓词，并增加并发反复创建/销毁及超时回归。

## 5. 二期：跨平台逐位一致

首批基线、实时回放和 Rust 分歧定位工具见 [Rust 二期基线](RUST_PHASE2_BASELINE.md)。

目标仍是相同版本、输入与参数在 macOS arm64、Windows x64、Linux x64 上产生相同最终 float32 PCM，但以下项目不阻塞一期：

- 建立确定性 FFT/数学参考路径，控制 SIMD/FMA、三角函数及系数生成。
- 明确 SVD 简并子空间、排序与停止条件；固定 worker 分组及累加拓扑。
- 覆盖保留的 libear 系数、增益、Rust 重采样和计量/归一化反馈链路。
- 按 renderer/布局/语义/后处理组合恢复位相等门禁。

一致性 CI 继续保存 A/B C++ 数值控制下的 PCM 和构建记录，使用一期专用的空跨平台门禁清单；同进程重复性仍是硬门禁。历史 SAF 清单和实验不改写：见 [原始定位](CONSISTENCY_LOCALIZATION.md)、[3D VBAP 定位](CONSISTENCY_VBAP_LOCALIZATION.md)。旧 run-localization.py 只适用于那些记录的 SAF 源码版本，会拒绝对当前 Rust 构建执行旧归因实验。

后续整数 BW64/WAVE I/O 迁移见 [Rust BW64 迁移](RUST_BW64_MIGRATION.md)，浮点 WAVE（移除 dr_wav）见 [Rust 浮点 WAVE 迁移](RUST_DR_WAV_MIGRATION.md)，`bext`/`ambi` 追加、布局重写、ADM chunk 读取与 export 写回见 [Rust WAVE 容器元数据迁移](RUST_WAV_CONTAINER_MIGRATION.md)；临时文件与替换编排仍在 C++。

## 参考实现退出

- 单元 `saf`（`tests/reference/saf_reference_test.cpp`、`tests/reference/spreader_mr.c`、`tests/reference/spreader_mr.h`、`tests/reference/spreader_mr_internal.h`）：在默认 OFF 的 `MR_ADM_BUILD_SAF_REFERENCE_TESTS` 下编译，由 `mr_adm_saf_reference_tests` 比较。
  同一开关还挂着 SAF 探针工具（`mr_adm_numeric_probe` 等）和 `MR_ADM_DIAGNOSTIC_PORTABLE_RNG`，退役时一并处理；历史一致性定位脚本只适用于原 SAF 版本。

2026-10-06 修复参考测试在新版 Xcode 上的头文件兼容性：SAF 的 `saf_hrir.h` 在 `extern "C"` 内包含 `<complex>`，测试入口现显式提前包含该标准头。只调整包含依赖，参考算法、输入和误差阈值均不变；同步更新登记表中测试文件的 SHA-256。

按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)的通用条件退役；登记与文件哈希见 [`tests/reference/retention.json`](../../tests/reference/retention.json)。当前状态：保留（Rust 实现尚未随正式 tag 发布，独立回归与二期需求待评审）。
