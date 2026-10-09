# Rust HRTF 插值与频域状态迁移

> 2026-10-04：离线、旧流式及实时 Scene 双耳路径均使用 Rust HRTF 插值。

## Live Scene 查询复用（2026-10-08）

Live 双耳会话在不可变 `Filters` 外持有两级私有 LRU：整数网格角点频谱，以及按原有
坐标归一化后 f32 位模式索引的完整连续方向查询。角点最多 512 项、查询最多 128 项；
包含键、LRU 链接和频谱的预算分别为 6 MiB、2 MiB（不含分配器自身开销），随频谱长度
缩减容量。所有槽位在会话创建时分配，冷查询、淘汰和 reset 均不分配或加锁。

角点仍由原来的 `grid_bin` 计算，连续查询仍使用原来的角点顺序、权重与乘加顺序。
容量不足时回到原查询算法，不量化姿态或改变插值精度。两轮事务处理共享缓存，因此
预校验的结果可供正式渲染复用；完整预校验和音频状态提交边界保留。失败请求只可能
预热纯查询缓存，不能推进音频历史。reset 清空键；新 SOFA／采样率通过新会话绑定新库。
共享滤波器、离线查询、公开接口及头追分块／lookahead 参数均沿用原契约。

Rust 对照测试覆盖缓存开关时的 HRTF/PCM 逐位一致、容量不足与淘汰、接缝／极点、
cloud/head-locked、失败回滚及多会话；FFI 分配计数从第一次冷查询覆盖持续运动和淘汰。

## 范围与所有权

`mradm-dsp/src/hrtf_filters.rs` 持有测量方向的频谱、实时幅度缓存及两种插值内核。
HRIR FFT 由准备构造函数调用已有 `RealFft`，准备结束即释放 FFT plan 和临时缓冲。
`hrtf.rs` 的几何算法沿用一期实现；新增不可变 `Grid` 持有权重和方向索引，
多个滤波器组通过 `Arc<Grid>` 共享同一张表。算法库继续禁止 unsafe，未新增依赖。

私有 `mradm-ffi/src/hrtf.rs` 与 C++ `src/adm_dsp/hrtf.h` 提供准备、查询、销毁和显式诊断快照。
正常渲染不会将整张网格或测量频谱复制回 C++。原来只负责导出网格和 HRIR FFT 结果的两个私有
FFI 入口已移除；公开 `include/adm/c_api.h` 不变。

C++ 继续负责场景语义、头部旋转、方向和 extent 解析、线程调度、缓存键与淘汰策略、声源求和及 I/O。
离线 spreader 所需的原始 HRIR 和测量方向仍由现有准备状态保留；实时缓存准备完成后释放这两份数据。
HpTF、通用监听增益渐变与峰值保护不属于本轮。

## 数值契约

| 路径 | 保留行为 |
|---|---|
| 离线 / 旧流式 | 方位角归一化后取最近的一度网格；保留离线末列 360 的取整规则，仰角限制到 ±90°。 |
| 实时 Scene | 相邻四个一度网格响应的复数双线性混合；方位角周期衔接、极点钳制，整数方向保留原网格响应。 |

每个网格响应仍以 `Σ w·|H|` 作为幅度，以 `Σ w·H` 的方向作为相位。
复数和的幅度不大于 `1e-9` 时，保留实数正幅度、零相位的回退。
这是原有抑制插值梳状抵消的方法，不是新的 ITD / 群延迟模型。

Rust 使用 `f32::hypot` 计算复数幅度；与平台 C++ 数学实现可能有末位差异。
验收约束当前算法、频响和连续性，不要求复现旧 SAF 位模式，也不承诺跨平台逐位一致。

## 实时、错误与缓存

- 查询只读取滤波器组；各 worker 使用独立输出缓冲，可并发共享同一组数据。
- 从准备后的第一次查询起无分配、无锁、无 I/O，包括方向变化、离线查询和无效方向返回。
  C++ 调用方仍须预留输出向量；显式诊断快照会分配，仅供诊断和测试使用。
- 创建失败清空输出句柄；检查空指针、尺寸溢出、数组长度、模式标志与非有限方向。
  非有限 HRIR 或变换后非有限频谱在准备期被拒绝。无效查询不会改写输出或诊断缓冲。
- Rust panic 由私有 FFI 转为错误码；RAII 保证创建方释放内存。销毁必须等待所有查询结束。
  滤波器组持有自己的网格引用，因此几何缓存淘汰或 C++ 网格句柄销毁不影响已有组。
- 几何缓存仍限 16 MiB / 8 项，实时状态缓存仍限 64 MiB / 4 项。
  内存统计包含 Rust 容器与其容量，实时缓存对每项保守计入共享网格；不含分配器和引用计数控制块开销。
  离线按需计算幅度，只有实时组保留幅度缓存。

原诊断频谱、网格及幅度/相位中间检查点名称保留；中间量由生产 Rust 内核产生。

## 验收

- macOS Debug：55/55 CTest；Release：55/55。测试代码格式调整后，插值测试两种构建各再通过 1/1。
- Windows canonical Release（SOFA ON，所有旧 C 参考开关 OFF）：54/54；最终测试代码调整后再通过 1/1。
- Rust fmt、Clippy `-D warnings`；改动和新增 C++ 的格式、clang-tidy、cppcheck；许可证、SBOM 校验通过。
- `MR_ADM_CONSISTENCY_DIAGNOSTICS` 分支额外通过语法编译检查，诊断缓冲由 Rust 单元/FFI 测试验证。
- 两个平台都保留 139 个公开 `adm_*` 导出，未泄漏私有 Rust 入口。Windows 仍有自动生成的
  1,984 个附带 C++ 导出，因此不能将其描述为只有 139 个 DLL 导出。

数值测试以 Release 结果作为记录：

| 对照 | 结果 |
|---|---|
| 独立保留的旧 C++ 插值算法；每平台 5,176 次查询，覆盖 KEMAR / 不完整球面、两种查询和缓存开关 | macOS 最大复数误差：KEMAR `4.80548e-7`，不完整球面 `1.88486e-7`；Windows 两组均为 0。 |
| macOS 迁移前后 Release CLI 的 8 个固定场景 | 最大 PCM 样本绝对差 `2.38418579e-7`，全部低于 `2e-6` 门限；其中 3 个用例逐位相同。 |

Rust 测试另覆盖独立 double DFT、测量方向还原、插值取整、方位接缝收敛、极点、相位抵消与阈值回退、
不完整球面、无效缓冲不改写输出、并发共享和准备后无分配。原双耳频响、实时运动、分块、SOFA
及公开接口 fixture 继续通过。

Windows 在保留既有修改的原工作区验证；同步的 21 个源码/测试文件与 macOS 相同，同步前保存原始字节并校验哈希。
本轮未运行 Linux CI、吞吐/RSS 基准或主观听音。以上小型 fixture 结果不代表所有节目或跨平台位相等。

机器可读记录见 [validation.json](evidence/rust-hrtf/validation.json)，
8 个 CLI 用例明细见 [macos-audio-comparison.json](evidence/rust-hrtf/macos-audio-comparison.json)。

复现主要检查：

```sh
cmake --build --preset debug
ctest --preset debug -R '(hrtf|binaural|sofa|rust_unit)' --parallel 6 --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base HEAD --build-dir build/debug

cmake --build --preset release
build/release/mr_adm_hrtf_interpolation_tests
ctest --test-dir build/release --parallel 6 --output-on-failure
```

CLI 前后对照使用已有 `scripts/consistency/compare-binaural-dsp.py`，参考二进制为迁移前
`ee21dc3` 的 Release CLI。检查新增且尚未跟踪的 C++ 文件时，另显式传给格式及静态检查工具。
