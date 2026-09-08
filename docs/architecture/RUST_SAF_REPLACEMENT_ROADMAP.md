# Rust 落地与 SAF 替换路线图

> 状态：阶段 0 测量基建与 A/B 受控构建已落地（fixture、PCM 工具、构建记录、渲染矩阵和三平台 CI workflow）；首轮三平台基线已于 2026-09-08 完成：默认 A 组 5/12、受控 B 组 6/12 个 case 逐位一致。Rust 模块与 `MR_ADM_ENABLE_RUST` 仍未实现。
>
> 本文落实 [ADR 0008](../adr/0008-rust-entry-and-saf-replacement.md) 的模块边界与确定性契约，沿用 ADR 0002 / 0003 / 0004 / 0005 / 0007 的语言、依赖、错误处理和 ABI 约束。

## 1. 目标与范围

目标是在 macOS arm64、Windows x64、Linux x64 上，对相同输入、参数和实现版本产生逐位一致的最终 float32 PCM，并逐步减少原生数学库依赖。首批替换重点是 SAF 数学模块，数值审计覆盖完整调用链。

本路线保留 `libear` 的 BS.2127 算法实现及 `libadm` / `libbw64` 的 IO 职责，不复制完整场景模型，不改变 GUI 的公开 C ABI。保留的模块若仍影响 PCM，必须统一其数值实现或继续列为阻塞项。HRTF / afSTFT / SOFA 不预设为全量重写目标，也不能整体标作“无需验证”。

验收按 renderer × 布局 × 语义 / 后处理组合记录。软件渲染的承诺不覆盖 Apple AUSpatialMixer、设备混音或系统编码器；整数量化和编码文件另行验证。

## 2. 现状：从调用点追踪到实际计算

下表是源码审计结果，不是三平台输出差异的实测表。函数封装数量和源码行数不足以估算替换工作量，必须继续追踪所选模式的内部调用。

| 区域 | 项目入口或传递调用 | 需要处理的数值边界 | 计划 |
|---|---|---|---|
| EAR FFT | `saf_rfft_{create,forward,backward,destroy}` | 当前默认 vDSP / KissFFT 分支，FFT 系数与缩放 | 阶段 1 |
| libear | 增益计算、`designDecorrelators()` | 平台数学函数、内部 KissFFT、Eigen / xsimd、FIR 系数 | 阶段 0 盘点，阶段 1 同步处理 EAR 所需路径 |
| 项目共用数学 | 场景坐标、头部旋转、extent、增益与 HOA 编码 | `sin` / `cos` / `atan2` / `tan` / `pow` 等及运算顺序 | 随使用它的目标路径处理 |
| spreader 默认 OM | `formulate_M_and_Cr_cmplx()` / `formulate_M_and_Cr()` → `saf_cdf4sap` | `utility_csvd` / `utility_ssvd` 与内部 GEMM | 阶段 2 主路径 |
| spreader EVD | `utility_cseig()` | LAPACK EVD、特征值顺序、相位和简并子空间 | 阶段 2 按实际暴露的模式处理 |
| 格型去相关器 | `latticeDecorrelator_*` → `getDecorrelationDelays()` / `randperm()` | C `rand()`、全局调用历史、延迟与滤波器状态 | 阶段 2 必须处理 |
| spreader 向量 / 矩阵 | 项目直接 `cblas_*` / `utility_*` 及其内部调用 | 累加顺序、复数布局、舍入 | 阶段 2 |
| HRIR / afSTFT | `HRIRs2HRTFs()` / `afSTFT_*` | 内部 FFT、系数准备、矩阵及历史状态 | 双耳端到端验收前必须处理；可保留外壳 |
| SOFA | `saf_sofa_open/close()` 与后续 HRTF 准备 | 同一文件解出的方向、样本和元数据，后续重采样 / 变换 | 双耳验收覆盖，解析器全量替换另议 |
| VBAP | `generateVBAPgainTable*`、凸包及几何辅助 | BLAS、三角函数、排序与退化几何处理 | 阶段 3 |
| HOA 解码 | `getLoudspeakerDecoderMtx()` 及内部 SH / AllRAD | 解码矩阵、归一化与用于计量的信号 | 阶段 3 |
| 计量 / 后处理 | `libebur128`、响度 / True Peak / 最终增益 | 测量反馈到 PCM 的增益，平台数学与阈值判断 | 每条最终输出路径的验收范围 |
| 重采样 | `libsamplerate` 与使用它的 HRTF / 实时路径 | 系数、精度、状态和调用分块 | 启用该功能的验收范围 |

`BinauralSpreaderAdapter` 当前显式设为 `SPREADER_MODE_OM`。该模式内部有实数、复数 SVD；项目中的 `utility_cseig()` 属于另一条 EVD 分支。替换直接看到的 `cblas_*` 和 `cheev` 不足以消除默认 spreader 的 BLAS/LAPACK 依赖。

`spreader_mr.c` 是项目维护的 SAF fork。修改它不要求先改上游，但仍需保留许可证、记录相对上游的改动，并评估后续同步成本。

## 3. Workspace 布局

继续使用当前仓库。以下是建议布局，首次只建立数学模块及其内部 FFI 边界，后续按实际边界增加 crate：

```text
rust/
├── Cargo.toml              # workspace 与 profiles
├── Cargo.lock             # 锁定依赖
├── rust-toolchain.toml     # 锁定工具链
└── crates/
    ├── mradm-math/         # 首批数学内核，普通 Rust 库
    ├── mradm-ffi/          # 内部 staticlib，按功能聚合 C 接口
    ├── mradm-vbap/         # 阶段 3，按需要拆分
    └── mradm-hoa/          # 阶段 3，按需要拆分
```

- C++ 持有 `AdmScene` 和 renderer 编排，Rust 首批只接收计算所需的数据。场景模型无需复制，但模型中的坐标和时间换算仍要审计。
- 算法与 FFI 分离；算法可独立运行 `cargo test`，FFI 负责缓冲区、状态句柄和错误转换。FFT plan / scratch / RNG 可有实例状态，不能机械限制为无状态函数。
- 集中式 `mradm-ffi` 与算法伴生 `-cabi` 都可行。示意采用一个 staticlib，功能开关控制各算法是否进入构建；独立发布确有需求时再评估拆分。
- crate、回滚单元和发布单元不必一一对应。C++ 调用点可以按算法选择实现，不要求尚未成熟的算法与已完成部分同时上线。

## 4. 构建集成

### 4.1 Cargo 与 CMake

拟使用 Corrosion，通过 `cmake/MRDependencies.cmake` 的统一依赖路径接入，并在 `cmake/MRRust.cmake` 封装 Cargo 导入、目标命名和选项。这些文件与选项尚未实现。

新增 `MR_ADM_ENABLE_RUST`，初期默认 OFF。关闭时不查找 Rust、不调用 Cargo；开启时应锁定 Corrosion、Cargo 依赖及工具链，并确保 Cargo 实际使用 `rust/rust-toolchain.toml` 指定的版本。

需要验证的构建边界：

- **PIC**：C++ 的 `CMAKE_POSITION_INDEPENDENT_CODE` 不自动配置 Rust，需核对共享 bundle 所链接的 Rust 产物。
- **配置**：当前 preset 与规范 Windows 路径使用 Ninja 单配置生成器。明确 Debug / Release / MinSizeRel 与 Cargo profile 的映射；若支持其他生成器，再补对应验证。
- **MSVC CRT**：分别核对 C++ Debug `/MDd`、Release `/MD` 与 Rust 实际链接的运行库，不能仅凭“都是动态 CRT”认定匹配。跨 FFI 的分配必须由原分配方释放。
- **离线构建**：Rust 关闭只保证旧构建路径继续可用。开启后要另外准备 Cargo 缓存或 vendor 目录，并验证 `--locked --offline`、Corrosion 来源及工具链可用性。
- **依赖登记**：Corrosion 和新增 Rust 依赖的许可证 / 版本需要登记；现有 manifest 与 CMake tag 检查不足以自动审计全部 Cargo 传递依赖。

### 4.2 符号导出与内部 FFI

现有 `mradm_capi_bundle` 把 `adm_c_api.cpp` 直接编入 SHARED 目标，让 Windows 自动导出扫描看到公开入口。内部 Rust 算法由这些 C++ 路径调用，无需改变 GUI 的导出契约。

`WINDOWS_EXPORT_ALL_SYMBOLS` 不自动扫描所有静态归档，但允许合并显式 `.def` 文件以导出依赖符号。Rust 符号并非无法进入 DLL；保持 C++ 公开入口是本阶段的迁移选择，不能据此排除集中式 FFI。

直接引用通常可以拉取所需归档对象。注册表、链接时裁剪、LTO 和仅用于导出的符号需单独验证是否需要显式保留。阶段 1 应在三平台检查实际导出表、符号冲突和链接闭包；Windows、ELF、Mach-O 不能套用同一项“导出全部符号”的结论。

内部 FFI 在实现前明确参数范围、长度、别名规则、句柄释放和线程约束。错误按 ADR 0005 翻译，Rust panic / C++ 异常不穿过普通 C ABI。公开 ABI 的值和签名按 ADR 0007 保持兼容。

### 4.3 CI

在现有三平台 CI 中增加固定 Rust 工具链、Cargo 缓存和 Rust ON / OFF 构建验证。缓存 key 包含目标平台、工具链和 lockfile，并核对现有缓存清理规则是否匹配。

数值验收使用 Release 构建，正确性与错误路径检查继续使用 Debug。PR 修改数学代码、系数、相关依赖或构建选项时运行受影响的数值矩阵；main / 手动任务补完整矩阵。不得仅在合并后才发现已承诺路径的确定性回归。

## 5. 阶段

### 5.1 阶段 0：建立原始与受控基线

1. EAR 的 FFT 注释已修正；该修改没有改变运行时计算。
2. 先用当前 Release 配置采集原始输出与构建信息，再采集受控配置；不能先改数学选项再把输出称为原始基线。
3. 受控 C/C++ 构建显式处理 FP contraction / fast-math，例如 Clang/GCC 的 `-ffp-contract=off`、现代 MSVC 的 `/fp:precise`，并检查实际编译命令、显式 FMA 与剩余向量化。系统预编译库不受项目编译选项控制，必须另行验证或使用可控构建。

   已实现：`MR_ADM_STRICT_FP`（`cmake/MRStrictFp.cmake`）。Clang / GCC 加 `-fno-fast-math -ffp-contract=off`，MSVC 加 `/fp:precise` 并探测是否接受显式关闭 contraction 的开关（接受与否记进构建记录，不靠文档推断版本行为）。标志走 `CMAKE_<lang>_FLAGS`，按语言分开、不会漏进汇编方言；设置点在 `include(MRDependencies)` 之前。通用及活动配置的 `-Ofast`、`-ffast-math`、`/fp:fast` 等冲突选项会在配置时直接报错；vendored FLAC 自行追加的重结合 / `/fp:fast` 选项仅在受控配置中移除，默认配置保持原行为。其他目标追加的冲突由构建记录检查，失败时不得发布为有效受控基线。
4. libear 标量参考用 `EAR_SIMD=OFF`，让 dispatcher 进入 `generic_for_dispatch` 对应的 scalar 实现。`ear_default_arch` 仍是 `PolarExtentCoreSimd<xsimd::default_arch>`，会随目标架构变化；同时核对 Eigen 的向量化 / FMA 配置。

   已实现：`MR_ADM_EAR_SCALAR_REFERENCE` 只在配置 libear 的局部作用域把 `EAR_SIMD` 置 OFF，并用 CMP0077 保留用户缓存；关闭后恢复使用原设置。实际选择记录在 `MR_ADM_EAR_SIMD_EFFECTIVE`，不能用缓存中的 `EAR_SIMD` 偏好代替实际状态。生效证据取自 libear 自己传下来的 `XSIMD_ARCHS`：默认构建是 `avx512bw,avx2_fma,avx,sse4_2,default_arch,generic_for_dispatch`，开关打开后只剩 `generic_for_dispatch`。顺带说明默认构建的一个性质——这串 arch 是**运行时**按 CPU 特性分派的，所以同一平台上两台特性不同的机器本来就可能走不同实现，这是平台内的差异来源，不是平台间的。若 libear 来自已安装包，本开关对它无效，配置阶段直接 FATAL 而不是静默降级成一个站不住的「标量参考」。
5. 建立 §6 的 PCM 比较工具与矩阵。原始路径的预期差异作为报告保存，不用一个永久失败的 CI job 代替基线；已有通过的能力应成为持续通过的门禁。

   已实现：`tests/tools/make_fixture.cpp`（确定性输入）、`tests/tools/pcm_bits.cpp`（位提取、格式校验与比较）、`tests/tools/repeat_render.cpp`（同进程两次渲染）、`scripts/consistency/render-matrix.sh`（12 个跨平台 case 及同进程重复测量）、`scripts/consistency/compare-platforms.sh`（比对与报告）、`.github/workflows/consistency.yml`（三平台 + 汇总 job）。默认与受控组的门禁清单已按首轮三平台运行分别填入 5 个、6 个已确认一致的 case。

   两组配置由 preset `consistency-a`（默认数值）与 `consistency-b`（受控数值）固定。两者都关掉已安装包查找并锁定 FLAC / Opus 为 vendored。A 显式关闭两个受控开关、选择默认 EAR SIMD，B 在此基础上开启控制，避免沿用实验缓存；否则依赖来源不同会混进比较结果。两组各有自己的门禁清单——`expected-identical.txt` 与 `expected-identical-controlled.txt`，经 `compare-platforms.sh --expected` 选择——因为受控构建预期能收敛的 case 多于默认构建，拿它的成果去卡默认构建等于让后者为自己没声称过的结果长红。

   每个 runner 另外产出两份记录：`scripts/consistency/build-info.sh` 调用 Python 标准库实现，读取 `compile_commands.json`、CMake 导出的 `consistency-dependencies.json` 和真实源码目录，记录编译器、实际标志、SAF 后端与可取得的依赖 commit / dirty 状态。它支持自定义 FetchContent 缓存、源码目录覆盖和 `.git` 文件；外部包或非 Git 源码会明确记为 unavailable，不会把缓存中的闲置目录算作实际依赖。旧构建须先重新配置以生成依赖记录；受控编译命令缺失必要标志或仍有冲突时记录步骤返回失败；`scripts/consistency/scan-fp.sh` 数二进制里残留的 FMA 指令。后者是必要的：`-ffp-contract=off` 只阻止编译器自行融合，管不了显式 intrinsic，所以「设了标志」和「FMA 没了」是两件事。

**退出条件**：共享输入的哈希、三平台构建与依赖记录、原始 / 受控输出比较表、首次不同的计算阶段、待处理调用链。覆盖同进程重复渲染和不同线程数，避免把全局 RNG 或状态历史漏掉。

重复性和并行覆盖的边界：

- 两遍矩阵分别启动多个 `mradm` 进程，只能测量新进程重复性，不能证明同进程 C `rand()` 状态可复现。预检已观察到同进程 spreader 输出不同，因此现在用 `mr_adm_repeat_render` 在一个进程中对相同请求渲染两次；保留真实状态，不调用 `srand()` 掩盖差异。
- 单音轨 spreader fixture 只有一个 OLA source 和一个 adapter group，不会进入多 worker 路径。新增 `objects-extent-multi` 提供三个 Objects 音轨，在多核环境中覆盖多个 source / group；日志保存 `hardware_concurrency` 和准备期源数、分组数。
- 这些日志与多音轨 case 不等于已证明固定 1 / 2 / 4 worker 下输出一致。`taskset` 改变 CPU 亲和性也不保证 `hardware_concurrency()` 或线程池大小变化；固定 worker 数的等价性实验仍需可验证的控制入口。
- `out/<platform>/repeats/` 保存单 / 多音轨的两份 PCM、准备日志及同进程比较结果。已知数值差异按阶段 0 记录，渲染失败、损坏 PCM 或工具错误立即失败。跨平台汇总报告附上这些结果，以免把平台内的不稳定性直接归因于平台差异。

**配置 A / B 的首批 Linux 实测（GCC 13.3、Release、x86-64；含 FLAC 选项修正后重采，`validation.strict_fp=passed`。单平台数据，不构成三平台基线）**：

- 同一构建把 12 个 case 的矩阵连跑两遍，逐位一致。这是解读下面几条的前提——否则 A / B 的差异分不清是构建配置还是运行噪声。
- **只开 `MR_ADM_STRICT_FP`**：12 个 case 全部与默认构建逐位相同。`-ffp-contract=off` 落到全部 420 个翻译单元（默认构建 0 个），`mradm` 中的 FMA 指令从 212 条降到 54 条，却没有改变任何一个 case 的输出位。**这不等于 contraction 无害**，只说明本矩阵覆盖到的路径上它没有产生可观测差异。
- **再加 `MR_ADM_EAR_SCALAR_REFERENCE`（完整配置 B）**：只有 `ear-5_1-extent` 改变（288000 个采样中 87439 个不同，最大绝对误差 1.34e-07），其余 11 个仍逐位相同。它也是矩阵里唯一带 extent 的 EAR case，即唯一会进 `PolarExtentCore` 的那个，与「差异来自 SIMD 分派」一致。
- **剩余 FMA 全部来自 libear**：单独统计 `libear.a`，默认构建 60 条、只关 contraction 后 54 条、关掉 `EAR_SIMD` 后 0 条；此时整个 `mradm` 也是 0 条。即编译器自行融合的部分靠编译选项就能清掉，剩下的是 xsimd 的显式 intrinsic，只能靠换实现或关掉分派。这正是第 3 项要求「检查显式 FMA」而不是只看编译选项的原因。
- **只数 `-ffast-math` 会漏掉真正的破口**：默认构建的 `compile.fast_math` 是 0，而 `compile.unsafe_fp` 是 29——vendored FLAC 以目标级选项追加了 `-fassociative-math` / `-fno-signed-zeros` / `-fno-trapping-math` / `-freciprocal-math`，排在 `CMAKE_<lang>_FLAGS` 之后，早期只统计 `-ffast-math` 的记录看不见它们。移除这些选项后受控构建的 `compile.unsafe_fp` 为 0。这些 TU 全在 libFLAC，而基线矩阵统一写 f32 WAV、不经过 FLAC 编码，所以上面三条的输出比较**修正前后完全一致**；受影响的是「受控」这个说法本身能不能成立，以及一旦把 FLAC 输出纳入矩阵就会立刻显形。
- 覆盖不到的一层：Linux / Windows 的 OpenBLAS 与 macOS 的 Accelerate 是预编译库，任何项目编译选项都到不了；SAF 实际选中的后端记在 build-info 里。受控构建不能声称覆盖它们。

#### 首轮三平台基线（2026-09-08）

[完整运行](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/34185456813)使用提交 `3a0636e1a15d46a37100b2dc86ed3751fc95f919`，覆盖 macOS arm64、Linux x64、Windows x64。六组构建、工具回归、矩阵、构建记录和汇总均成功；CI 成功表示测量有效，不表示所有 PCM 已一致。

| 配置 | 三平台逐位一致 | 仍有差异 |
|---|---:|---:|
| A：默认数值 | 5 / 12 | 7 / 12 |
| B：受控数值 | 6 / 12 | 6 / 12 |

两组均一致的 case 为 `ear-5_1-directspeakers`、`ear-5_1-point`、`ear-5_1-point-postproc`、`saf-5_1-point`、`saf-5_1-extent`；B 组另有 `ear-5_1-hoa-input` 一致。这些结果已写入各自的 expected-identical 门禁。

两组仍有差异的是 EAR extent、HOA3 编码和四个双耳 case；A 组还包括 EAR HOA 输入。单 / 多音轨 spreader 的同进程重复测量在两组的三个平台均有差异，继续作为已知分歧记录。后续应沿这些实际路径定位，不能仅凭配置 B 或 FMA 计数宣告确定性。

第一次运行的 Windows 构建控制测试曾因 Python 用 CP1252 解码 CMake UTF-8 诊断而中断；修正日志编码后完整重跑，未将前后两个运行的产物拼接成基线。

##### 从平台相关性到计算阶段定位

首轮基线中，Linux 与 Windows 在 A、B 下均有 9/12 个 case 一致；macOS 独有的小差异、两种 spreader 的大差异以及 cloud 的残差，只是输出上的分组，不能直接作为相互独立的原因。

后续逐层检查点和控制实验见 [数值差异定位实验](CONSISTENCY_LOCALIZATION.md)。目前确认的边界包括：

- EAR HOA 输入的四组合实验表明，严格 FP 单独即可收敛，而单独关闭 EAR SIMD 无效。实际 f32 增益一致，首个 PCM 分歧由通道混加的 FMA 精确复现。
- EAR extent 的直接声/扩散声增益在受控构建中一致，后续 SAF FFT 路径存在分歧。macOS 换 OpenBLAS/KissFFT 后该测例与 Linux、Windows 收敛；不能把这一结果解释成 libear extent 增益仍受 SIMD 影响。
- HOA point 的三角函数输出和未归一化方向相同，第一次分歧发生在三参数 `std::hypot`。它属于 C++ 标准库算法差异，不能仅通过更换 BLAS 消除。
- SAF 的全局 C RNG 同时参与去相关延迟和凸包三角化。cloud 也会受进程历史影响；point 在测量网格点的相等性，不能证明其它方向的 HRTF 插值路径一致。
- 同一随机种子只统一同一 C 运行库内的起点；统一算法及 `RAND_MAX` 后，仍需处理 FFT、矩阵求逆、向量归约与插值数学。实际实验结果和未证明的推断须分开记录。
- 固定分组与 RNG 起点后，worker 调度和改变分组拓扑是两种不同实验。分组随硬件并行预算改变会影响多轨 spreader 的输出，不能用普通重复渲染代替线程数/拓扑验证。

因此 RNG 生命周期与三角化需要提前纳入第一批确定性改造；数学切片应覆盖 FFT、向量归一化和小矩阵运算。Rust 语言本身不保证这些操作跨平台逐位相同。

阶段 0 会决定后续切片大小。尚未定位的路径继续标为未完成，不因语言或库名推定确定性。

### 5.2 阶段 1：数学内核与 EAR FFT 切片

先建立 `mradm-math`、内部 FFI 和回滚开关。首批功能：

- 实数 FFT：替换 EAR 的 `saf_rfft_*`，保持长度、频谱布局和逆变换 1/N 缩放约定，验证系数生成及运算顺序。
- 数学函数：选择可审计实现。使用 `libm` 时锁定版本和 features，核对 `arch` / intrinsics 等实现选择，并用三平台位模式测试验证；不能仅凭使用该 crate 判定一致。
- 实例状态与缓冲管理：FFT plan 和 scratch 的创建 / 销毁不进入每个音频块的热路径。

同时检查 EAR 的上游输入：`designDecorrelators()` 的 FIR 系数、增益向量和共用数学。可保留 libear 算法并统一数学入口或保存经过验证的固定系数；仅改变项目侧 FFT 不会自动统一 libear 内部的 `std::exp` / KissFFT 结果。

随机数也需追踪，但两处性质不同，已按源码核对：

- libear 侧**已确认无需处理**。`src/decorrelate.cpp:19-21` 的 `genRandFloat` 是 `e() / static_cast<double>(0x100000000l)`，不是实现相关的 `std::uniform_real_distribution`；mt19937 序列由标准规定，种子是确定的 `decorrelatorId`，整数可精确转为 `double`，再除以 2³² 的浮点运算也精确。该链路跨平台可复现。这条路径上真正需要统一的是同函数 `:36-37` 的 `std::exp(std::complex<double>)`（落到平台 libm 的 `cos`/`sin`）。
- 阶段 2 的 SAF 去相关器**确为分歧源**，必须改用实例化的固定随机算法或固定延迟表：`saf_utility_decor.c:100` 用 C `rand()` 算去相关延迟、`:154` 用它生成白噪声，`:102` 的 `randperm()`（`saf_utility_misc.c:169`）同样基于 `rand()`。C `rand()` 的算法由各 libc 自定，且项目未调用 `srand()`，序列还依赖进程内的调用历史。不能等到未来 dither 功能再处理。

**模块退出条件**：相同 FFT 输入与状态在三平台位相等，数学正确性与旧实现误差指标通过，Release 性能可接受。

**EAR 路径退出条件**：选定矩阵内的增益、FIR、卷积和启用的后处理共同通过最终 float32 PCM 位比较。模块完成后仍有分歧的组合继续列为阻塞项，不宣称完整 EAR 已达标。

**回滚**：`MR_ADM_ENABLE_RUST=OFF` 恢复现有实现；测试构建保留按单元选择新旧实现的能力。

### 5.3 阶段 2：默认 OM spreader 的完整数学调用链

本阶段以实际使用的 `SPREADER_MODE_OM` 为重点：

- 替换项目直接调用的向量 / 矩阵运算，固定复数表示、累加、缩放和别名规则。
- 处理 `saf_cdf4sap` 内部的实数 / 复数 SVD 与 GEMM，即 `formulate_M_and_Cr_cmplx()` / `formulate_M_and_Cr()` 的传递调用。先列明矩阵尺寸、退化输入和调用频率，再决定实现及性能预算。
- 统一格型去相关器的随机算法、种子派生、延迟表、滤波器状态与重置顺序，消除进程级 C `rand()` 依赖。
- 统一该路径实际使用的 HRIR / afSTFT FFT、系数和矩阵计算。可以保留 SAF 外壳，但不能仅替换 EAR FFT 后继续使用不同平台的内部 FFT。
- 对要暴露的 EVD 模式再处理 `utility_cseig()`。循环 Jacobi 可作为候选，但须定义初始化、遍历、停止、特征值排序、并列与简并处理；规范化相位本身不让简并特征向量唯一。

**退出条件**：声明覆盖的双耳模式和语义组合在三平台最终 float32 PCM 位相等，并通过旧版语义 / 音质指标回归、重复初始化和状态测试。**1 ULP 不属于通过。** 保留模块中的未解决数学路径会阻塞此端到端退出条件。

**回滚与性能**：按调用单元保留双路径，Release 实测吞吐、内存和实时延迟；小矩阵在频带 / 音源循环中的总成本也需测量。

### 5.4 阶段 3：VBAP、HOA 与共用数学收敛

主要入口是 `generateVBAPgainTable*()` 和 `getLoudspeakerDecoderMtx()`，但不能假定二者都只在 prepare 阶段调用一次。动态位置和实时场景可能重复计算 VBAP；需要分别记录准备期与更新期的调用范围。

VBAP 覆盖二维 / 三维布局、虚拟扬声器、extent、凸包与退化几何，以及内部 BLAS / 数学调用。HOA 覆盖项目自己的编码系数、语义处理和用于响度测量的 AllRAD 解码，不能只替换解码矩阵生成函数。

**HOA 归一化约定**：现有 AllRAD 解码矩阵按 N3D 基底生成，项目对输入为 SN3D 的 HOA 信号逐列乘 `sqrt(2n+1)`。替换实现必须明确输出矩阵对应的基底；若直接生成适配 SN3D 输入的矩阵，就同时移除原补偿，避免重复缩放。

**退出条件**：声明的布局、语义、动态更新与后处理组合通过三平台 PCM 位比较，并在约定指标内通过旧 SAF / libear 行为回归。最终审查原生链接闭包，逐项记录仍保留的 SAF / libear / IO 依赖，不用已替换函数数冒充已删除库数。

## 6. 验证

### 6.1 跨平台 PCM 比较

三个 runner 使用同一份已固化的输入文件、ADM / policy / HRIR 和参数清单。记录输入哈希；若要合成 fixture，应生成一次后共享或使用已验证的整数 / 位模式生成器，不能让各平台分别用未受控的 `sin()` 生成输入。

验收流程：

1. 使用 Release 输出 float32 WAV 或直接采集目标 PCM 缓冲区，保留完整的采样率、声道标签顺序、帧数和生效参数。
2. 提取 PCM，确认读取没有量化或重采样。将 float32 的每个 32-bit 位模式按统一小端顺序序列化，先拒绝 NaN / Inf，再比较字节或其 SHA-256。正负零按位区分。
3. 元数据单独比较需要稳定的字段。**不比较完整容器哈希来判断 DSP 一致性**：输出会写入当前 UTC 时间；FLAC 还会先将 float32 量化为 24-bit，既可能误报文件差异，也可能掩盖渲染差异。已实测确认：同一输入、同一参数、间隔一秒的两次 `--renderer ear` 渲染，输出 WAV 文件不同（`render_service.cpp` 的 `date_utc` 经 bext `OriginationDate`/`OriginationTime` 写入），而提取出的 PCM 逐位相同。输入侧则无此问题——`make_fixture` 生成的 ADM BWF 跨次运行逐字节一致，因此输入用整文件比较即可，不需要另外取哈希。
4. 失败时报告首个不同的 frame / channel、两侧位模式、最大绝对误差和 ULP 分布；误差指标用于定位，不用于放宽位一致门禁。

覆盖矩阵至少包含 `ear × 5.1`、`saf × 5.1`、`hoa × hoa3`、`saf-binaural × binaural`，并明确区分 point、extent、diffuse、spreader OM、DirectSpeakers、坐标转换、动态块与自定义 HRIR 等实际路径。

双耳的 `auto` 当前选择 cloud，不能用默认双耳输出代替 spreader 验证。OM 测试须显式设置 `--binaural-spread-mode saf-spreader`，使用能实际进入该路径的 extent 元数据，并核对生效语义；需要改变 diffuse / extent 覆盖时使用临时 `--semantic-policy` 和 `--write-semantic-report`，不修改原始 ADM 音频文件。

基础渲染先关闭响度归一化与峰值增益调整，随后分别验证默认 True Peak 行为、响度目标、峰值归一化和最终增益。只有关闭后处理的单元通过时，不得声称带后处理的最终输出已通过。实时能力另加重采样、固定事件时间线和设备前 PCM 检查。

初期可以互相比对三平台产物，无需先建立新的 golden 文件格式；同时保存原始参考产物与独立正确性检查，避免三平台共同出现同一种错误仍被当作正确。固化基线时沿用 `CPP_ADM_PLATFORM_REWRITE.md` §10 的目录约定。

### 6.2 新旧实现差分与按位比较分开

测试构建拟增加 `MR_ADM_MATH_BACKEND=saf|rust` 或等价内部选择机制，让同一个测试进程比较两条实现。每次运行重置完整状态与固定随机序列，不能受上一次调用历史影响。

- **新实现跨平台、重复运行、线程数变化**：承诺范围内要求位相等。
- **新实现与旧实现**：按模块预先约定绝对 / 相对误差、能量、增益和语义等指标。旧平台 FFT 本身不同，不能要求统一的新算法逐位复制所有旧平台结果。
- **窗口渲染与完整渲染切片**：保持独立的时间线 / 状态测试，不因跨平台验证通过而直接删除现有容差。

复用 `ReaderHandle`、现有 fixture 构造与 CTest 注册方式；提取共享比较工具时，明确提供“位比较”和“有界误差比较”两个接口。现有 `maximum_difference()` / `window_bit_exact()` 用绝对差值，不等价于位比较；零容差不能区分正负零，未显式检查有限值的差值比较还可能漏过 NaN。

### 6.3 测量工具的回归测试

`tests/unit/consistency_tools_test.py` 仅使用 Python 标准库，调用真实工具与脚本，覆盖正负零、跨零 ULP 距离、NaN / Inf、损坏与溢出 header、缺失产物、case / fixture 清单不一致、比较工具错误、`--expected` 选中的替代门禁清单（生效、被强制执行、文件缺失即报错），以及门禁经 `tee` 管道传播的失败状态。工具输出状态约定为 0=通过、1=数值不同、2=输入 / IO / 配置错误。

`compare-platforms.sh` 在数值比较前校验所有平台的清单和每份 `.pcmbits`，拒绝空 / 重复清单、遗漏文件和失效门禁配置；对所有平台对输出结果。未登记的数值差异可以记录为基线，基础设施错误不能当作正常差异。workflow 显式使用带 `pipefail` 的 Bash，并在每个平台运行工具回归测试。

安装 Python 的测试构建会注册 `mr_adm_consistency_tool_tests`，可用 `ctest --test-dir build/debug -R consistency_tool --output-on-failure` 运行。Debug 检查协议和错误路径；矩阵与同进程 DSP 测量使用 Release。Bash 不可用时 CTest 只跑 PCM 工具部分；三平台 consistency workflow 明确传入 Bash 路径并要求完整工具测试。

`tests/unit/consistency_build_test.py` 使用离线的最小 CMake 工程验证真实依赖接入代码：标量开关 ON/OFF、用户 EAR 偏好保留、A preset 重置实验选项，以及通用 / Release 编译标志冲突、FLAC 目标选项的启停恢复。构建记录测试覆盖实际源码目录、`.git` 文件、闲置缓存、非 Git / 外部包、JSON 字段顺序和 `-Ofast`。它注册为 `mr_adm_consistency_build_tests`，workflow 三平台均显式运行。

早期版本用 FORCE 改写过的自定义构建目录无法自动推断原 EAR 偏好；使用更新后的 consistency presets，或显式指定一次 `-DEAR_SIMD=ON/OFF`。之后切换标量控制不会再改写该偏好。

### 6.4 Rust 数学正确性

`cargo test` 使用独立解析解或高精度参考验证 FFT、SVD / EVD、矩阵与数学函数，覆盖零输入、极小值、边界尺寸及退化矩阵。SVD / EVD 除参考结果外，验证重建残差与正交性；跨平台一致但数学错误仍必须失败。

系数、随机序列和内部状态也需要固定测试向量。性能验证使用 Release，记录优化前后实际配置；不能以 Debug 时间作为发布性能依据。

### 6.5 本地验证入口

PCM 提取 / 比较工具与矩阵脚本已实现，可直接运行；`MR_ADM_ENABLE_RUST` 仍未实现，下面带该选项的命令是阶段 1 的形态。现有 CTest 没有名为 `release` 的测试 preset，Release 测试使用构建目录。

跑完整矩阵并与另一平台的产物比对：

```bash
cmake --preset release
cmake --build build/release --target mradm_exe mr_adm_pcm_bits mr_adm_make_fixture mr_adm_repeat_render
bash scripts/consistency/render-matrix.sh build/release out/local
# 取另一平台的 out/<platform> 后：
bash scripts/consistency/compare-platforms.sh build/release/mr_adm_pcm_bits out/local out/other
```

跑原始 / 受控两组配置（阶段 0 第 2-4 项）。同一台机器上跑完 A、B 两组再互相比较，得到的是「构建选项改变了哪些 case」；把两组分别与其他平台的同组产物比较，得到的才是「受控构建收敛了多少跨平台差异」：

```bash
for c in a b; do
    cmake --preset consistency-$c
    cmake --build --preset consistency-$c \
        --target mradm_exe mr_adm_pcm_bits mr_adm_make_fixture mr_adm_repeat_render
    bash scripts/consistency/render-matrix.sh build/consistency-$c out/config-$c
    bash scripts/consistency/build-info.sh build/consistency-$c out/config-$c/build-info.txt
    bash scripts/consistency/scan-fp.sh build/consistency-$c/mradm out/config-$c/scan-fp.txt
done
bash scripts/consistency/compare-platforms.sh build/consistency-a/mr_adm_pcm_bits out/config-a out/config-b
# 受控组与其他平台比对时换用受控门禁清单：
bash scripts/consistency/compare-platforms.sh \
    --expected scripts/consistency/expected-identical-controlled.txt \
    build/consistency-a/mr_adm_pcm_bits out/config-b out/other-config-b
```

单个开关也可以单独打开（`-DMR_ADM_STRICT_FP=ON` 或 `-DMR_ADM_EAR_SCALAR_REFERENCE=ON`），用来把配置 B 的效果拆到具体某一项上。

单个文件的手工检查：

```bash
cmake --preset release -DMR_ADM_ENABLE_RUST=ON
cmake --build --preset release
ctest --test-dir build/release -R '(ear|render_trim)' --parallel "$(sysctl -n hw.ncpu)" --output-on-failure

# 基础渲染：float32，关闭默认峰值增益调整；不传 --loudness-target。
./build/release/mradm render -i fixture.wav -o out.ear.wav --renderer ear --output-layout 5.1 --output-bit-depth f32 --no-peak-limit
```

随后按 §6.1 提取 PCM 比较；不要对 `out.ear.wav` 整文件取哈希作为数学验收。后处理矩阵另用相应参数运行。Windows 使用仓库规范构建路径与 Windows 命令，不照搬 macOS 的 `sysctl`。

## 7. 风险与缓解

| 风险 | 处理方式 |
|---|---|
| FFT、SVD、矩阵替换后吞吐或实时延迟回归 | Release 测量；优化逐项证明与位基线一致 |
| 剩余 C/C++ / 第三方数学导致端到端不一致 | 沿实际调用链定位，不把保留库排除审计 |
| 随机初始化或状态历史破坏重复性 | 实例 RNG、固定种子 / 转换、重复创建与同进程 A/B |
| Cargo / CMake、PIC、CRT、符号与 LTO 不兼容 | 阶段 1 验证三平台实际链接、导出表与释放边界 |
| 离线构建和依赖审计不完整 | Rust ON / OFF 分别验证，锁定 Cargo 传递依赖并纳入许可证检查 |
| 双路径长期维护 | 每个替换单元记录门禁、默认切换和旧路径退出条件 |
| 仅模块一致却宣称完整 renderer 一致 | 分开记录模块与端到端状态，按语义 / 后处理组合公布覆盖 |

## 8. 后续方向

以下需要独立评估，不由本路线自动扩展实现范围：

- **BW64/RF64 自研**：可替换 `libbw64` 和对应 WAV 读写路径，需验证大文件、seek 和 ADM metadata。`dr_flac` 是 FLAC 解码器，不能由 BW64 容器实现替代；`dr_wav` / `dr_flac` 同属 dr_libs，依赖收益还要看保留用途。
- **计量 / 重采样 / 设备库替换**：评估 Rust `ebur128`、`rubato`、`cpal` 等候选的功能、数值与延迟，以及原生和 Cargo 传递依赖；使用 Rust 封装不自动移除系统音频依赖。
- **CLI 迁移**：有机会移除 CLI11 / spdlog 的 CLI 用途。`fmt`、`nlohmann_json`、`tl-expected` 仍被 core / engine 等使用，只有这些使用点也迁移后才能从闭包移除。
- **SOFA / afSTFT / spreader 全量替换**：与本路线中必要的数学入口统一区分，按兼容性、性能和真实依赖收益另行决定，不预先断言收益最低。
- **历史 ABI 命名**：可以继续保留 `ADM_RENDERER_SAF` 等名字及数值；若要调整，遵循 ADR 0007，不把改名作为内部替换的前置条件。
