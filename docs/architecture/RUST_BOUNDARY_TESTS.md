# C++ 测试移植为 Rust（R3）

> 状态：进行中。A 已完成（2026-11-15，本机 Linux x64）；B–G 待做。macOS arm64 / Windows x64 原生复核待维护者。
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
