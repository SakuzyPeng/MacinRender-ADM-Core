# 六期结项（六期 6.7）

> 状态：验收中（2026-11-19）。验收运行见下表，结论待各工作流完成与维护者实机检查。
> 六期按 [ADR 0021](../adr/0021-rust-primary-project.md) 把边界迁入 Rust。切片与用户决定见
> [PCM 一致性审计的六期一节](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)，C ABI 家族、CLI 契约与规模基线见 [6.1 记录](RUST_BOUNDARY_BASELINE.md)。

## 验收

验收提交是 main `e40638c`：6.6 合入 `115afbd`，之后是维护者的两个 MSVC CRT 修正。
本分支在它之上另有 6.7 的三个提交：
- A：Quality 的 cppcheck 修正；
- B：删除对照工具；
- 本记录。

Consistency 与 CI 在 main 上运行。Quality 与 Release Build 用 6.7 A、B 之后的分支运行。原因有两个：
- Quality 在 main 上从 `2029e31`（R3 G 合入）起失败。唯一的 cppcheck 发现在 R3 G2 加入的 `tests/manual/capture_tap.c`，由 6.7 A 修正。
- Release Build 自 6.6 改为 Cargo 构建后还没有运行过。

| 工作流 | 提交 | 运行 | 结果 |
| --- | --- | --- | --- |
| Consistency | main `e40638c` | 44492349541 | 通过；config A、B 的普通与诊断四份报告都是 294 个 PCM id 相同、0 个不同、平台集完整，门禁失败 0 |
| CI | main `e40638c` | 44492349621 | 通过 |
| Quality（full） | 分支 6.7 B `8177028` | 44494896493 | 待完成 |
| Release Build | 分支 6.7 B `8177028` | 44494891324 | 待完成 |

Reference tests 已随 R1 删除，六期不再运行。

维护者实机检查（macOS arm64、Windows x64）见 [6.6 记录](RUST_BOUNDARY_CARGO.md#待维护者在-macos--windows-复核)。
6.6 合入前，维护者已在两平台修正 Cargo 构建：
- `faff0b0`：CLI 入口、编解码器链接依赖、版本化的库别名；
- `2723730` 与 `e40638c`：MSVC Debug 与 vendored Opus 的 CRT。

**退出条件**的状态（逐项待上表确认）：
- ABI 导出：`check-capi-exports.py` 在 CI 与 Release 中校验恰好 164 个 `adm_*`。
- GUI P/Invoke：由 Release 的两个 GUI 包 smoke 验证。
- CLI 输出与退出码：6.4 起由 CLI 记录对照，R3 E 起由 `mradm-cli` `tests/binary.rs` 守护。
- 依赖：CLI11、spdlog、fmt、tl-expected 已移出构建。nlohmann_json 只在构建测试时获取，供 Apple 冒烟测试。

## 切片

| 切片 | 内容 | 生产 C/C++（行） | 记录 |
| --- | --- | ---: | --- |
| 6.1 | 方向与基线：C ABI 家族、CLI 契约与岛入口清单，CLI 记录与 C ABI 采集对照工具 | 21,772 | [6.1](RUST_BOUNDARY_BASELINE.md) |
| 6.2 | 岛改为由 Rust 调用（编码器与元数据、Apple 后端、设备与平台事实），`island-interfaces` 计量 | 21,124 | [6.2](RUST_BOUNDARY_ISLANDS.md) |
| 6.3 | C ABI 的 164 个导出迁入 `mradm-capi` | 16,705 | [6.3](RUST_BOUNDARY_CAPI.md) |
| 6.4 | CLI 迁入 `mradm-cli`（clap，退出码沿用 CLI11，ADR 0022） | 15,314 | [6.4](RUST_BOUNDARY_CLI.md) |
| 6.5 | C++ 外壳与 SDK 删出生产，`include/adm` 只剩 `c_api.h` | 9,432 | [6.5](RUST_BOUNDARY_SDK.md) |
| R1 | 冻结参考退役（`tests/reference` 的 18 个单元） | 9,432 | [R1](RUST_REFERENCE_RETENTION.md) |
| R2 | `mradm-capi` / `mradm-cli` 直接调用 Rust，`mradm-ffi` 收缩为 `api` 门面 | 9,432 | [R2](RUST_BOUNDARY_DIRECT.md) |
| R3 | C++ 测试移植为 Rust，依赖 SDK 的工具与探针改为 Rust example，C++ 测试 SDK 删除 | 8,704 | [R3](RUST_BOUNDARY_TESTS.md) |
| 6.6 | 构建以 Cargo 为主：岛调用 Rust 的函数表、岛工程与 `mradm-islands-sys`、cdylib 与 `mradm` bin，Corrosion 移出 | 8,684 | [6.6](RUST_BOUNDARY_CARGO.md) |
| 6.7 | 验收与结项；Quality 修正，删除 C ABI 采集与 CLI 记录对照工具 | 8,684 | 本记录 |

行数是各切片最后一次合并后 `language-surface-baseline.json` 的值。
- 6.2 与 6.3 之间相差 3 行，6.3 的记录从 21,121 行起算。
- R1 与 R2 不改生产代码。
- PCM 门禁六期保持 294 个 id，内核测量 255 份，全部设为门禁，六期没有有意的数值变化。

各切片都是结构迁移，以同一构建上的新旧工具对照验收：
- `collect.py --tool` 与 `compare-collections.py`；
- 6.1 的 CLI 记录与 C ABI 采集；
- 删除前生成的黄金数据。

易漏项另做故障注入。

## 规模

**生产 C/C++**：21,772 → 8,684 行，减少 13,088 行（60.1%）。其中：
- `c_api.h` 2,009 行是契约；
- 其余主要是编码器岛与岛共用的头。

| 区域 | 6.1 | 结项 |
| --- | ---: | ---: |
| 生产 C/C++ | 21,772 | 8,684 |
| Apple / Windows 岛 | 3,045 | 3,021 |
| 岛接口（6.2 起单独计量） | — | 1,057 |
| CUDA 边界 | 512 | 512 |
| C++ 测试 | 53,071 | 9,927 |
| 冻结参考 | 12,026 | 0 |
| Rust crate（不含 vendor） | 126,935 | 144,523 |

生产 C/C++ 的剩余部分（48 个文件）：

| 内容 | 行数 | 文件 |
| --- | ---: | --- |
| 公开 C ABI | 2,009 | `include/adm/c_api.h` |
| 编码器与容器元数据 | 3,550 | `src/adm_audio`（Opus MKA 835、APAC 738、FLAC 583、IAMF 525、MP4 / CAF 元数据 318、WAV 读写与句柄 273、WAV 包装与 FFI 头 221、IAMF bridge 头 57，不含岛接口） |
| 岛共用的头 | 1,341 | `src/adm_core`（`audio_io.h` 333、`options.h` 224、`errors.h` 144、`render.h` 116、`room.h` 91，以及函数表 `rust_services_ffi.h` 72 等） |
| 渲染公共层与私有 FFI 记录 | 848 | `src/adm_render_common`（`engine_ffi.h` 277、`render_common` 255、布局表视图、路由与布局 FFI） |
| 输出设备 | 405 | `src/adm_realtime`（miniaudio 设备 215、设备接口 135、媒体时间线 55，不含岛接口） |
| 私有 FFI 头 | 504 | `src/adm_metadata` 252、`src/adm_dsp` 252（计量、增益、Scene 数学、房间） |
| 进度回调跳板 | 27 | `src/adm_c_api/adm_c_api.cpp` |

[6.1 记录](RUST_BOUNDARY_BASELINE.md#规模基线)预计六期结束时生产 C/C++ 约 5,500–6,500 行，实际多出约 2,200 行。差距主要来自两处：
- 编码器与容器元数据：6.1 估计约 3,000 行，实际 3,550 行；
- 岛共用的头与渲染公共层：编码器与 Apple 岛仍在使用，共 2,189 行。

它们都留在七期按岛处理。

## 开放问题（移交七期）

- **BINAURAL-001**：Windows cloud 离线端到端耗时回退，只涉及性能，继续单独跟踪。见 [4.7 记录](RUST_INVERSION_BINAURAL.md#binaural-001windows-cloud-离线端到端耗时回退p2开放)。
- **物理头追踪**：维护者暂不接入物理头追踪设备，OSC 接收的实机采样与听感确认暂缓。
- **GUI**：按维护者决定，GUI 在迁移完成后再考虑，可能替换 Avalonia。六期没有改 GUI 行为。
- **Windows 打包**：Windows 的 CLI 发布与 GUI 打包仍经顶层 CMake 树构建，DLL 与 exe 本身由 Cargo 产出。原因有两个：
  - 发布前要跑 fixture 测试；
  - GUI 包要检查可选的 CUDA 插件与 vcpkg 工具链。
  
  见 [6.6 记录](RUST_BOUNDARY_CARGO.md)。
- **macOS 签名**：直接 `cargo build` 得到的 `mradm` 要手动 `codesign --force --sign -` 后才能做 APAC 编码。cargo 没有链接后的步骤。
- **进度回调跳板**：`adm_c_api.cpp` 的 27 行 C++ 把调用方回调抛出的 C++ 异常转为 `ADM_ERROR_INTERNAL`。
  - 去掉它会改变「回调抛异常」的行为；
  - C 调用方不受影响；
  - 留给七期决定。
- **dr_flac**：按用户决定暂缓。

## 七期（初拟）

七期按[逆转路线](RUST_INVERSION_ROADMAP.md)收缩平台岛：
- Apple / Windows sink 与编码器 C 库只留窄 C 接口；
- 评估 objc2 / windows crate；
- 退出条件是生产 C/C++ 只剩岛。

冻结参考退役与 C++ 测试清退已提前到 R1 / R3。

下面的切片是初拟，等用户决定：

| 切片 | 工作 |
| --- | --- |
| 7.1 | 方向与基线：逐岛清单（调用的第三方 API、岛接口、测试），objc2 / windows crate 的评估标准与原型范围，七期规模基线。 |
| 7.2 | 编码器的 C++ 封装迁入 Rust，libFLAC / libopus / AOM bridge 保留为 C 库，由 Rust 经窄绑定直接调用：<br>• Opus MKA 的 Matroska 封装<br>• FLAC 写出<br>• IAMF 的 OBU 与 MP4 打包驱动<br>• MP4 元数据<br>• WAV 薄包装 |
| 7.3 | 输出设备：miniaudio 保留为 C 库，由 Rust 经窄 C 接口直接驱动；C++ 设备岛与 `audio_output_device.h` 删除。 |
| 7.4 | Apple 岛：以 objc2 原型评估 AudioToolbox APAC 编码、AUSpatialMixer 后端与 AVSampleBufferAudioRenderer sink，据结果决定迁入或保留窄 C 接口。 |
| 7.5 | Windows 岛：以 windows crate 评估 ISpatialAudioClient sink 与 MMCSS。 |
| 7.6 | 收尾：岛共用的 C++ 头与私有 FFI 记录随调用方删除，C++ 岛测试与顶层 CMake 测试树随岛收缩，进度回调跳板与 Windows 打包路径另行决定。 |
| 7.7 | 三平台验收与结项。 |

## 维护与边界

**Consistency**：
- 继续在 push main 和手动触发时运行；
- `scripts/consistency/gates.json` 是门禁的唯一清单，新增 PCM 用例必须同步加入门禁（`collection_tools_test.py` 强制）；
- 门禁只约束锁定的矩阵、构建条件与三个平台（macOS arm64、Linux x64、Windows x64），不构成对任意输入或工具链的全局承诺。

**契约**只有三样（用户决定 2026-11-14）：
- `c_api.h` 的二进制兼容；
- CLI 输出与退出码；
- 门禁矩阵。

其他行为可以改，改了记进发布说明。

**对照工具**：6.1 的 CLI 记录与 C ABI 采集工具已随 6.7 删除，git 历史可查。之后的边界改动由以下几处守护：
- `mradm-cli` 的 `tests/binary.rs`；
- `mradm-capi` 的 Rust 测试；
- C 消费者测试 `mr_adm_c_api_tests`；
- 导出检查 `check-capi-exports.py`。
