# Rust 落地与 SAF 替换路线图

> 状态：规划中（阶段 0 第 1 项已完成，其余未开工）。本文是 ADR 0008 的执行细节：`rust/` workspace 的 crate 划分与理由、Cargo↔CMake 集成的具体约束、四个阶段的内容与退出条件、差分测试与跨平台一致性验证的搭法。
>
> 相关：ADR 0008（Rust 落地方向与 SAF 按模块替换）、ADR 0002（语言路线）、ADR 0003（后端边界）、ADR 0004（依赖接入）、ADR 0007（C ABI 稳定性）。

## 1. 目标与非目标

**目标**：消除跨平台浮点分歧，同时按模块收敛对 SAF 的依赖。

**非目标**：
- 不重写 `libear`（BS.2127 增益计算永久保留，见 ADR 0008 决策一）
- 不替换 `libadm` / `libbw64`（不是 SAF 问题，超出本路线定位）
- 不替换双耳 HRTF / afSTFT / SOFA 链路（风险最高，须在前序阶段建立差分测试信任后单独决策）
- 不让 Rust 拥有 `adm_c_api`，GUI 边界零变更

## 2. 现状：SAF 的实际调用面

实测 30 个函数，跨 8 个区域。上游代码量已剔除数据表（`saf_utility_loudspeaker_presets.c` 的 53,500 行是数据不是代码）。

| 区域 | 项目调用的函数 | 上游真实代码量 | 是否为分歧源 | 处置阶段 |
|---|---|---|---|---|
| FFT | `saf_rfft_{create,forward,backward,destroy}` | 973 + kissFFT 952 | **是**（vDSP vs KissFFT） | 阶段 1 |
| veclib | `utility_{cseig,siminv,cvsmul,svsmul,svvcopy}` | 4,800（92 处 BLAS/LAPACK） | **是**（`cseig` → LAPACK `cheev_`） | 阶段 2 |
| VBAP | `generateVBAPgainTable{3D,3D_srcs,2D_srcs}` | 1,592 + convhull_3d 1,672 | 间接（内部走 BLAS） | 阶段 3 |
| HOA 解码 | `getLoudspeakerDecoderMtx` | saf_hoa 2,085 + saf_sh 4,894 | 间接（内部走 BLAS） | 阶段 3 |
| HRIR | `HRIRs2HRTFs` + 内置 KEMAR 数据 | 764 代码 + 1,714 数据表 | 间接 | 不在本路线 |
| SOFA | `saf_sofa_{open,close}` | 919 + libmysofa 5,057 + zlib 11,296 | 否 | 不在本路线 |
| afSTFT | `afSTFT_*` ×6 | 4,793 | 间接 | 不在本路线 |
| 格删相关器 | `latticeDecorrelator_*` ×3 | filters 1,389 + 系数表 3,159 | 否 | 不在本路线 |

另有项目自有的 `src/adm_render_binaural/spreader_mr.c`（fork 自 SAF examples，904 行），内含约 30 处 cblas 直接调用——它不是上游代码，改动不涉及上游同步，是阶段 2 的主战场。

**veclib 的关键观察**：项目只碰到 5 个 veclib 函数，其中 `cvsmul` / `svsmul` / `svvcopy` / `siminv` 分别是标量乘、拷贝、找最小索引，各三行即可自研；**只有 `utility_cseig` 是真 LAPACK**。SAF 的 92 处 BLAS/LAPACK 调用里，项目直接触达的非平凡路径只有这一条（外加 VBAP/HOA/HRIR/afSTFT 内部间接使用）。

## 3. Workspace 布局

```
rust/
├── Cargo.toml              # workspace；[profile.*] 固定确定性开关
├── rust-toolchain.toml     # 钉住工具链版本（可复现构建）
└── crates/
    ├── mradm-math/         # 阶段 1：纯 safe Rust，无 unsafe、无 C ABI
    ├── mradm-math-cabi/    # 阶段 1：staticlib，唯一持有 unsafe / #[no_mangle] / repr(C)
    ├── mradm-vbap/         # 阶段 3
    ├── mradm-vbap-cabi/
    ├── mradm-hoa/          # 阶段 3
    └── mradm-hoa-cabi/
```

划分依据是**接缝形状**而非领域概念（ADR 0008 决策五）。三条具体理由：

1. **不设 `scene` crate**：`AdmScene`（`include/adm/scene.h`，394 行、约 15 个 struct）被 `RenderPlan` 按值持有（`include/adm/render.h:80`）。Rust 侧再建一份等于两个领域模型要同步 + 每次调用 marshal，且场景/时间线/语义既不是一致性问题也不是依赖问题。
2. **不把 DSP 合成一个 crate**：VBAP 与 HOA 的接缝是纯函数（见 §5.3），双耳则涉及 afSTFT + 协方差域合成 + LAPACK 特征分解 + SOFA。合并意味着成熟部分要等最不成熟部分。
3. **不设集中式 `ffi` crate**：见 §4.2——Windows 上 Rust 符号根本无法直接进导出表，集中式 FFI crate 是无目的的间接层，还会因入口点未被 C++ 直接引用而需要 whole-archive。

算法 crate 与 `-cabi` 分离：前者保持纯 safe Rust（`cargo test` 直接可跑），后者隔离全部 `unsafe`；且 `staticlib` crate-type 会拉入完整 Rust std，不适合作为普通依赖复用。

## 4. 构建集成

### 4.1 Cargo ↔ CMake

用 **Corrosion**，经 `cmake/MRDependencies.cmake` 的 `mr_adm_core_find_or_fetch()` 接入（ADR 0004 强制新依赖走该函数）。手写等价物约 200 行 CMake（cargo profile 映射、target dir、增量重建依赖跟踪、MSVC CRT 匹配、IMPORTED 目标生成）且需长期维护。

新增 `cmake/MRRust.cmake`，遵循 `cmake/MRIamfAomBridge.cmake` 的既有范式：`MacinRender::Xxx` 命名 + `mr_adm_core_find_xxx()` 函数封装 + `MR_ADM_XXX_ROOT` cache 变量 + 顶层 `option()` 门控。

新增 `MR_ADM_ENABLE_RUST`（**默认 OFF**）作为整体回滚开关。默认 OFF 同时保证 `MR_ADM_CORE_FETCH_DEPS=OFF` 的离线/发行版打包路径不受影响。

已实测确认的约束：

- **PIC**：`MR_ADM_BUILD_CAPI_BUNDLE=ON` 时全局 `CMAKE_POSITION_INDEPENDENT_CODE ON`（`CMakeLists.txt:28-30`），Rust 侧产物需确认匹配。
- **单配置生成器**：三个 preset（`CMakePresets.json`）与全部 CI 都是 `-G Ninja`；Windows 规范构建树 `build\win-canon` 与 CI Windows job 同样是 Ninja + `cl`。无多配置场景，`CMAKE_BUILD_TYPE` ↔ cargo profile 可一一映射，不需处理 `$<CONFIG>` 子目录。
- **MSVC CRT**：项目未覆盖 `CMAKE_MSVC_RUNTIME_LIBRARY`，用 CMake/MSVC 默认的 `/MD`、`/MDd`，与 Rust `x86_64-pc-windows-msvc` target 默认动态 CRT 一致——但需在集成时显式确认。
- **依赖登记**：Corrosion 须进 `third_party/manifest.json`、SBOM，并通过 `scripts/quality/check-licenses.sh` 的 manifest↔CMake `GIT_TAG` 一致性校验。

### 4.2 符号导出与可见性

`mradm_capi_bundle`（`CMakeLists.txt:626-641`）把 `src/adm_c_api/adm_c_api.cpp` **直接编进 SHARED 目标**，因为 `WINDOWS_EXPORT_ALL_SYMBOLS` 不导出从静态归档传递进来的符号（`:632` 注释已点明）。

推论有两条：

1. **Rust 函数必须由 C++ 包装转调**，不能期待 Rust 符号自动出现在 DLL 导出表。这是 ADR 0008 决策三「C++ 调 Rust」的构建层依据。
2. 因为 C++ 直接引用 Rust 函数，普通链接即可拉取目标文件，**不需要** `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>`（CMake 3.24 恰好支持，若将来方向改变需记得这一点）。

**已知既有缺口**：仓库目前没有任何 `-fvisibility=hidden` / version script / 导出白名单，bundle 会导出全部传递静态库的默认可见性符号。加入 Rust 后 `compiler_builtins` 提供的 `memcpy`/`memset` 等可能与 C 侧冲突。阶段 1 早期实测是否真冲突；补可见性收敛是**独立的既有卫生项**，不作为本路线阻塞项。

### 4.3 CI

插入点已确认：

- macOS/Linux 工具链：`.github/workflows/ci.yml:37`（`brew install`）与 `:40-42`（`apt-get`），或用独立的 `dtolnay/rust-toolchain` step。
- Windows：`:205-217` 定位 MSVC 环境，`:223` 起各步骤 `call "%VCVARS64%"`。cargo 复用同一 shell 环境即可——`link.exe` / `lib.exe` 已在 PATH。
- 缓存：照抄现有 `actions/cache@v5` 模式加一个 key family（如 `cargo-${{ runner.os }}-${{ hashFiles('rust/Cargo.lock') }}`）。`cache-maintenance.yml` 按「family-hash」正则自动纳管，无需改动。

## 5. 阶段

### 5.1 阶段 0：纯 C++ 前置（不碰 Rust）

**必须先做。** 目的是建立「今天三平台差多少、差在哪」的可测量基线——没有它，无法区分 Rust 修复了什么、引入了什么。

1. ~~修 `src/adm_render_ear/ear_renderer.cpp` 的错误注释，改为如实描述平台分支。~~ **已完成**——现 `:93-100`（`DecorrState` 前）与 `:679-680`（`apply_decorrelator` 前）如实记录后端随 `SAF_PERFORMANCE_LIB` 分叉。注意这只修了**描述**，分歧本身要等阶段 1。
2. 全树统一 `-ffp-contract=off`（Clang/GCC）/ `/fp:precise`（MSVC），CI 断言生效。
3. libear 只构建 `ear_default_arch`（关掉 per-arch SIMD 分派）。`cmake/MRDependencies.cmake` 里为 `EIGEN_MPL2_ONLY` 逐个遍历 `BUILDSYSTEM_TARGETS` 的那段可直接复用其目标枚举逻辑。
4. 建立跨平台一致性 CI job（见 §6.1）。**首次运行预期是红的——那就是基线。**

**退出条件**：产出一张「哪个 renderer × 哪个布局在哪两个平台之间不一致」的表。
**回滚**：全部是构建标志与注释，`git revert` 即可。

### 5.2 阶段 1：`mradm-math` + 替换 EAR 的 FFT

最小可验证切片。选此入口的四条理由：它是已确认的 macOS/Windows 分歧源；位于默认 EAR 渲染器；接缝只有 4 个函数且已被隔离在 `DecorrState`（`ear_renderer.cpp:103-123`）；`render_trim_fixture_test.cpp` 已有 EAR **bit-exact** 断言作为现成回归护栏。

`mradm-math` 本阶段内容：

- **`fft`**：实数 FFT，替换 `saf_rfft_*`。必须是标量确定性实现（ADR 0008 决策六）。注意 `saf_rfft_backward` 内部已做 1/N 缩放（`ear_renderer.cpp:707` 注释），替换实现必须保持同一约定，否则增益差 N 倍。
- **`libm`**：确定性超越函数，走 `libm` crate。

**不需要 RNG**。实测渲染路径无随机数：`opus_mka_io.cpp:520` 的 Matroska UID 与 `render_service.cpp:52` / `audio_handles.cpp:44` 的临时文件名是仅有的用例，均无确定性要求。若将来加 dither，再进 `mradm-math`。

**退出条件**：同一 fixture 经 `--renderer ear` 在三平台产出 byte-identical 输出；性能回归经实测确认可接受。
**回滚**：`MR_ADM_ENABLE_RUST=OFF`，C++ 侧走原 `saf_rfft_*` 分支。

### 5.3 阶段 2：`spreader_mr` 去 BLAS + 确定性特征分解

`spreader_mr.c` 是项目自有 fork，改动不涉及上游同步。

- `mradm-math` 增补 `blas1`（`scal` / `axpy` / `copy` / `dot`）与**固定累加顺序**的 `gemm`。`spreader_mr.c` 的 30 处 cblas 调用里绝大多数是 `sscal` / `saxpy` / `scopy`，工作量小；只有几处 `cgemm` 需要认真写。
- `mradm-math` 增补 `linalg::cheev` 替换 `utility_cseig`：用**循环 Jacobi**（确定性，无主元选择歧义），并**规范化特征向量相位**（例如强制最大模分量为正实数）。这比任何 BLAS 都强——它让结果唯一，直接消灭「特征向量符号/相位跨平台不同」这一整类问题。矩阵是 Q×Q 小矩阵，性能不敏感。

**退出条件**：`--renderer saf-binaural` 三平台差异从「有界容差」收敛到 byte-identical 或 1 ULP；`render_trim_fixture_test.cpp:800` 的容差注释可删除或收紧。
**回滚**：保留 `#ifdef` 双路径一个发布周期。

### 5.4 阶段 3：`mradm-vbap` / `mradm-hoa`

两者的接缝都是纯函数、POD 进出、prepare 期只调一次：

- `generateVBAPgainTable3D_srcs(source, n_src, speakers, n_spk, omit_large_triangles, enable_dummies, spread, &out_table, &out_size, &out_simplex)` — `vbap_renderer.cpp:179-196`
- `getLoudspeakerDecoderMtx(dirs, nls, LOUDSPEAKER_DECODER_ALLRAD, order, norm, out_mtx)` — `hoa_renderer.cpp:802`

**HOA 的归一化陷阱**：SAF 内部用 N3D，项目输出 SN3D，现有代码在 `hoa_renderer.cpp:803-807` 逐列乘 `sqrt(2n+1)` 补偿。替换实现要么保持同一约定（输出 N3D 再由调用方补偿），要么直接输出 SN3D 并同步删掉那段补偿——两者必须同时改，不能只改一边。

**退出条件**：各自替换后三平台 byte-identical，且与 SAF 参考实现在既有测试容差内一致。

## 6. 验证

### 6.1 跨平台一致性 CI job

本路线的**目标度量**。三平台各自渲染同一组 fixture，对输出文件取 SHA-256，比对三方结果。

**v1 不引入 golden 基线文件**——直接让三个 runner 的产物互相比对即可暴露分歧，避免过早发明参考文件格式。将来需要固化基线时，按 `CPP_ADM_PLATFORM_REWRITE.md` §10 已规划的 `tests/golden/` + `tests/fixtures/` 目录与字段清单（解析摘要 / 布局摘要 / 时长 / 声道 / 峰值 / 响度 / True Peak / 按 renderer×layout 的误差阈值 / warning 列表）来做，不要另起一套。

覆盖矩阵至少：`ear × 5.1`、`saf`(VBAP) × `5.1`、`hoa × hoa3`、`saf-binaural × binaural`。

考虑只在 push main + 手动触发时跑，避免拖慢 PR 反馈。

### 6.2 进程内 A/B 差分测试

加一个 `MR_ADM_MATH_BACKEND=saf|rust` 开关，让**一个测试二进制**用两条路径渲染同一 fixture 并逐样本 diff。信号最强、不需要 CI 编排。

复用现有设施，**不引入新测试框架**（全仓库无 Catch2/doctest，一律手写 `check()`）：

| 需要的东西 | 复用什么 |
|---|---|
| 读回输出音频 | `mradm::audio::ReaderHandle`（`include/adm/audio_io.h:227-246`），wav/caf/flac 通吃 |
| 两 buffer 比较 + 诊断 | `tests/unit/scene_stream_c_api_test.cpp:1936-1970` 的 `maximum_difference()` / `check_output_equivalence()`，**提炼成共享头** `tests/unit/test_numeric_compare.h`（`tests/unit/test_portable.h` 是既有先例） |
| 两文件逐样本 diff | `tests/unit/render_trim_fixture_test.cpp:729-796` 的 `window_bit_exact()`，已含 sample_index/frame/channel/expected/actual/max_abs_diff 的失败诊断 |
| 参数化 fixture 生成 | `tests/unit/binaural_render_fixture_test.cpp:109-122` 的 `ObjectFixtureOptions` struct + `write_fixture(opts, frames)` 形态 |
| 测试目标注册 | 根 `CMakeLists.txt:696-1055` 的四步样板（`add_executable` + `target_link_libraries` + `mr_adm_core_apply_warnings` + `mr_adm_core_apply_static_analysis` + `add_test`），没有专用宏 |
| Rust 二进制路径注入 | 照抄 `CMakeLists.txt:1006-1011` 的 `MRADM_EXE_PATH` 模式（`if(TARGET ...)` 防御 + `target_compile_definitions` + `add_dependencies`） |

**容差默认 0（bit-exact）**。这是全仓库唯一被严格遵守的纪律：只在定位到具体非确定性来源时才放宽，并在代码注释里写明原因。

### 6.3 Rust 侧单元测试

`cargo test` 覆盖 `mradm-math` 各函数对已知解析解的一致性：FFT 对 DFT 直接定义、Jacobi 对已知特征值矩阵、`libm` 对标准值表。

### 6.4 本地手工验证

```bash
cmake --preset release -DMR_ADM_ENABLE_RUST=ON
cmake --build --preset release
ctest --preset release --output-on-failure

# 同一输入在三平台跑，比对哈希
./build/release/mradm render -i fixture.wav -o out.ear.flac --output-layout 5.1
shasum -a 256 out.ear.flac
```

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 标量 FFT 性能不足 | 离线渲染变慢，实时监听可能掉帧 | 阶段 1 必须实测。加速只走跨声道 SIMD，不动蝶形内顺序。若不可接受需回 ADR 重新权衡确定性 vs 性能 |
| Rust std 符号与 bundle 内 C 符号冲突 | Windows/ELF 链接失败或运行期错乱 | 阶段 1 早期实测；可见性收敛作为独立项 |
| MSVC CRT 不匹配 | Windows 链接失败 | 默认 `/MD` 与 Rust windows-msvc target 默认一致，但需显式确认 |
| Corrosion 引入新构建期依赖 | 与离线/发行版打包路径张力 | `MR_ADM_ENABLE_RUST` 默认 OFF；走 `mr_adm_core_find_or_fetch` + 登记 manifest/SBOM/license check |
| `spreader_mr.c` 去 BLAS 后性能回归 | `cgemm` 在热路径 | 保留 `#ifdef` 双路径一个周期，实测后再删 |
| 阶段 0 暴露的分歧点多于预期 | 后续排期变长 | 这正是阶段 0 先行的目的；分歧表产出后重排优先级 |
| CI 时间增加 | 反馈变慢 | cargo 缓存照抄现有模式；一致性 job 只在 push main + 手动触发时跑 |

## 8. 后续方向（不在本路线内）

以下都需要单独 ADR 决策，不因本路线自动获批：

- **BW64/RF64 容器自研**（替 `libbw64` + `dr_wav` + `dr_flac`）：风险最低、能一次干掉 3 个依赖，并顺带修掉 `libbw64` `seek(int32_t)` 2³¹ 帧上限的 workaround（现由 `render_common::seek_reader_abs` 兜住）。不在本路线只因它不是 SAF 问题。
- **`ebur128` / `rubato` / `cpal` 三换三**：分别替 `libebur128` / `libsamplerate` / `miniaudio`，均有成熟纯 Rust 替代。
- **CLI 迁 Rust**（ADR 0002 点名的第一入口）：可干掉 CLI11 + spdlog + fmt + nlohmann + tl-expected。
- **SOFA 读取**：为读 SOFA 拖进 libmysofa + zlib（约 16K 行）是全树依赖比最差的一处，值得单独评估。
- **双耳 afSTFT + spreader 全替换**：风险最高、收益最低，最后再碰。
- **`ADM_RENDERER_SAF` 等 ABI 名字的处置**：SAF 实际被替换后这些名字变成历史遗留命名，需在 ADR 0007 框架下决定保留还是 deprecate。
