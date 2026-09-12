# ADR 0008：Rust 落地方向与 SAF 按模块替换

> 状态：已接受（架构方向；实现与数值验收尚未完成）
> 日期：2026-09-07
> 适用范围：同仓库 `rust/` Cargo workspace、SAF 数学模块替换、C ABI 与 GUI 边界，以及跨平台 PCM 逐位一致的验收契约。本 ADR 是 ADR 0002 第二阶段的具体化。

## 背景

ADR 0002 确立了先建立 C++ 地基、再按模块引入 Rust 的路线。本 ADR 根据现有源码和依赖调用链，确定首批进入点与验证要求。下面的调用链盘点属于静态审计，尚未完成三平台渲染对比，不能据此宣称某条完整渲染路径已经确定或已经测得可听差异。

### 一、保留 libear 的标准算法实现，同时审计其数值行为

生产代码通过 `src/adm_render_ear/ear_renderer.cpp` 适配 `libear`，主要使用布局、Objects / DirectSpeakers / HOA 增益计算和去相关 FIR 设计。逐块增益累加、流式状态和去相关卷积由项目侧管理，适配边界较窄。

保留这一标准实现能减少重新验证 BS.2127 语义的工作，但边界窄不代表其数值输出与平台无关。`libear` 的 `designDecorrelatorBasic()` 使用 `std::mt19937`、复数 `std::exp` 和内部 KissFFT；增益计算也使用三角函数、Eigen 和可选 xsimd 路径。即使不重写标准算法，也需要统一或验证这些实现、系数和构建选项。

这三项的风险并不等价，已按源码区分（libear `src/decorrelate.cpp`）：

- **随机序列可复现，不构成分歧源。** `:19-21` 的 `genRandFloat` 是 `e() / static_cast<double>(0x100000000l)`，即 mt19937 输出的整数转成 `double` 后除以 2³²，**不是** `std::uniform_real_distribution`（后者的取值与实现相关，各标准库不一致）。`std::mt19937` 的序列由标准规定，整数可精确转为 `double`，除以 2³² 也可精确表示，且种子是确定的 `decorrelatorId`。因此这条链路不需要替换 RNG。
- **`std::exp(std::complex<double>)`（`:36-37`）是这里的实际风险**：它展开为 `exp(re)` 与 `cos(im)` / `sin(im)`，落到平台 libm，Apple / glibc / MSVC UCRT 三家结果不同。
- **内部 KissFFT（`:43`）也需要审计系数生成**。其构造函数使用 `std::acos` 和复数 `std::exp` 生成 twiddle；同一份源码不保证这些平台数学函数一致。需要同时统一数学入口 / 系数及编译期 FP 行为。

结论：EAR 的 FIR 系数若要跨平台一致，需要统一 `sin` / `cos` 入口或保存经验证的固定系数表；不需要为 RNG 单独设计方案。SAF 去相关器是另一回事（见下），其 C `rand()` 确为分歧源。

### 二、SAF 的调用面需要沿传递依赖盘点

SAF 用于 EAR FFT、VBAP、HOA 解码、HRTF 准备、SOFA、afSTFT、格型去相关器以及 spreader 的协方差合成。只统计项目源文件中的 `saf_*`、`utility_*` 或 `cblas_*` 符号会漏掉实际执行的数学代码。

两个影响阶段划分的调用链已经确认：

- `BinauralSpreaderAdapter` 显式选择 `SPREADER_MODE_OM`，经 `formulate_M_and_Cr_cmplx()` / `formulate_M_and_Cr()` 进入 `saf_cdf4sap`，内部继续调用复数 / 实数 SVD 和 GEMM。`utility_cseig()` 位于另一条 EVD 模式分支，替换它不会消除默认 OM 模式中的 LAPACK 调用。
- `spreader_initCodec()` → `latticeDecorrelator_create()` → `getDecorrelationDelays()` / `randperm()` 使用 C `rand()` 初始化去相关延迟。随机算法、种子和进程内调用顺序都属于渲染状态；仅固定 C `srand()` 的参数不保证不同 C 运行库生成相同序列。

稳定 C ABI 已含 `ADM_RENDERER_SAF`、`ADM_RENDERER_SAF_BINAURAL` 和 `ADM_BINAURAL_SPREAD_SAF_SPREADER` 等名字。这是兼容性命名，不要求底层永远链接 SAF。替换实现时保持旧枚举值和接口语义；如需改名或移除入口，遵循 ADR 0007。

### 三、确定性必须覆盖完整 PCM 生成链路

当前默认构建在 macOS 选择 `SAF_USE_APPLE_ACCELERATE_ILP64`。SAF 的 CMake 将其匹配为 `SAF_USE_APPLE_ACCELERATE`，支持的 FFT 长度走 vDSP；Windows / Linux 的 OpenBLAS 配置通常回退到 KissFFT。原 EAR 注释中固定写作 KissFFT / platform-agnostic 不准确，现已修正。统一 FFT 是一个可验证切片，但不是整条 EAR 路径确定性的充分条件。

其他需要审计的路径包括：

- 项目场景坐标转换、头部旋转、HOA 编码、extent 和增益处理中的平台数学函数；
- `libear` 的系数设计和增益计算，以及保留的 HRIR / afSTFT 内部数学；
- spreader 默认 OM 模式的 SVD、矩阵运算和随机初始化；
- `libebur128` 测量、响度归一化、True Peak 增益调整及最终增益换算；这些测量会反馈到最终 PCM；
- 重采样、分块、状态重置和输入事件顺序，以及启用功能实际触达的其他依赖。

特征分解的相位、排序和简并子空间需要明确处理规则，但差异是否传递到输出、是否可听，要由实际模式和实验判断。仅规范化特征向量相位也不能唯一确定重特征值对应的基。

现有 `window_bit_exact()` 验证同平台窗口渲染与完整渲染切片的关系。它可作为时间线回归参考，不能作为已经通过跨平台逐位验收的证据。

## 决策

### 决策一：保留 libear，不在本路线重写 BS.2127

`libear` 继续作为 EAR 后端的私有依赖，由 ADR 0003 约束类型边界。本路线不自行重写其标准增益算法，但允许为确定性统一构建选项、数学调用、固定系数表或维护小范围补丁，并验证标准语义回归。

保留依赖不豁免数值验收。若阶段 0 证明某个剩余调用阻碍目标，须调整实现或明确暂未达标的能力范围，不能将它排除审计后仍宣称整条路径已逐位一致。

### 决策二：优先替换 SAF 数学模块，数值审计覆盖全链路

Rust 放在当前仓库的 Cargo workspace 中，先进入可独立比较与回滚的数学模块。SAF 是首批替换重点；项目自有 C++ 和其他依赖中的数值问题按调用链同时处理。

本路线不完整重写 `libadm` / `libbw64`，也不预先承诺重写整个 HRTF / afSTFT / SOFA 系统。保留外壳时，其内部会影响目标 PCM 的 FFT、系数准备、矩阵和状态初始化仍必须处理。没有消除的数值分歧继续列为验收阻塞项。

依赖收益按最终目标的传递依赖计算。替换某组函数不等于可从发行包删除 SAF；只要 HRIR、SOFA 或其他保留路径仍链接它，就应如实记录这一依赖。

### 决策三：当前集成方向为 C++ 调 Rust

C++ 继续拥有公开的 `include/adm/c_api.h` 和 `mradm_capi` 入口，GUI 的 `LibraryImport` 边界保持兼容。Rust 算法通过内部 C 接口被现有 C++ 后端调用。这是渐进迁移的工程选择，不是 Windows 的语言限制。

`WINDOWS_EXPORT_ALL_SYMBOLS` 自动扫描 SHARED 目标的输入对象文件，不能指望它自动发现所有静态归档入口；但 CMake 支持合并显式 `.def` 文件，MSVC 链接器也支持显式导出依赖符号。集中式 FFI crate 和算法伴生 `-cabi` crate 都可被 C++ 正常引用，导出机制不能决定 crate 的组织方式。

算法层保持普通 Rust 库；C 边界单独承接指针、长度、错误和所有权转换。首版可以用一个内部 `mradm-ffi` staticlib 聚合算法入口，只有独立打包确有需求时再拆伴生库。普通直接引用通常足以拉取所需对象；仅靠注册表或显式导出的入口需要另行验证保留策略，不能一概要求或排除 whole-archive。

### 决策四：首批 Rust 模块不复制领域模型

`AdmScene` 和渲染编排继续由 C++ 持有。数学模块优先接收标量、定长结构或带长度的缓冲区，避免重复维护完整领域模型。场景中的坐标与时间转换仍属于确定性审计范围。

FFT plan、随机序列和 scratch 等需要生命周期的状态可以通过不透明句柄管理；不能为了“无状态”而在每个音频块重建状态。分配方负责释放，跨边界约定别名、可重入性和错误处理，Rust panic / C++ 异常不得穿过普通 C ABI。

本阶段不要求 Rust 直接实现带 STL 类型的 `IRenderer` / `IRenderStream`。未来可由 C++ adapter 转调 Rust，属于边界设计问题，并非跨语言实现不可行。

### 决策五：算法单元与发布组织分开

按可替换、可验证的计算单元组织模块。初期建立 `mradm-math` 与内部 FFI 边界，VBAP / HOA 成熟后按需要拆 crate。crate 数量不要求与 renderer 数量对应，也不要求每个 crate 独立仓库或同步启用。

算法库以普通 Rust 库形式复用，FFI 层隔离 `unsafe` 和 C 符号导出。功能开关、C++ 调用点及 CI 决定各单元是否启用；集中式 FFI 不应强迫尚未成熟的算法参与构建或发布。

### 决策六：确定性契约

首批验收平台为 macOS arm64、Windows x64、Linux x64。同一实现版本、输入 PCM / ADM / HRIR 字节、布局、参数和事件序列，在承诺覆盖的功能组合上必须产生相同的最终 float32 PCM 位模式。整数 PCM 的量化与编码器输出另列测试。

- **完整计算链一致。** 数学算法、系数、FMA 语义、舍入环境、极小浮点数处理、分块和累加顺序均需固定或证明等价。单纯改成 Rust 或 `f64` 不提供该保证。
- **数学函数可审计。** Rust 首选经过验证的纯实现，`libm` 可作为候选，但必须锁定版本、features 和实际执行路径；不能把 crate 名字或“纯 Rust”当作保证。保留的 C/C++ 数学调用也须验证或替换。
- **先建立统一基线再加速。** 首版使用标量算法，关闭未经验证的运行时分派。跨声道并行通常容易保持每个样本的操作顺序；任何 SIMD 或线程优化都必须通过同一按位测试，不能仅凭并行形式推定安全。
- **线性代数覆盖传递调用。** 确定性路径不能继续依赖未经验证的平台 BLAS/LAPACK。为实际用到的 SVD / EVD 明确迭代顺序、停止条件、排序与并列规则；相位归一化不足以唯一决定简并子空间。
- **随机状态归实例所有。** 固定算法、种子派生、采样转换和初始化顺序，不依赖全局 C `rand()` 的实现或进程历史。
- **验收按位进行。** float32 以 `bit_cast<uint32_t>` / `to_bits()` 比较，并拒绝 NaN / Inf；正负零也按位区分。1 ULP 或绝对误差为零的数值比较不替代位比较。

Apple AUSpatialMixer、系统设备混音和系统编码器不属于软件渲染 PCM 的跨平台承诺。实时路径如纳入承诺，应固定输入事件时间线，在设备输出前取样；不能比较不同硬件实时操作的结果。

## 阶段边界

详细执行计划见 [Rust 落地与 SAF 替换路线图](../architecture/RUST_SAF_REPLACEMENT_ROADMAP.md)。

- 阶段 0 先测当前实现，再分别测受控构建，产出 renderer × 布局 × 语义 / 后处理选项的基线和剩余调用链。
- 数学模块验收与端到端验收分别记录。模块通过按位测试不等于整个 renderer 达标；只有完整目标路径通过，才能宣称该能力跨平台逐位一致。
- 新实现跨平台比较使用位相等；与旧 SAF / libear 的行为回归使用事先确定的误差和语义指标，不要求新算法复制多个旧平台各自的舍入结果。
- 每个替换单元保留回滚开关及 CI 覆盖。`MR_ADM_ENABLE_RUST` 初期默认 OFF，不改变当前默认发布行为。

## 风险与后果

- 标量 FFT、矩阵与 SVD 可能影响吞吐和实时延迟。阶段 1、2 使用 Release 实测，不从矩阵尺寸或函数行数直接推断性能成本。
- 引入 Rust / Cargo 和 Corrosion 后，工具链、PIC、MSVC CRT、静态库链接及导出表需要三平台验证。符号冲突应依据实际链接结果处理；Windows 自动导出与 ELF / Mach-O 的可见性规则不同。
- Corrosion 按 ADR 0004 接入并登记依赖。Rust 关闭时不调用 Cargo；Rust 开启时还需准备锁定的 Cargo 依赖和离线缓存，默认关闭不能证明开启后的离线构建可用。
- 迁移期会同时维护旧路径和新路径。应按单元记录退出条件与移除旧路径的时机；可回滚不会自动消除双路径维护成本。
- C ABI 名字和数值保持兼容。减少原生依赖、缩小数学实现范围和减少 Cargo 传递依赖是不同指标，需要分别记录。

## 参考资料

- [ADR 0002：先建立 C++ 地基，后续渐进引入 Rust](0002-cpp-first-rust-later.md)
- [ADR 0003：自有 ADM 领域模型与后端边界](0003-owned-domain-model-and-backend-boundaries.md)
- [ADR 0004：第三方依赖管理策略](0004-third-party-dependency-management.md)
- [ADR 0005：错误处理模型](0005-error-handling-model.md)
- [ADR 0007：C ABI 稳定性承诺与版本策略](0007-c-abi-stability-policy.md)
- [Rust 落地与 SAF 替换路线图](../architecture/RUST_SAF_REPLACEMENT_ROADMAP.md)
- [CMake WINDOWS_EXPORT_ALL_SYMBOLS](https://cmake.org/cmake/help/latest/prop_tgt/WINDOWS_EXPORT_ALL_SYMBOLS.html)
- [Rust f32 数学函数精度约定](https://doc.rust-lang.org/std/primitive.f32.html)
- [libm](https://github.com/rust-lang/libm)、[libear](https://github.com/ebu/libear)、[SAF](https://github.com/leomccormack/Spatial_Audio_Framework)、[Corrosion](https://github.com/corrosion-rs/corrosion)
