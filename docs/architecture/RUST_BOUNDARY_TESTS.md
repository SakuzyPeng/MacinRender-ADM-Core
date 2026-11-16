# C++ 测试移植为 Rust（R3）

> 状态：进行中。A、B、C 已完成（2026-11-15），B、C 已通过 macOS arm64 / Windows x64 原生复核；D 已完成（2026-11-16，本机 Linux x64），原生复核待维护者；E–G 待做。
> 依据：[用户决定 2026-11-14](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)（R3 由七期提前到 6.6 之前）与下文的用户决定 2026-11-15。

R2 之后，C++ 测试代码共 57,767 行（`tests/unit` 38,055、`tests/support` 14,734、`tests/tools` 2,553、`tests/manual` 2,425）。
其中大部分驱动只为测试留下的 C++ SDK（`tests/support/sdk`），或经私有 FFI 调用早已有 Rust 测试的内核。R3 的目标：
- 把测试覆盖的行为移植为各 crate 的 Rust 测试；
- C ABI 只保留一个精简的 C 消费者测试；
- 删除 `tests/support` 的外壳与 SDK，以及 `mradm-ffi` 中只供测试的 `extern "C"` 入口。

## 用户决定（2026-11-15）

1. **依赖 C++ 岛的测试暂留 C++**（FLAC / Opus / APAC / IAMF 编码、Apple 后端、设备岛、FLAC 读取）。它们改为只经 C ABI 共享库或岛的
   窄 C 接口驱动，不再依赖 SDK。6.6 的 `-sys` crate 让 Cargo 构建岛之后，再移植为 Rust。
2. **一致性门禁用的 C++ 工具移植为 Rust**，新工具的输出须与 C++ 版逐字节相同；切换脚本与 CI 后删除 C++ 版。
3. **依赖 SDK 的手动探针与基准移植为 Rust example**，同步更新引用它们的文档。只依赖 `c_api.h` 的 `scene_stream_stress`、
   `system_spatial_monitor_check` 保持 C 消费者不动。
4. **C ABI 对照工具保留到 6.6 验收后再删**（`mr_adm_capi_capture` + `capi-capture.py`、`cli-transcript.py`）。6.6 换 cdylib 时用于 A/B
   对照，6.7 结项时删除。

## 方法

逐个 C++ 测试文件处理，二选一：
- **已覆盖**：注明覆盖它的 Rust 测试或门禁项，然后删除。
- **未覆盖**：把缺的断言移植到对应 crate 的 `tests/`。

不为测试在 C ABI 或 `mradm-ffi` 中新增导出。fixture 在运行时由代码生成。分批顺序为 A 工具与 fixture、B 内核、C SDK 渲染、D 实时、
E C ABI 与 CLI、F 岛测试去 SDK、G 手动探针与收尾。

## A 测试工具与 fixture

新 crate `mradm-testkit`（`publish = false`，`#![forbid(unsafe_code)]`，无依赖）：
- `adm`：ADM XML 编写器，移植自 `tests/support/adm_fixture.h`。
- `wave`：RIFF / PCM 编写器，移植自 `wav_fixture.h`。
- `fixtures`：一致性矩阵的 12 种 fixture，移植自 `make_fixture.cpp`。

两个编写器不依赖生产 crate，fixture 因此不会继承被测代码的缺陷。它们照原样复现 C++ 的输出：
- 数值按 `%.17g`（例如 50 ms 插值写成 `0.050000000000000003`）；
- 属性按 `std::map` 的键序；
- 元素按引用图的加入顺序；
- 24 位量化按截断取整。

后续批次的 Rust 测试共用这些生成器。

| 工具（CMake 目标名不变） | 原 C++ | 现在 | 说明 |
|---|---|---|---|
| `mr_adm_make_fixture` | `tests/tools/make_fixture.cpp` | `mradm-testkit` example `make_fixture` | |
| `mr_adm_pcm_bits` | `tests/tools/pcm_bits.cpp` | `mradm-testkit` example `pcm_bits` | 只读 WAV / CAF（`collect.py` 与各脚本只用这两种）；FLAC 读取见下 |
| `mr_adm_fft_benchmark` | `tests/tools/fft_benchmark.cpp` | `mradm-testkit` example `fft_benchmark` | 直接调用 `mradm_dsp::fft::RealFft`，即原 `mradm_dsp_fft_*` 包装的同一计划；JSON 经 `mradm-json`（与 nlohmann `dump(2)` 字节一致） |
| `mr_adm_repeat_render` | `tests/tools/repeat_render.cpp`（经 C++ `RenderService`） | `mradm-ffi` example `repeat_render`（引擎 `Render` 加 `FfiHost`） | 请求默认值与 `consistency_render` 共用 `examples/consistency/request.rs` |
| `mr_adm_render_common_numeric_tests` | `tests/unit/render_common_numeric_test.cpp` | `mradm-dsp` `tests/scene_math.rs` 的 `vector_lengths_are_correctly_rounded` | Consistency workflow 改为在各平台以 `cargo test --release` 运行 |

这些 example 都在 Release 下构建，再按原名复制到构建目录（`CMakeLists.txt` 中的 `foreach`），因此 `collect.py`、`render-matrix.sh`、
`benchmark-fft.py` 与 workflow 照旧按原名调用。`mr_adm_pcm_bits` 仍默认构建，供 CTest 的工具测试使用。

**FLAC 读取**：`flac_reader_test.py` 测的是生产的 FLAC 读取岛（先经 Rust 严格校验，再由 dr_flac 解出 PCM），原先借用
`mr_adm_pcm_bits`。按决定 1 改用新的 C++ 探针 `mr_adm_flac_reader_probe`（`tests/unit/flac_reader_probe.cpp`）。它只调用
`FloatFlacReader`，写出同样的规范图像；Windows 的 UTF-8 活动代码页清单随之改名为 `flac_reader_probe.manifest`。

**行为变化（只涉及工具）**：
- `mr_adm_pcm_bits extract` 不再接受 FLAC。
- `mr_adm_repeat_render --reset-rng` 只为兼容旧脚本而保留，不再起作用：它原本重置 C 运行时的 RNG，而渲染器早已不用它。
- `hardware_concurrency=` 改报 `std::thread::available_parallelism`，会计入 CPU 亲和性。

### A 的验证（对照 C++ 版，同一提交、同一构建）

- **fixture**：12 种 fixture 的文件、`wrote ...` 输出、用法与错误输出逐字节相同。
  `mradm-testkit` 的 `fixtures_golden` 用 C++ 生成器的长度与 FNV-1a 64 摘要锁定这 12 个文件。
- **`pcm_bits`**：7 个输入（f32 / i24 / i16 WAV、CAF 与三个 fixture）的 `extract` 图像与输出逐字节相同；`compare` 在相同、
  形状不同、数值不同三种情况下的输出与退出码相同。`consistency_tools_test.py` 19 项通过。
- **`fft_benchmark`**：除计时外 JSON 逐字节相同，包括 schema、长度、迭代数与两组位指纹。
- **`repeat_render`**：`objects-extent-multi` 在 spreader 与 `--cloud` 下两遍的 PCM 位图像相同，日志除输出路径外相同。
- **`flac_reader_test.py`**：经新探针 72 项通过。
- **全矩阵**：在参照 worktree（`b89687c`）的构建目录换入三个 Rust 工具（`make_fixture`、`pcm_bits`、`repeat_render`），`mradm` 用候选构建，
  跑完整采集后与原参照采集对照，结果完全相同：294 个输出、220 个 fixture、989 项比较、176 个重放、256 个产物、666 份日志，
  其中包括 fixture 生成、PCM 提取与 worker 实验的日志。对照前只在副本里把清单中这三个工具的二进制摘要换成参照值，
  因为 `compare-collections.py` 只允许替换 `mradm` 与重放工具；比较的内容本身没有改动。
- **其他**：ctest 73 项通过（原 74 项，向量长度测试并入 `mr_adm_rust_unit_tests`）。许可证检查、`mr_adm_rust_quality`、FFI 头校验与
  C/C++ 棘轮通过。

**规模**：测试 C/C++ 由 57,767 行降到 56,619 行（`tests/tools` 2,553 → 1,435，剩 `capi_capture.cpp`；`tests/unit` 38,055 → 38,025）。
生产 C/C++ 不变（9,432 行）。

## B 内核测试

B 批处理经私有 FFI 直接测 Rust 内核的 C++ 测试。逐文件读出断言，与对应 crate 的 Rust 测试核对：
- 已覆盖的，注明覆盖它的 Rust 测试或 golden 后删除；
- 有缺口的，移植到 crate 的 `tests/`，直接调用 Rust 类型。

断言若以测试支持里的 C++ 旧实现副本为期望（`legacy_state.cpp` 的 HRTF 状态、`SizeFilterBank`、语义策略的 C++ 算术），
不移植副本本身，改为独立推导的期望（插值 oracle、三维中点积分）、捕获文件或已有门禁。

| C++ 测试 | 去向 |
|---|---|
| hptf_eq_test | 解析、bypass 恒等、热切换淡化、frames=0 已由 `hptf_parse_golden.rs`、`parametric_eq` 单元测试与 `hptf.rs`（直接型 I 对照的精确淡化公式）覆盖；移植 MDR-MV1 绝对响应与运行滤波器对照、OFF 段、warn-only / auto-trim、20 Hz Q8 长跑 → `hptf_regression.rs` |
| hptf_eq_regression_test | 越界与非法参数已由 `hptf.rs` `design_anchors_and_parameter_contracts` 覆盖；移植纯 preamp、reset 优先级与元数据、窄峰 / 重叠峰 / 离网格 auto-trim、量化低频峰、并发查询 → `hptf_regression.rs` |
| hptf_dsp_test | 移植精确淡化值与排队目标起点、7 种段型 × 6 采样率 × 2 模式的分块无关 → `hptf_regression.rs`；capture 模式只供已完成的迁移对照，随 `scripts/consistency/compare-hptf-dsp.py` 删除 |
| meter_test | **改归岛测试，保留 C++**：测的是生产 C++ 包装 `src/adm_dsp/meter.h`（Apple 岛使用）的所有权转移、空指针与错误码映射，不依赖 SDK；内核行为另有 `mradm-dsp` `meter` 单元测试与 `meter_acceptance.rs` |
| resampler_test | 已覆盖：`resampler.rs`（含 44101 Hz、精确输出长度、声道隔离、非法输入不污染历史、EOS 之后拒绝）；FFI 创建失败清空输出一项随 `mradm_dsp_resampler_*` 入口删除 |
| hrtf_grid_test | 移植 KEMAR 与带假极点的部分球面网格的维度、权重有限且归一、索引越界、重复构建结果确定，以及非有限方向被拒绝 → `mradm-dsp` `hrtf_kemar.rs`；几何缓存已由 `mradm-render` `binaural_hrtf` 覆盖 |
| hrtf_interpolation_test | 原先与 4.7 A 之前的 C++ HRTF 状态副本（`legacy_state`）对照。现改为独立的查表规则 oracle（1° 网格双线性 + 保幅三方向混合），对离线与实时两种存储布局、相同的边界与随机点逐项比较 → `hrtf_kemar.rs`；存储差恰为幅度缓存、畸形输入被拒绝也一并移植 |
| sofa_fixture_test | 维度、采样率、名称、未归一化样本、采样率不符、畸形 / 缺失文件与 SOFA 关闭已由 `mradm-render` `binaural_hrtf` 单元测试覆盖；移植 Unicode 路径（两种准备流程的频谱与名称相同）、UTF-8 错误上下文与 512 帧块的 FFT 布局 → `mradm-render` `binaural_hrtf_sofa.rs` |
| ear_post_test | 已覆盖：`mradm-dsp` `ear_post.rs` 的独立 double 时域 FIR 对照（不规则分块、各种容量）；声道数扩到与 C++ 相同的 12、24 |
| pcm_mix_test | 移植「被对象结束裁剪成零长度的块保持静音，但仍是下一块的插值前驱」，16 个精确样本、三种分块 → `pcm_mix.rs` `clipped_blocks_stay_silent_and_keep_the_interpolation_history`；后端的裁剪本身已由 `mradm-render` `vbap.rs` / `ear.rs` 覆盖 |
| triple_balance_222_test | 节点 one-hot、分块 / reset / 快照、床的 LFE 路由已由 `triple_balance.rs`、`triple_balance_d.rs` 与 `bed` 单元测试覆盖；移植 9×9×9 × 7 种 size 网格的对称（2e-6）与功率（3e-6）、径向依赖、层间连续、size 积分与独立三维中点积分对照，以及 22.2 size 对象的 LFE 不泄漏、静音尾在文件内复位、不同对象交错互不影响 → `triple_balance_regression.rs` |
| triple_balance_point_test | 移植留出参考库的 7.1.4 / 9.1.6 点增益（相对误差 ≤ 1%）与越界拒绝 → `triple_balance_regression.rs`；能力表一项由 `mradm-engine` `catalog.rs` 的 golden 覆盖 |
| triple_balance_semantics_test | 移植「首个事件晚到」与 Renderer 捕获 PCM 的对照（7.1.4 / 9.1.6，`tests/fixtures/room_compat_semantics`）及分块无关 → `triple_balance_regression.rs` |
| triple_balance_size_processor_test | 移植捕获的空间增益（`reference.json`）、去相关滤波器捕获、零 size 快速复位捕获、分块 / reset / 实例独立、采样率与倒序元数据拒绝、稠密初始元数据 → `triple_balance_regression.rs`；未验证布局的拒绝在 Rust 中由 `Layout` 类型保证 |
| triple_balance_size_filter_test | 删除，不移植：测的是只存在于测试支持里的 C++ `SizeFilterBank`（头文件注明「从未链接进生产渲染器」） |
| triple_balance_size_probe | 改为 `mradm-dsp` example `triple_balance_size_probe`（同样的请求 JSON 与 stdin 两种模式）；stdin 每读完一组参数就输出并 flush，不等待 EOF，保留跨行参数与未完成末组的处理 |
| live_binaural_continuity_test | 全 FIR 与分块、重定向沿用输入历史、OLA 与 diffuse delay 已由 `mradm-dsp` `binaural_convolution.rs` 覆盖，连续查表由 `hrtf_filters.rs` 与 `hrtf_kemar.rs` 覆盖；移植真实 HRTF 下的头锁定不随头动、头 / 对象 / 增益运动无宽带跳变、离网格方向与分块无关 → `mradm-engine` `scene_binaural_motion.rs`，并保留 `MR_ADM_TEST_SOFA` 对外部数据集重复三组检查的入口 |
| live_triple_balance_test | 已覆盖：`mradm-render` `live_triple_balance.rs`（创建、代次与床检查、乱序床的固定路由与 LFE 分配、不支持的状态与 `element=` 上下文、拒绝的原子性、描述符位置与初始状态、spread=none 只报一次、分块无关）与 `mradm-dsp` `live_triple_balance.rs` `level_changes_do_not_restart_spatial_ramps_or_filter_history` |
| room_profile_allocation_test | 删除，不移植：测的是 SDK 包装 `room_internal::Access::adopt` 在 C++ `operator new` 失败时释放已接管的 Rust 房间句柄；生产中房间句柄只由 Rust 持有（`mradm-capi` `options::Room` 经 `Drop` 释放），不再有跨语言接管 |
| semantic_policy_test | 已覆盖：断言对照的是 C++ 算术副本；Rust 侧有 `mradm-dsp` `semantic_policy` 单元测试、`mradm-scene` 策略 golden 与门禁矩阵的策略用例；dB 换算表补入 −20.5 dB 一项 |
| speaker_layouts_test | **保留 C++ 岛测试，去掉 SDK 部分**：布局视图 `speaker_layouts.cpp` 与 `resolve_live_head_locked` 是生产代码（Apple / Windows 岛使用）；只在测试支持里的 DS 标签别名一节删除，已由 `mradm-render` `routing.rs` golden（`index` / `position` 两类共 2,355 例）覆盖；测试不再链接 `mr_adm_scene_test_support` |
| direct_speakers_matrix_test | 已覆盖：被测的解析与目标解析只在 SDK / 测试支持中；系数与严格 schema 错误由 `mradm-scene` `config.rs` golden（491 个矩阵用例，增益按位比较）覆盖，别名解析、重复源 / 目标、LFE、歧义与多行匹配由 `mradm-render` `routing.rs` golden（412 个矩阵用例）覆盖 |
| config_json_compat_test | 移植末尾 NUL、错误消息中的内嵌 NUL、1024 / 10000 位数字溢出 → `mradm-scene` `config.rs` `terminal_nul_embedded_nul_and_long_overflow`；1 MiB 上限已在 golden 中 |

新增的 Rust 测试：
- `mradm-dsp`：`hptf_regression.rs`（10 项）、`hrtf_kemar.rs`（4 项）、`triple_balance_regression.rs`（6 项），以及 `pcm_mix.rs`
  的被裁剪块一项；`ear_post.rs` 的声道数扩到 12、24；`semantic_policy` 的 dB 表补一项。
- `mradm-render`：`binaural_hrtf_sofa.rs`（SOFA 开、关两种构建都通过）。
- `mradm-engine`：`scene_binaural_motion.rs`（4 项，含外部 SOFA 入口拒绝无效数据集的回归）。
- `mradm-scene`：`config.rs` 的 `terminal_nul_embedded_nul_and_long_overflow`。
- example：`mradm-dsp` `triple_balance_size_probe`，2 项测试随 `cargo test --workspace` 运行，检查管道未关闭时的逐组响应、空白分隔与 EOF 行为。

`mradm-dsp` 新增两个只供测试的 dev-dependency：`mradm-json`（读 `reference.json`）与 `mradm-testkit`，都是项目内 crate。

**保留的 C++**：`meter_test` 测的是生产 C++ 包装 `src/adm_dsp/meter.h`（Apple 岛使用），按决定 1 保留。`speaker_layouts_test`
同样保留，但删去只在测试支持中的一节，不再链接 `mr_adm_scene_test_support`。

**随批删除的测试支持**（B 之后没有使用者）：
- `tests/support/triple_balance/`：整个目录及库 `mr_adm_triple_balance_test_support`。
- `triple_balance_size_filter.*`、`binaural/live_binaural_convolver.h`、`dsp/resampler.h`、`sdk/dsp/semantic_policy_ffi.h`、
  `tests/unit/ear_post.h`。
- B 之前就已没有使用者的 `post_gain/` 与 `dsp/scene_transition*.h`。
- 只为 `mr_adm_hptf_dsp_tests` 的 capture 模式存在的 `scripts/consistency/compare-hptf-dsp.py`。

仍有使用者的支持留到对应批次：`binaural/hrtf_state.*`、`binaural_internal.h`、`legacy_state.cpp`（C 批的 `binaural_test_probe`），
`dsp/hptf_cascade.*`（D 批），以及 `live_scene/live_scene_trace.*`（D 批）。

**`mradm-ffi` 入口**：391 → 327。删除失去全部 C/C++ 调用者的 7 个模块，共 64 个 `extern "C"` 入口：
`triple_balance`（34）、`scene_transition`（7）、`ear_post`（6）、`post_gain`（6）、`resampler`（4）、`spreader`（4）、`semantic_policy`（3）。
其中 `post_gain`、`scene_transition`、`spreader` 在 B 之前就已没有 C/C++ 调用者。同步删除 `dsp_test_ffi.h` 中的声明，并从
`check-ffi-headers.py` 的清单中去掉删除的头。

另有 5 个没有 C/C++ 调用者的入口留到 G：`mradm_dsp_hoa_coefficients`、`mradm_dsp_hoa_matrix`、`mradm_dsp_live_vbap_snapshot`、
`mradm_dsp_scene_pose`、`mradm_dsp_scene_rotate_pose`。它们所在模块的其余入口仍被调用，`mradm-ffi` 自身的单元测试也调用其中三个。

`check-ffi-headers.py` 的数组别名表随 `mradm_dsp_tb_filter_process` 清空。机制本身保留，`quality_scripts_test.py` 的 `FfiArrayTests`
在测试中自行登记该项。

**文档与 CI**：
- `release.yml` 的 Windows 优化构建测试集去掉 `mr_adm_sofa_fixture_tests`。SOFA 的 Unicode 路径改由 Rust 测试覆盖：
  `cargo test --workspace` 经 `mradm-ffi` 的默认特性统一开启 `mradm-render/sofa`，在各平台 debug job 中运行。
- `hptf-eq.md`、`live-binaural-convolution.md`、`TRIPLE_BALANCE_RENDERER.md`、`CI.md` 改为指向 Rust 测试。
- 迁移记录与 closeout 摘要中的旧测试名属于历史，不改。

生产代码不变，没有行为变化。

### B 的验证

- **Rust**：新增与改动的测试全部通过；`mr_adm_rust_quality`（rustfmt、clippy `-D warnings`）通过。`binaural_hrtf_sofa` 在 SOFA 开、关两种构建下都通过。
- **C++ 侧**：debug 构建通过，ctest 53 项通过（原 73 项，删去 20 项；size 探针原本不在 ctest 中）。FFI 头校验通过（327 个函数），
  `quality_scripts_test.py`、许可证检查与 C/C++ 棘轮通过。`speaker_layouts_test.cpp` 通过 clang-format 与 clang-tidy；本机没有 cppcheck。
- **对照参照构建 `b89687c`**（方法同 A，候选为删除入口之后的 `consistency-a`）：
  - C ABI 采集：3,283 行、36 个文件，0 差异；
  - CLI 记录：113 条命令，0 差异；
  - C 消费者测试 2 项通过；
  - 共享库恰好导出 164 个 `adm_*`；
  - 完整 PCM 采集与参照完全相同：294 个输出、220 个 fixture、989 项比较、176 个重放、256 个产物、666 份日志。
    采集时与 A 一样换入三个 Rust 工具，并在清单中把它们的摘要换成参照值。

**规模**：测试 C/C++ 由 56,619 行降到 51,580 行（`tests/unit` 38,025 → 34,330，`tests/support` 14,734 → 13,390）；
`mradm-ffi` 由 25,838 行降到 23,751 行。生产 C/C++ 不变（9,432 行）。

### B 的 macOS / Windows 原生复核（2026-11-15）

验证源码为 `4be5d6da`，包含 B 的迁移与审查修复。两项新增回归均在修复前失败，修复后通过。

- macOS arm64 的 `cmake --preset debug`、`cmake --build --preset debug` 与完整并行 CTest 通过，60/60 项成功，
  包含 workspace Rust 测试与 size 探针的管道交互测试；探针可执行文件也验证了输入尚未关闭时连续返回两组结果。
- Windows x64 复用规范 MSVC/Ninja Release 构建与 Cargo 缓存，SOFA/IAMF 关闭，完整并行 CTest 通过，58/58 项成功。
  随后在同一 Cargo 缓存启用 `mradm-render/sofa`，`scene_binaural_motion` 与 `binaural_hrtf_sofa` 定向测试通过。
- 两平台均通过 `MR_ADM_TEST_SOFA` 对 `simple.sofa` 与 `general-compressed.sofa` 重复头锁定、运动连续性和分块检查。
  无效外部路径会使运动检查失败，Unicode SOFA 路径测试通过。
- macOS 的 rustfmt、Clippy `-D warnings`、34 项质量脚本测试、FFI 头校验、许可证与 C/C++ 棘轮检查通过。
  macOS 与 Windows 的 C ABI 共享库均恰好导出 164 个 `adm_*` 函数。

## C SDK 渲染测试

C 处理经 C++ SDK（`RenderService` / `PreviewSession`、`tests/support/engine_renderer` 的 `IRenderer` 适配、`io::import_scene` 与
`adm_scene`）做整段渲染的 18 个测试。Rust 后端的检查改在 `cargo test` 中运行，Apple 岛的大文件裁剪检查经公开 C ABI 运行。分四个子批提交：
C1 扬声器后端、C2 双耳 / Triple Balance 流 / room、C3 导入导出与声道床、C4 渲染服务与后处理。

**整段渲染的测试工具** `mradm-engine` `tests/common/render.rs`：
- 用 `mradm-testkit` 写 ADM WAVE（单对象、多 track、嵌套对象、DS 与 HOA 块）；
- 经 `mradm_engine::render::Render` 与 `TestHost` 渲染，请求默认值照 C++ `RenderOptions`，输出 float32 以便读出精确值；
- 任何岛请求都让测试失败，保证不经编码器或 Apple 岛；
- 用 `mradm-wav` 读回输出，并收集服务的日志行（检查警告）。

导入与导出的测试读后端收到的场景：inspect JSON（`scene_json::inspect_json`，即 C++ 测试读的 `AdmScene` 视图）加快照中的作者来源。
只在某个后端内部的断言放到 `mradm-render` 的测试。

`room_runtime.rs` 的切换检查在 Scene 流式渲染中用零帧处理将候选送进等待队列，再提交失败的新请求，
随后推进活动过渡并确认旧候选保持失效。`render_service.rs` 中的 `Type` 导入与稀疏文件检查同样限定为 Unix。

| C++ 测试 | 去向 |
|---|---|
| vbap_smoke_test | 移植为 `mradm-engine` `vbap_render.rs`（19 项，经渲染服务整段渲染）：对象位置、块切换与插值钳位、嵌套对象起点、对象增益 / 静音 / 时长 / 偏移、DS 标签 / 别名 / 位置 / 回退路由（stereo、5.1、wav71、2+5+0、4+5+4、9.1.6、22.2）、显式 DS 路由在其他后端被拒、无坐标后置标签、channelLock / maxDistance / divergence、MDAP、默认插值、Apple 几何、2D 布局警告、Triple Balance 块增益与语义报告；实时增益覆盖的线性比例 → `mradm-render` `vbap.rs` `a_live_gain_override_scales_the_stream`；文件流式等于离线窗口与分块无关已由 `vbap.rs` `seeks_follow_the_offline_window` 等覆盖；能力表由 `catalog.rs` golden 覆盖 |
| ear_render_fixture_test | 移植为 `mradm-engine` `ear_render.rs`（9 项）：前方对象、DS 床与混合场景、9.1.6 对象与顶层标签、RC_LFE 别名、Cartesian 对象与扬声器位置、位置偏移、diffuse 与 255 帧补偿延迟、短输入、P2 语义的警告、channelLock / divergence、同一窗口内两个跳变块；整体与按标签的实时增益 → `mradm-render` `ear.rs` `live_gains_scale_the_whole_object_or_one_label`；文件流式与窗口等于整段离线已由 `ear.rs` 覆盖；C++ 岛辅助 `resolve_live_channel_gain` 与 `LiveGainRamp`（生产 `render_common`，Apple 岛使用）的断言移到 C++ 岛测试 `speaker_layouts_test` |
| ear_render_smoke_test | 已覆盖：能力表由 `catalog.rs` golden 覆盖（含 wav71、无 0+7+0）；缺失输入的 io_error 与非空消息 → `ear_render.rs` `a_missing_input_is_an_io_error_with_a_message` |
| hoa_encode_fixture_test | 移植为 `mradm-engine` `hoa_render.rs`（6 项）：前 / 左 / 偏移声源的一阶分量、对象静音与增益、跳变与插值块切换、diffuse / extent / divergence 降低指向性、DS 位置与 RC_LFE、只有 LFE 时的 True Peak 与无响度；流的缓冲规则（过小缓冲不消耗也不改写、零帧）、整体增益、静音与标签优先级 → `mradm-render` `hoa.rs` `stream_buffers_and_live_gains`；窗口与文件流式等于整段离线、seek 不留 diffuse 历史已由 `hoa.rs` 覆盖，prepared / 输入不匹配由 `stream.rs` 覆盖，能力表由 `catalog.rs` 覆盖 |
| hoa_decode_fixture_test | 移植为 `mradm-engine` `hoa_decode_render.rs`（4 项）：HOA1 的 W 到 5.1 与 9.1.6、对象静音与增益、增益为 0 的后一块在补偿延迟后静音、无 track 的 UID 不产生 W |
| binaural_render_fixture_test | 移植为 `mradm-engine` `binaural_render.rs`（9 项，经渲染服务整段渲染）：偏离中心声源的两耳差、块起点之前不泄漏、LFE 绕过 HRTF、CAF 双耳布局标签与多声道布局被拒、缺失 SOFA、channelLock / divergence / extent / diffuse、点块之后的 divergence 块保持对称、SAF spreader 保持输入长度、离线朝向与 ADM 头锁定（含 spreader 与先自由后锁定的块）；文件流式的缺口 → `mradm-render` `binaural_stream.rs`（20 ms 线性增益、拓扑交叉淡化从旧状态开始且还原后逐位相同、实时 extent 缩放等于按 ADM 范围钳位后的块、头锁定与只改增益 / 显式解锁 / cloud 与 diffuse 分支、姿态扫动有界）；拉取大小、seek、放弃的 seek、拓扑合并与按标签增益已由 `binaural_stream.rs` 覆盖；原先经 `binaural_test_probe`（4.7 A 之前的 C++ HRTF 状态副本）检查的网格点恒等、离网格幅度不出凸包、侧向头影改为对内置 KEMAR 的独立断言 → `mradm-dsp` `hrtf_kemar.rs` `kemar_lookups_reproduce_bound_and_shadow_the_measurements`；外部 SOFA（`MR_ADM_TEST_SOFA_PATH`）一项不移植，SOFA 读取由 `binaural_hrtf_sofa.rs` 覆盖 |
| triple_balance_stream_test | 标准模式的流（拉取模式与 seek 等于离线、取消的 seek、空处理与过短缓冲、静态编辑等于策略离线渲染、拒绝时保留已接受目标、实时静音与 size 自动化与拉取大小无关）已由 `mradm-render` `triple_balance_stream.rs` 覆盖；D 模式移植为 `mradm-engine` `triple_balance_render.rs`（2 项，同一 13 声道场景经渲染服务）：起始静音、有效报告、22.2 LFE 等功率分配、裁剪窗口、重复渲染、spread none 忽略 size、布局与采样率边界；D 模式拒绝实时由 `monitor-v1` golden 覆盖 |
| room_render_test | **缩为 C ABI 测试，去掉 SDK**：Scene 流、房间检查与两种档位、距离集合与 SOFA 检查、损坏的包、选项设置器、显式 HRIR 与 SOFA 冲突，只经 `c_api.h`（E 批再移植）；文件渲染（离线追加尾部且等于碎片化文件流、seek、窗口、实时静音、屏幕映射，两种档位、距离集合、靠墙房间与 SOFA）→ `mradm-engine` `room_render.rs`；切换规则（失败候选保留当前房间、最新请求胜出、重复请求不重启过渡、失败的最新请求不复活被取代的房间）、声源增长与 reset → `mradm-render` `room_runtime.rs` 三项；屏幕 JSON 解析由 `mradm-scene` `config.rs` 覆盖，切换不能改档位由 `room_runtime.rs` 覆盖 |
| adm_io_fixture_test | 移植为 `mradm-engine` `adm_import.rs`（14 项，读后端收到的场景：inspect JSON 即 C++ 读的 `AdmScene` 视图，作者来源读快照）：最小图与 CHNA 映射、对象与块的来源（dB、相对 rtime）、块位置 / 增益 / 默认值、分数帧边界连续、channelLock / divergence 及默认值、DS 块、三种类型的头锁定优先级、HOA pack 属性、混合对象、按 track 限定的 pack 声道与大小写不同的 CHNA UID、Binaural 警告、LFE 低通、programme / content 响度、语言、标签、对白、参考屏幕、programme 时间、importance 与 dialogue ID |
| adm_io_smoke_test | 缺失输入、mask 自动识别、显式布局与 AXML 优先级、格式错误已由 `mradm-scene` `import.rs` 覆盖；移植 7.1.4 两种顺序、别名几何与 LFE、带仰角的上层标签、空标签被拒、自定义 float 床（一个对象每声道一 track、LFE 只有低通标记）、预设声道数不符、格式错误的 AXML 不退回 mask → `import.rs` `catalog_aliases_custom_beds_and_malformed_axml` |
| scene_export_fixture_test | 移植为 `mradm-engine` `scene_export.rs`（3 项，照 C ABI 的导出：`patched_axml` 后 `replace_axml`，再重新导入）：无策略时块字段不变、增益缩放只改对象增益、插值上限只改插值、位置覆盖延后、头锁定覆盖写到每种类型的每个块而对象保持、data chunk 逐字节相同、BW64 源仍为 BW64；各策略写出的 AXML 由 `mradm-scene` `export-v1` golden 锁定 |
| channel_bed_render_test | 移植为 `mradm-engine` `channel_bed_render.rs`（5 项，经渲染服务）：扬声器输出的 WAVE mask、自定义标签床到双耳（ADM Binaural 语义、无扬声器 mask、重新导入）、7.1.4 WAVE 顺序改写保持各声道信号、8 声道以上整数输出为 BW64 且带 DirectSpeakers ADM、HOA3 的 ACN / SN3D pack |
| render_trim_fixture_test | 移植为 `mradm-engine` `render_service.rs`：结束 / 起止 / 无裁剪的帧数、巨大有限结束钳到整段、起点超出时长与 inf / -inf / NaN / 负值 / 结束不在起点之后 / 不足一帧的窗口被拒且不留输出、计量跟随窗口（EAR 与 HOA3 静音半段无 True Peak）、窗口逐位等于整段切片（EAR、VBAP、HOA3，双耳 1e-6）、稀疏 BW64 输入在 2^31 帧之后的 6 对象窗口（只在 Unix 运行；C++ 在 Windows 也只在设置 `MR_ADM_RUN_LARGE_FILE_TESTS` 时运行）；Apple 后端的两种布局窗口由 `apple_render_trim_test.py` 经公开 C ABI 共享库检查，注册为 macOS 的 `mr_adm_apple_render_trim_tests` |
| core_smoke_test | **岛测试，保留**：去掉 SDK 的版本与 `RenderService` 检查（版本由 C ABI 测试与 `mradm --version` 的 CLI 记录覆盖，渲染服务由 `render_service.rs` 覆盖），只留 `src/adm_audio` 的 WAVE / CAF 元数据与写出包装；SDK `version.h` / `version.cpp.in` 随之删除 |
| render_service_test | 准备缓存复用（不重复准备、仍导入并应用策略、仍写语义报告、输出相同）已由 `render.rs` `cached_preparation_does_not_skip_import_policy_or_report` 覆盖（Apple 的缓存归 F）；策略 JSON 错误为参数错误、缺失策略文件为 IO 错误 → `render_service.rs` `policy_errors_are_parameter_and_io_errors`；非 UTF-8 输出 / 策略路径被拒而非改写为替换字符、含 NUL 被拒、字面替换字符可用 → `mradm-capi` `options.rs` `request_paths_reject_invalid_utf8_instead_of_replacing_it`（路径在 C ABI 边界转换，引擎只收 UTF-8） |
| normalization_acceptance_test | 移植为 `render_service.rs` `final_pcm_meets_the_normalisation_targets`：同一 6 s 节目（1 kHz 与跨采样峰值突发），响度 -23、加 -2.5 dB 终增益、峰值补偿到 -6 dBTP（float 与 24-bit）、热输入的默认 -1 dBTP 上限、静音保持数字静音且不报读数；以 `mradm-dsp` `Meter` 重新测量写出的文件，误差 1e-5，报告值等于测量值（`Meter` 对独立 BS.1770-4 参考的准确性由 `meter_acceptance.rs` 覆盖） |
| loudness_fixture_test | 移植为 `render_service.rs` `loudness_normalisation_reaches_its_target`：5 s 正弦归一化到 -23 LUFS（1.5 LU 内）、静音不报错且保持静音、缺失输入为 IO 错误 |
| peak_limiter_fixture_test | 移植为 `render_service.rs` `the_limiter_and_peak_make_up`：0.95 正弦限制到 -1 dBTP 附近、0.10 不变、0.01 补偿到 -6 dBTP 附近、峰值补偿无限制器时为参数错误 |

新增的 Rust 测试（约 4,900 行）：
- `mradm-engine`：`vbap_render.rs`（19 项）、`ear_render.rs`（9）、`hoa_render.rs`（6）、`hoa_decode_render.rs`（4）、
  `binaural_render.rs`（9）、`triple_balance_render.rs`（2）、`room_render.rs`（2）、`adm_import.rs`（14）、`scene_export.rs`（3）、
  `channel_bed_render.rs`（5）、`render_service.rs`（8），以及共用的 `tests/common/render.rs`；dev-dependency 加 `mradm-testkit`。
- `mradm-render`：`vbap.rs`、`ear.rs`、`hoa.rs` 各一项，`binaural_stream.rs` 四项，`room_runtime.rs` 三项；
  `tests/common` 的输入改为从文件读声道数。
- `mradm-dsp`：`hrtf_kemar.rs` 的 `kemar_lookups_reproduce_bound_and_shadow_the_measurements`。
- `mradm-scene`：`import.rs` 的 `catalog_aliases_custom_beds_and_malformed_axml`。
- `mradm-capi`：`options.rs` 的 `request_paths_reject_invalid_utf8_instead_of_replacing_it`。

**保留的 C++**（按决定 1，测的是生产岛代码，或只经 C ABI）：
- `core_smoke_test`：去掉 SDK 检查，只测 `src/adm_audio` 的包装；
- `speaker_layouts_test`：加入 `render_common` 的实时增益辅助（Apple 岛使用）；
- `room_render_test`：缩为只经 `c_api.h` 的测试，不再链接 SDK，E 批再移植。

**随批删除的测试支持**：
- `tests/support/binaural/` 全部（`binaural_test_probe.*`、`binaural_internal.h`、`hrtf_state.*`、`legacy_state.cpp`、`binaural_dsp.h`）
  及库 `mr_adm_binaural_test_support`，`engine_renderer` 库去掉 `hrtf_state.cpp`；
- `speaker_mix.h`、`scene_policy.h`、`sdk/dsp/hrtf.h`；
- SDK 的 `version.h` / `version.cpp.in`。

`engine_renderer` 与 `adm_scene` 库仍被 D（Monitor 支持）、F（Apple 与 LFE 路由测试）、G（手动基准）使用，留到那时。

**`mradm-ffi` 入口**：327 → 291。C2 删除失去全部 C/C++ 调用者的 `convolution`、`render_binaural`、`hrtf` 模块，以及 `spatial` 中的
HRTF 数据集与 HOA 矩阵入口。`render_binaural` 的分配计数测试改挂在 `render.rs` 下。`dsp_test_ffi.h` 与 `render_test_ffi.h`
中的声明、结构体同步删除。B 记录中留到 G 的无调用者入口剩 4 个（`mradm_dsp_hoa_matrix` 已在 C2 删除）。

**文档与 CI**：
- `release.yml` 的 Windows 优化构建测试集去掉已删除的 C++ fixture（含 `mr_adm_loudness_fixture_tests`），新增一步
  `cargo test --release -p mradm-engine -p mradm-render`，在优化构建下运行上述整段渲染测试；
- `CI.md` 同步。

稀疏大 BW64 一项只在 Unix 运行。C++ 版在 Windows 上也默认跳过，只在设置 `MR_ADM_RUN_LARGE_FILE_TESTS` 时运行。

生产代码不变，没有行为变化。

### C 的验证

每个子批都运行新增与改动的 Rust 测试、`cargo fmt --check` 与 workspace clippy `-D warnings`，以及 debug 构建与 ctest。
C4 之后的结果如下。

- **Rust**：新增与改动的测试全部通过（`mradm-engine` `render_service.rs` 8 项，包括 2^31 帧之后的稀疏 BW64 窗口）。
  `mr_adm_rust_unit_tests`（`cargo test --workspace`）通过。
- **C++ 侧**：debug 构建通过，ctest 37 项全部通过（C 之前为 53 项）。
  - FFI 头校验通过：291 个函数、138 个结构体；
  - `quality_scripts_test.py`、许可证检查与 C/C++ 棘轮通过；
  - `core_smoke_test.cpp` 通过 clang-format 与 clang-tidy。
- **对照参照构建 `b89687c`**（方法同 A，候选为 C4 之后的 `consistency-a`）：
  - C ABI 采集：3,283 行、36 个文件，0 差异；
  - CLI 记录：113 条命令，0 差异；
  - C 消费者测试 2 项通过；
  - 共享库恰好导出 164 个 `adm_*`；
  - 完整 PCM 采集与参照完全相同：294 个输出、220 个 fixture、989 项比较、176 个重放、256 个产物、666 份日志。

**规模**：测试 C/C++ 由 51,580 行降到 36,402 行：
- `tests/unit` 34,330 → 20,339；
- `tests/support` 13,390 → 12,203。

`mradm-ffi` 由 23,751 行降到 22,366 行。生产 C/C++ 不变（9,432 行）。

### C 的 macOS / Windows 原生复核（2026-11-15）

验证源码为合并提交 `a739e54f`，包含 C1–C4、审查修复 `84be879d` 和主线已有的 B 批修复。

- macOS arm64 的 `cmake --preset debug`、`cmake --build --preset debug --parallel 8` 与完整并行 CTest 通过，45/45 项成功。
  新增的 `mr_adm_apple_render_trim_tests` 经公开 C ABI 共享库，将 6 对象稀疏 BW64 输入裁剪到超过 2^31 帧的位置，
  `4+7+0` 和 `9+10+3` 分别输出 12、24 声道，各 1024 帧。
- Windows x64 复用规范 MSVC/Ninja Release 构建与 Cargo 缓存，SOFA/IAMF 关闭，完整并行 CTest 通过，42/42 项成功。
  两平台的 `mr_adm_rust_quality` 均通过，包含 workspace 的 rustfmt 与全部 targets 的 Clippy `-D warnings`。
  Windows 对 `Type` 的未使用导入告警通过 Unix 条件编译消除。
- 房间切换检查恢复 Scene 流式渲染的零帧处理步骤，确认旧候选已进入等待队列后，再提交失败的新请求。
  临时移除 `Runtime::pump` 的 `note_room_request` 通知时，修复后的检查会因旧候选被应用而失败。
  还原通知后检查通过。
- macOS 的变更 C/C++ 检查通过，包含 clang-format、clang-tidy 和 cppcheck。
  clang-tidy 仍报告 `room_render_test.cpp` 中 5 项既有告警，涉及非法枚举测试、公开成员和指针声明。
  FFI 头校验、许可证与 C/C++ 棘轮检查通过，两平台共享库均恰好导出 164 个 `adm_*` 函数。

## D 实时测试

D 处理经 C++ 外壳测实时链路的 6 个测试：
- `tests/support/realtime/monitor_engine` 之上的 Monitor；
- `tests/support/scene_stream` 之上的 Scene 输出；
- `tests/support/osc` 之上的 OSC 协议。

检查改为直接驱动 Rust 的 `mradm_engine::monitor::Monitor`、`mradm_engine::scene::output::Session` 与 `mradm-osc`。
测试设备只拉取环中已有的帧，与 C++ 的 `ManualSink` 相同。分四个子批提交：D1 Monitor、D2 Scene 输出、D3 OSC、D4 收尾。

| C++ 测试 | 去向 |
|---|---|
| realtime_stream_fixture_test | 移植为 `mradm-engine` `monitor_engine.rs` 的 18 项（经测试设备只拉取环中已有的帧，同 C++ `ManualSink`）：整次回调的电平含 EOS 填充、零帧拉取清空电平而不消耗媒体（实时与非实时）、暂停为精确静音（有无 HpTF）、seek 从最后输出帧桥接且步进 < 0.05、480 帧后落在目标时间线、暂停 seek 后静音拉取不推进播放头、恢复时淡入从静音开始、暂停中快速 seek 再 play 等待目标并只 flush / resume 一次、干净 EOS 之后的静音不计欠载、增益编辑整体校验（800 dB、±inf、NaN、后一项非法时整份拒绝、静音时允许溢出、下溢为合法增益）并到达输出、切换后保持编辑、2048 帧交叉淡化的逐帧权重、被第二次切换或 seek 打断的交叉淡化、交叉淡化中提前结束的短流、停止时不再开始 loop 预滚、长预滚不阻塞控制且被 seek 与停止取消、预热成功后交叉淡化接入、输出级 loop 分别回绕源与播放、LUFS 播放前静音 / 读出正弦 / 暂停 seek 清空并重新积分、HpTF 输出符合解析幅度且切换后仍生效、无 HpTF 时逐位相同、LUFS 取 EQ 前而峰值取 EQ 后、HpTF 跨 seek 无爆音；输出环的单生产者单消费者无丢失无重复 → `mradm-engine` `ring.rs` 单元测试；播放、loop、畸形流、失败日志、实时编辑 revision、带预热的切换与失败预热已由原有 10 项覆盖；ring buffer、PatternStream、capture sink、stream factory 与 downmix 测的是测试支持本身，随 `ring_buffer.h` / `downmix_stream.h` 删除；miniaudio null 设备由 `scene_output_c_api_test` 经 C ABI（`ADM_SCENE_OUTPUT_NULL`）覆盖 |
| stereo_peak_guard_test | 峰值保护内核的透明、音量、静音、分块无关、EOS 与 reset、非有限输入已由 `mradm-dsp` `output_dsp.rs` 覆盖；移植增益平滑（相邻帧步进 < 0.001）、在 5 ms lookahead 内提前压低、之前的材料不变 → `output_dsp.rs` `peak_guard_gain_is_smooth_and_anticipates_the_overload`；Scene 输出移植为 `mradm-engine` `scene_output.rs` 6 项：保护后的媒体长度与呈现时钟、暂停保留 lookahead 不消耗媒体、新 epoch 不带旧 lookahead（加一个在仍有响亮 lookahead 时中断的 epoch，原测试的两个 epoch 都排空到结尾，测不到这一点）、无保护输出对超满幅 PCM 精确缩放音量、epoch 重置按快照应用 HpTF 元数据与挂起的 bypass、设备输出与拉取分块无关且被替换的排队目标从不生效、欠载不改变媒体，以及并发创建与销毁空闲流；HpTF 在上限之内与非立体声不支持 HpTF 已由原有测试覆盖；外部真实 PCM（`MR_ADM_TEST_STEREO_PCM`）一项不移植 |
| scene_callback_alloc_test | 移植为 `mradm-engine` `scene_pull_allocations.rs` `session_pulls_through_hptf_volume_and_underruns_do_not_allocate`：双耳与 44.1 → 48 kHz 重采样立体声（HpTF 淡化中再排一个目标、音量）及无保护 4+7+0（音量），每次拉取包括欠载与恢复后都不分配；C++ 分配计数器自检只针对 C++ `operator new`，随测试删除 |
| hptf_parameters_test | **缩为只经 C ABI**（E 批再移植）：解析缓冲区、未来布局与校验、快照所有权与快速更新、文件接口的错误所有权；解析结果改与按原文写出的期望比较，不再经 C++ 解析器；准备好的 HpTF 不等输出控制锁 → `mradm-ffi` `engine_scene_output.rs` 单元测试（设备的 progress 阻塞时发布仍完成，epoch 重置采用共享或独立目标）；文本文件与内存参数等价 → `mradm-ffi` `hptf/settings.rs` 单元测试（两者设计出相同目标） |
| live_scene_trace_test | 测的是只供手动基准的 C++ trace 写出器（`live_scene_trace.cpp`，G 批随基准删除）；Rust 检查点的状态行布局（12 个 f32、15 个 i32、保留负零、64 位元素 ID 先低后高）→ `mradm-engine` `scene/stream/trace.rs` 单元测试（编码从写出中分出，行为不变）；检查点键由 `scene_tail_trace.rs` 与完整采集的检查点对照覆盖 |
| osc_head_tracking_test | **缩为只经 C ABI**（E 批再移植）：遥测数值边界与实例排序、状态引用变化、回环 UDP 接收器契约、自动绑定与无效参数，测试内自带姿态时间字段，不再经 SDK 的 `HeadTrackingTiming`；解析与排序原先经测试支持的 C++ shim（`osc_head_tracking_protocol`）调用 `mradm-osc`，截断、尾字节、padding、非有限、零四元数、身份长度与 UTF-8、版本与旧地址、遥测身份必须是十进制字符串已由 `mradm-osc` `src/tests.rs` 与 `decode_golden.rs` 覆盖；移植四元数向量表及其缩放与取反、2^53 以上的 int64 精度、六个键的零与负值、遥测嵌套会话不符与超大 JSON、极点附近 Euler 输出保持旋转、来源排序的完整序列（重复、传输间隔、主机时间倒退、设备时间重复、显式时钟重置、缺席时间戳不抹去时钟历史、新设备会话需要元数据与参考推进、重连、旧实例退役）→ `src/tests.rs` 5 项；听者朝向使世界固定声源反向旋转已由 `mradm-dsp` `scene_math.rs` 覆盖 |
| （B / C 遗留）`tests/unit/{ear,hoa,live_vbap}.h` 与 `_ffi.h`、`sdk/dsp/pcm_mix.h` | 没有任何 include（前三者自 B 起，`pcm_mix.h` 随 D1 删除 `downmix_stream.h`），对应的 `mradm-ffi` 模块 `ear`（7）、`hoa`（12）、`live_vbap`（5）、`pcm_mix`（12）只剩这些头调用，连同原定留到 G 的 `scene_pose` / `scene_rotate_pose`（2）一并删除；内核由 `mradm-ear`、`mradm-dsp` 的 `hoa.rs`、`live_vbap.rs`、`pcm_mix.rs`、`scene_math.rs` 与 `realtime_allocations.rs` 覆盖，只测边界（空指针、别名、首次调用分配）的单元测试随边界删除 |

**新增的 Rust 测试**：
- `mradm-engine`：`monitor_engine.rs` 18 项，`ring.rs` 1 项，`scene_output.rs` 6 项，`scene_pull_allocations.rs` 1 项，
  `scene/stream/trace.rs` 1 项。
- `mradm-dsp`：`output_dsp.rs` 1 项。
- `mradm-osc`：`src/tests.rs` 5 项。
- `mradm-ffi`：`engine_scene_output.rs` 与 `hptf/settings.rs` 各 1 项，另有两个只在 `cfg(test)` 中的构造（`DeviceTable::progress_only`、
  `Settings::for_test`）。

`scene/stream/trace.rs` 把状态行的编码从写出中分出，便于不开诊断构建也能测，写出的字节不变。除此之外生产代码不变。

**故障注入**（每项改动后恢复）。D 按 C 批之后的做法，为最容易「测不到」的移植项各做一次。
- 能检出的故障：
  - D1：交叉淡化改为 2047 帧、loop 预滚前不检查停止、seek 不清 LUFS、seek 不重置响度积分、回调中不做 HpTF、播放端不重置 loop、
    不延后设备 flush；
  - D2：新 epoch 不重置保护或 HpTF、拉取中分配、HpTF 发布取控制锁、HpTF 移到保护之后；
  - D3：metadata revision 允许为零、主机时间允许倒退、新设备会话不需推进、缺席时间戳抹去时钟历史。
- 有两项最初测不到，都是照搬 C++ 断言造成的：
  - 「seek 不重置响度积分」：C++ 测试 seek 后重放同一音调，积分值本来就一样。现在 seek 后降低 20 dB 重放。
  - 「新 epoch 不重置保护」：C++ 的两个 epoch 都排空到结尾，没有 lookahead 可泄漏。现在加一个在仍有响亮 lookahead 时中断的 epoch。

**保留的 C++**（只经 C ABI，归 E）：`hptf_parameters_test`、`osc_head_tracking_test`。

**随批删除的测试支持**：
- `dsp/ring_buffer.h`、`realtime/downmix_stream.h`、`sdk/dsp/pcm_mix.h`；
- `dsp/stereo_peak_guard.*`、`dsp/hptf_cascade.*` 及库 `mr_adm_output_dsp_test_support`；
- `tests/support/osc/` 及库 `mr_adm_osc_protocol_test_support`、`head_rotation.h`；
- 无人 include 的 `tests/unit/{ear,hoa,live_vbap}.h` 与 `_ffi.h`。

`monitor_engine`、`island_stream`、`render_stream_factory`、`room_worker_wait`、`scene_stream`、`live_scene`（含 trace 写出器）与
`sdk/realtime/osc_*` 仍被 G 的手动基准与探针使用，留到那时。

**`mradm-ffi` 入口**：291 → 234，共删除 57 个：
- D2：峰值保护 6、HpTF cascade 6、HpTF 设计与幅度 2、`gain_scale` 1；
- D3：OSC 解析、来源 ID 校验与来源排序 4；
- D4：模块 `ear` 7、`hoa` 12、`live_vbap` 5、`pcm_mix` 12，以及 `scene_pose` / `scene_rotate_pose` 2。

`check-ffi-headers.py` 去掉三个删除的头。B 记录中留到 G 的无调用者入口已全部删除。

**文档**：`hptf-eq.md` 改为指向 Rust 测试。

### D 的验证

- **Rust**：新增与改动的测试全部通过，`monitor_engine.rs` 连跑 8 次稳定；workspace fmt 与 clippy `-D warnings` 通过。
- **C++ 侧**：debug 构建通过，ctest 33 项全部通过（D 之前为 37 项）。
  - FFI 头校验通过：234 个函数、122 个结构体；
  - `quality_scripts_test.py`、许可证检查与 C/C++ 棘轮通过；
  - 两个缩减后的 C++ 测试通过 clang-format 与 clang-tidy。
  - D1 的一次 ctest 中 `mr_adm_boundary_tool_tests` 失败：两次 CLI 记录之间工作树有改动，导致 source 不同。
    单独重跑通过，此后没有再出现。
- **对照参照构建 `b89687c`**（方法同 A，候选为 D4 之后的 `consistency-a`）：
  - C ABI 采集：3,283 行、36 个文件，0 差异；
  - CLI 记录：113 条命令，0 差异；
  - C 消费者测试 2 项通过；
  - 共享库恰好导出 164 个 `adm_*`；
  - 完整 PCM 采集与参照完全相同：294 个输出、220 个 fixture、989 项比较、176 个重放、256 个产物、666 份日志。

**规模**：测试 C/C++ 由 36,402 行降到 31,322 行：
- `tests/unit` 20,339 → 16,252；
- `tests/support` 12,203 → 11,210。

`mradm-ffi` 由 22,366 行降到 19,893 行。生产 C/C++ 不变（9,432 行）。
