# ROOM 准备迁移审查与对照

记录日期：2026-10-25。审查对象是 `4899f9c1`（4.6 D：资源加载与文件渲染事件表迁入 Rust），
参考实现为主线 `305159fc` 的 C++ `RoomRenderer::prepare`，修复提交为 `1b8463c9`。

## 修复

迁移前，极坐标转换经过私有 `mradm_room_polar`，临时建立 RNE / 渐进下溢环境，返回时恢复调用方。
`4899f9c1` 的事件表准备直接调用 Rust `loading::polar`，丢失了这层保护。新增入口测试先在未修复版本
失败：macOS arm64 的向上舍入模式下，`M+030` 声源的 Y 分量从 `0x3f5db3d7` 变成 `0x3f5db3d8`。
该复现只检查元数据，不产生听音或 PCM 对照音频。

修复将受保护的极坐标转换函数从 FFI 传入 Rust 事件表准备。增益和位置偏移继续使用原有算术作用域，
浮点寄存器操作仍留在 FFI，`mradm-render` / `mradm-dsp` 保持 `forbid(unsafe_code)`。
测试覆盖新准备入口的四种舍入模式、下溢清零开关，以及转换的次正规数输入和错误返回后的环境恢复。

## Release 对照

macOS arm64 与 Windows x64 均比较 `305159fc` 和 `1b8463c9`，复用既有 Release、Cargo 和
FetchContent 缓存。Windows 使用 canonical MSVC / Ninja 构建树。两者均关闭一致性插桩及 ROOM CUDA，
使用 48 kHz、exact 质量和固定合成资源。Windows 的通用 SOFA feature 关闭，ROOM 原生 SOFA 加载仍由
`room-exact-sofa` 用例实际覆盖。

| 每个平台的覆盖 | 用例数 | 检查 |
| --- | ---: | --- |
| 离线渲染与 CLI | 9 | 原有三项 ROOM 矩阵，以及点源、DirectSpeakers、LFE、同起点块、窗口/增益、i24；API 两次重复与 CLI PCM 相同 |
| 文件流式渲染 | 16 | 四种素材 × cloud/none × 固定/碎片分块，含实时覆盖、头姿、seek、FIFO 和完整尾声；两次重复与独立帧数预期一致 |
| Scene 流式渲染 | 4 | cloud 开关 × 固定/碎片分块，每项两个 epoch、两次重复；检查 PCM、状态、事件和输入 |
| 非默认浮点环境 | 24 | 三种素材 × 四种舍入模式 × 下溢清零开关；每项两次准备/文件流式渲染，核对调用方环境恢复 |

每个平台的 **57 份最终 PCM 输出**（9 + 16 + 8 epoch + 24）与参考实现逐位相同，
修复版的这 57 份输出在两平台间也逐位相同。输入、资源和用例目录指纹一致。采集后删除临时音频，保留指纹。
本次记录的是正确性和 PCM 对照，不构成性能或实时零超时验收，也未扩大为 96 kHz 迁移矩阵验收。

原始记录及用例在 [证据目录](evidence/room-preparation-review-20261025/summary.json)：
四份平台/版本记录保存二进制、资源、PCM 和时间线指纹；`catalog.json` 固定参数；
`room_fenv.cpp` 为非默认浮点环境的测量驱动，`collect.py` 为重复性与 PCM 采集器。
执行日志与构建恢复记录另保存在未跟踪的 `local/review-phase46d-20261025/`。
对照结束后，两平台均恢复执行前的工作区、构建配置和标准目标；Windows 原分支保持干净。

## 定向检查

- macOS：47 项 FFI 单元测试、8 项 ROOM 事件表测试，Debug 与 Release ROOM 集成测试通过。
- Windows：3 项 ROOM FFI / 浮点环境测试、8 项 ROOM 事件表测试及 Release ROOM 集成测试通过。
- Rust 格式、两 crate 的全部 targets Clippy（`-D warnings`）、C/C++ 规模棘轮通过。
- 私有 FFI 头校验通过：330 个函数、140 个结构体。

Rust 定向测试可复用 CMake 记录的 `MR_ADM_RUST_TARGET_DIR` 和目标 triple：

```sh
cargo test --locked --manifest-path rust/Cargo.toml -p mradm-ffi --lib room --no-default-features
cargo test --locked --manifest-path rust/Cargo.toml -p mradm-render --test room
```

实际运行时附加既有 `--target-dir` / `--target`，并保持构建树的 SOFA feature 配置；不另建依赖缓存。
