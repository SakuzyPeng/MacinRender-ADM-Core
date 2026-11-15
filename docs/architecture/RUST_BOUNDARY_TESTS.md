# C++ 测试移植为 Rust（R3）

> 状态：进行中。A、B 已完成（2026-11-15，本机 Linux x64）；C–G 待做。macOS arm64 / Windows x64 原生复核待维护者。
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
