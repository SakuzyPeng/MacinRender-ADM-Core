# ADR 0008：Rust 落地方向与 SAF 按模块替换

> 状态：已接受
> 日期：2026-09-07
> 适用范围：`rust/` Cargo workspace 的存在形式与边界、`src/adm_render_*` 对 SAF 的依赖收敛路径、C ABI 与 GUI 边界的所有权归属、跨平台浮点确定性契约。本 ADR 是 ADR 0002 第二阶段的具体化，不推翻 ADR 0002。

## 背景

ADR 0002 确立了「先 C++ 地基、后续按模块渐进引入 Rust」的语言路线，并列出了 Rust 可优先进入的区域（CLI、工具链、metadata 诊断、边界清楚的小型 DSP），但没有回答两个具体问题：**第一个 Rust 模块落在哪里**，以及**哪些 C++ 依赖是要被替换的、哪些是要长期保留的**。

在评估「是否值得 Rust 重写」时对依赖树做了一次实测盘点，结论与 ADR 0002 写作时的假设有偏差，需要单独记录：

### 一、libear 的耦合面远小于其实现体量

`libear` 只被 `src/adm_render_ear/ear_renderer.cpp` **一个文件**引用，符号面 13 个（`getLayout` / `GainCalculatorObjects` / `GainCalculatorDirectSpeakers` / `GainCalculatorHOA` / `designDecorrelators` / `decorrelatorCompensationDelay` / `Layout` / `Channel` / `PolarPosition` / `PolarSpeakerPosition` / `ObjectsTypeMetadata` / `DirectSpeakersTypeMetadata` / `HOATypeMetadata`）。

其接口是纯函数式的：**元数据进 → 增益向量出**，无状态、无回调、无流式生命周期。EAR 渲染器中真正的 DSP（增益累加、去相关卷积、重叠相加）在项目自有代码里，不在 libear 内。

ADR 0002 把「第一阶段 BS.2127 标准渲染主路径」列为不建议 Rust 承担的区域，本 ADR 把它升级为**永久约束**：耦合面这么窄的依赖，替换收益为零而承担 BS.2127 一致性验证义务的成本极高。

### 二、SAF 才是真正的脊柱，且已泄漏进稳定 C ABI

SAF 被 4 个 renderer target 链接、16 个文件引用，实际调用面约 30 个函数，跨 8 个区域：FFT（`saf_rfft_*`）、VBAP（`generateVBAPgainTable*`）、HOA 解码（`getLoudspeakerDecoderMtx`）、HRIR（`HRIRs2HRTFs` + 内置 KEMAR 数据）、SOFA（`saf_sofa_open/close`）、afSTFT、格删相关器（`latticeDecorrelator_*`）、veclib（`utility_cseig` 等）。

更关键的是 SAF 的名字已经进入 stable C ABI，带 `static_assert` 守护（`src/adm_c_api/adm_c_api.cpp:157,161,174`）：

```
ADM_RENDERER_SAF
ADM_RENDERER_SAF_BINAURAL
ADM_BINAURAL_SPREAD_SAF_SPREADER
```

即：libear 可以静默替换而用户无感，SAF 不行——替换它要么永久保留这些名字，要么走 ADR 0007 的 deprecation 流程。这个成本与实现语言无关。

### 三、跨平台数学不一致的根因全部在 SAF，且是构建配置问题

已验证的链条：

- 项目设 `SAF_PERFORMANCE_LIB=SAF_USE_APPLE_ACCELERATE_ILP64`（`cmake/MRDependencies.cmake`）→ SAF 上游 `framework/CMakeLists.txt:175` 用 `MATCHES` 正则命中该值 → `:177` 定义 `SAF_USE_APPLE_ACCELERATE=1` → `saf_utility_fft.c:573` 走 **vDSP DFT**；Windows/Linux 走 **KissFFT**。
- 因此 `src/adm_render_ear/ear_renderer.cpp:94` 与 `:673` 的注释「Uses overlap-add FFT convolution via saf_rfft (KissFFT backend, platform-agnostic)」**在 macOS 上不成立**。这是一处实际缺陷，且位于默认 EAR 渲染器的 diffuse/去相关路径。
- `saf_utility_veclib.c` 的 `utility_cseig` 走 LAPACK `cheev_`（复 Hermitian 特征分解）。Accelerate LAPACK 与 OpenBLAS LAPACK 的特征向量**符号/相位可能不同**，简并特征值时顺序也可能不同——这是可听差异级别，不是舍入级别。
- `src/adm_render_binaural/spreader_mr.c`（项目自有 fork，904 行）有约 30 处 cblas 直接调用，是 BLAS 分裂的最大暴露面。
- `tests/unit/render_trim_fixture_test.cpp:800` 已有记录：EAR/VBAP/HOA 要求 bit-exact，binaural 需放宽容差以容纳「platform math differences in the HRTF overlap path」。

## 决策

### 决策一：libear 永久保留，不自研 BS.2127

不实现自有的 BS.2127 增益计算。`libear` 长期作为 `mr_adm_render_ear` 的私有依赖存在，边界继续由 ADR 0003 约束（libear 类型只允许出现在 `src/adm_render_ear/` 内部）。

若将来引入 Rust 侧的 EAR 调用方，走 `libear-sys` 风格的薄 FFI 封装，而非重新实现。

### 决策二：SAF 是唯一的替换目标，按模块蚕食

`rust/` workspace 的定位是**按模块替换 SAF 的载体**，不是全量重写的地基。替换按上述 8 个区域逐个进行，每个区域独立过边界、独立上线、独立回滚。

明确排除在本路线之外：`libadm`（ADM XML）、`libbw64`（BW64 容器）、双耳 HRTF/afSTFT/SOFA 链路。前两者不是 SAF 问题；后者是全树风险最高的区域，须在前序阶段建立起差分测试信任后单独决策。

### 决策三：FFI 单一方向——C++ 调 Rust

C++ 保持 `include/adm/c_api.h` 的唯一所有权。GUI 的 `[LibraryImport("mradm_capi")]` 边界完全不动。Rust crate 只被现有 C++ 后端**内部**调用。

这不只是偏好，构建事实强制如此：`mradm_capi_bundle`（`CMakeLists.txt:626-641`）依赖 `WINDOWS_EXPORT_ALL_SYMBOLS`，而该属性**不导出从静态归档传递进来的符号**——项目已经为此把 `adm_c_api.cpp` 直接编进 SHARED 目标（见 `:632` 注释）。Rust 符号在 Windows 上无法直接出现在 DLL 导出表，必须由已在导出路径上的 C++ 代码包装转调。

推论：不存在集中式的 `mradm-ffi` crate。每个算法 crate 自带一个 `-cabi` 伴生 crate，由调用它的 C++ 后端直接引用。因为 C++ 直接引用，普通链接即可拉取，**不需要** `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>`。

### 决策四：Rust 侧不建领域模型

不在 Rust 侧复制 `AdmScene`（`include/adm/scene.h`，394 行、约 15 个 struct 的值类型模型，被 `RenderPlan` 按值持有，见 `include/adm/render.h:80`）。Rust crate 只提供**无状态叶子函数**：POD 进、POD 出。

Rust 也不实现 `IRenderer` / `IRenderStream`——这两个接口的签名含 `std::span`、`Result<T>`、`std::shared_ptr`、`std::optional<std::filesystem::path>`，跨语言实现不可行且无必要。

### 决策五：crate 按可替换单元划分，不按领域概念

划分依据是**接缝形状**：一个 crate 对应一个可以独立替换、独立验证、独立回滚的 SAF 调用点集合。

按领域概念聚合（例如把 VBAP、HOA、双耳合成一个 `dsp` crate）会把成熟度差一个数量级的东西绑在一起——VBAP 的 `generateVBAPgainTable3D_srcs` 与 HOA 的 `getLoudspeakerDecoderMtx` 都是纯函数、POD 进出、prepare 期只调一次；双耳则涉及 afSTFT（上游 4793 行）、`spreader_mr` 的协方差域合成与 LAPACK 特征分解、以及 SOFA 读取拖入的 libmysofa + zlib（约 16K 行）。绑在一起意味着成熟的部分要等最不成熟的部分。

算法 crate 与 `-cabi` crate 分离：算法 crate 保持纯 safe Rust（`cargo test` 直接可跑），`-cabi` 隔离全部 `unsafe` / `#[no_mangle]` / `repr(C)`，且 `staticlib` crate-type 会拉入完整 Rust std，不适合作为普通依赖被复用。

### 决策六：确定性契约

Rust 侧数学实现受以下硬约束，违反即视为缺陷：

- **禁止运行时 SIMD 分派。** 不使用 `rustfft` 等带 AVX/SSE/NEON 运行时分派的库——它们跨架构非逐位一致。若需 SIMD 加速，**只允许跨声道并行，不允许在变换内部并行**：前者不改变单声道内的运算顺序，保住 bit-exactness。
- **禁止平台 libm。** 超越函数必须走 `libm` crate（MUSL 纯 Rust 移植），不使用 `f64::sin` 等标准库方法——后者调用平台 libm，Apple / glibc / MSVC UCRT 三家结果不同。这是 Rust 确定性最常见的误解：Rust 免费给的是「不做 FP contraction、不做重结合」，不包括超越函数。
- **禁止引入 BLAS/LAPACK 绑定。** 线性代数用纯 Rust 实现，且在算法层面消除歧义（例如特征分解用循环 Jacobi 并规范化特征向量相位，使结果唯一）。
- **固定累加顺序。** 向量与矩阵运算不做与规模相关的分块变化。

## 阶段边界

详细执行计划见 [Rust 落地与 SAF 替换路线图](../architecture/RUST_SAF_REPLACEMENT_ROADMAP.md)。ADR 层面只约束阶段的**准入条件**：

- **阶段 0（纯 C++）必须先完成**，产出「三平台在哪个 renderer × 布局上不一致」的实测基线。没有基线，后续无法区分 Rust 修复了什么、引入了什么。
- 每个后续阶段的退出条件是**可度量的**：目标渲染路径在 macOS / Linux / Windows 产出 byte-identical 输出。
- 每个阶段都必须保留回滚开关（`MR_ADM_ENABLE_RUST=OFF` 或 `#ifdef` 双路径），且回滚路径在 CI 中有覆盖。

## 风险

- **标量 FFT 性能不足**：确定性要求排除了 vDSP 与运行时分派 SIMD。缓解：阶段 1 必须实测；加速方向限定为跨声道并行。若实测不可接受，需回到 ADR 重新权衡「确定性 vs 性能」的取舍点。
- **Rust std 符号污染 bundle 导出表**：仓库目前没有任何 `-fvisibility=hidden` / version script / 导出白名单，`mradm_capi_bundle` 会导出全部传递静态库的默认可见性符号。加入 Rust 后 `compiler_builtins` 提供的 `memcpy`/`memset` 等可能与 C 侧冲突。缓解：阶段 1 早期实测；补可见性收敛作为独立的既有卫生项。
- **构建期依赖增加**：Cargo-CMake 桥接（Corrosion）与 Rust 工具链进入构建链路，与 `MR_ADM_CORE_FETCH_DEPS=OFF` 的离线/发行版打包路径存在张力。缓解：`MR_ADM_ENABLE_RUST` 默认 OFF，发行版打包路径不受影响；Corrosion 走 `mr_adm_core_find_or_fetch()` 并登记 `third_party/manifest.json` + SBOM + `scripts/quality/check-licenses.sh`。
- **`spreader_mr.c` 去 BLAS 后性能回归**：`cgemm` 在热路径上。缓解：保留 `#ifdef` 双路径一个发布周期，实测后再删。
- **阶段 0 暴露的分歧点多于预期**：`ear_renderer.cpp:94` 的错误注释说明当前对平台分歧的认知不完整。缓解：这正是阶段 0 先行的目的；分歧表产出后重排后续优先级。

## 后果

优点：

- 明确了「哪些依赖永久保留」，消除了「是不是迟早要全部重写」的悬置状态。libear 从「最贵的替换目标」重新定位为「最该保留的依赖」。
- 跨平台数学一致性从「靠容差兜住」变成「可度量、可回归的 CI 断言」。
- 每个 Rust crate 是独立可回滚单元，不存在「重写做到一半两套代码库并行维护」的经典风险。
- GUI 与 C ABI 边界零变更，ADR 0007 的兼容承诺不受影响。

代价：

- 引入第二套工具链（Rust + Cargo）与一个构建期桥接依赖，CI 时间与维护面增加。
- 确定性契约排除了部分高性能实现路径（vDSP、运行时分派 SIMD、BLAS），存在性能代价，需实测确认可接受。
- `ADM_RENDERER_SAF` 等 ABI 名字在 SAF 实际被替换后会变成历史遗留命名，需在 ADR 0007 框架下单独决策是保留还是 deprecate。

## 参考资料

- 语言路线上位决策：[ADR 0002：先建立 C++ 地基，后续渐进引入 Rust](0002-cpp-first-rust-later.md)
- 模块边界约束：[ADR 0003：自有 ADM 领域模型与后端边界](0003-owned-domain-model-and-backend-boundaries.md)
- 依赖接入约束：[ADR 0004：第三方依赖管理策略](0004-third-party-dependency-management.md)
- C ABI 兼容承诺：[ADR 0007：C ABI 稳定性承诺与版本策略](0007-c-abi-stability-policy.md)
- 执行细节：[Rust 落地与 SAF 替换路线图](../architecture/RUST_SAF_REPLACEMENT_ROADMAP.md)
- 回归基线规划（golden fixture 字段与目录约定）：`docs/architecture/CPP_ADM_PLATFORM_REWRITE.md` §10
- Spatial_Audio_Framework：https://github.com/leomccormack/Spatial_Audio_Framework
- libear：https://github.com/ebu/libear
- Corrosion（Cargo-CMake 桥接）：https://github.com/corrosion-rs/corrosion
- `libm` crate（MUSL 移植，确定性超越函数）：https://github.com/rust-lang/libm
