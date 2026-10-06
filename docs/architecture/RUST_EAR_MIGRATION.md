# Rust EAR 算法迁移

> 基线：`5dd0378`；上游 libear：`2db69f8fcea0bc5db8a78e14a9c2ae6ed4283c15`。
> 决议：[ADR 0013](../adr/0013-rust-ear.md)。

## 所有权与实现

`mradm-ear` 接管标准布局数据、nominal/effective 拓扑、虚拟扬声器、三角形/四边形/虚拟多边形
声像区域、特殊立体声下混和标量 extent。保留 extent 内部 f32 运算、direct/diffuse 等功率分离，
普通增益与解码矩阵使用 f64。支持项目全部标准布局、9.1.4/9.1.6 自定义布局与 Apple geometry。

DirectSpeakers 保留 common-definition 映射、标签优先顺序、坐标范围及 LFE 路由；标签和
pack ID 在 FFI 用长度明确的 UTF-8 字符串传递。HOA 使用原始 5200 点采样、AllRAD、整体能量
归一化和 N3D/SN3D/FuMa，支持稀疏/混合阶数，FuMa 延续最高三阶的范围。

FIR 设计使用 MT19937、整数除 2^32 的映射和声道名称排序种子，使用已有 RustFFT 的 f64
逆变换。每声道仍为 512 taps，直接声延迟仍为 255 samples。FFT 的平台 SIMD 路径仍可能不同，
本轮不承诺跨平台 PCM 逐位一致。

准备资源通过私有不透明句柄复用，extent 与 HOA 表按需生成。C++ 保留场景和语义预处理、
增益时间线编排、I/O 与计量调度。准备结束后仍由已有 Rust 混音和后处理持有运行时资源。
对象布局标签的 string_view 指向布局描述自身，避免临时字符串悬空影响矩阵路由。

原始指针只出现在 `mradm-ffi`；检查维度、对齐、可写缓冲互不重叠、句柄与描述不重叠，
在写输出前完成参数检查和计算。错误使用既有错误码，panic 被捕获；调用方仍须保证指针
存活和可变句柄独占。算法 crate 禁止 unsafe，不新增公开 C ABI 或运行时后端开关。

## 参考与依赖

`MR_ADM_BUILD_LIBEAR_REFERENCE_TESTS` 默认 OFF。开启后构建算法对照和可选 Release benchmark；
旧库不得成为运行时回退。默认 EAR 后处理测试改用确定的 dense FIR，继续验证冻结卷积算法
和独立 double 时域 FIR。冻结初始化仅改为接收系数，原短块缺陷对照继续保留。

默认生产与测试无需 libear/Boost/Eigen/xsimd/KissFFT。常规 CI 移除 Boost/vcpkg 安装；
手动 Reference tests workflow 保留 libear 防腐测试及其 Boost 依赖。
移植算法、静态表及采样数据的许可证和来源分别登记在 crate 的 LICENSE、NOTICE.txt、
PROVENANCE.json 和发行依赖清单；资源哈希纳入许可检查。

## 验证

验证摘要与平台证据存放于 `evidence/rust-ear/`。算法对照覆盖标准/Apple geometry 共 16 组
布局、点源/extent/depth/距离、DirectSpeakers、三种 HOA 归一化和 FIR。Rust 独立回归验证能量、
左右对称、精确路由、位置范围、LFE 零槽、HOA 归一化转换、MT 序列、FIR 频谱及声道重排。
FFI 测试覆盖错误原子性、输出别名、维度、非有限元数据与无效 HOA 参数。

| 平台与构建 | 默认参考 OFF 的回归 | 算法对照 | C ABI 导出 |
|---|---:|---:|---:|
| macOS ARM64 | Debug 68/68；Release 定向 12/12 | 582,984 个数值通过 | 139 |
| Windows x64 canonical Release | 67/67 | 582,984 个数值通过 | 139 |
| Linux ARM64 GCC 12 Release，严格 FP | 67/67 | 582,984 个数值通过 | 139 |

启用旧库参考时，macOS Release 定向 13/13，Windows 和 Linux 严格 FP 全套各 68/68。
三平台 extent 的最大缩放误差低于 `4e-7`，普通增益/HOA 矩阵低于 `4e-14`，
当前 FIR 对照逐位相同。13 组 macOS Release CLI 音频对照通过更紧的
`2e-6*(1+abs(ref))` 检查；未修改后端控制组逐位相同。音频为小型合成 fixture，
不作为听感评价或完整节目吞吐结论。

**本阶段验收时发现的 Linux ARM64 默认 FP 缺口（后续已处理）：** 全套结果为 67/68，失败项为
`mr_adm_scene_numeric_rust_tests`。差异来自其旧 C++ 参考与共享 Rust Scene 数值链路的
表达式收缩契约；这些源码与迁移基线一致。本轮未修改这条数值链路。使用既有
`MR_ADM_STRICT_FP=ON` 后，该项 3,214,202 个数值逐位相同，全套通过。
Linux 验证使用本机 ARM64 容器；本记录不代表 Linux x64 CI 已运行。
后续 [Scene 算术规则统一](SCENE_ARITHMETIC_POLICY.md) 已在全局严格 FP 关闭时收敛此问题；
此处保留 EAR 迁移当时的验证记录。

Release benchmark 分别测量 22.2、128 个 Objects 元数据块的准备，以及 1,048,576 帧
既有 Rust 混音/后处理；五次交替运行取中位数，另有一次预热。

| 项目 | macOS：旧 / Rust | Windows：旧 / Rust |
|---|---:|---:|
| 准备 | 7.365 / 0.915 ms | 15.076 / 2.164 ms |
| 持续混音与后处理 | 182.444 / 182.425 ms | 212.789 / 218.126 ms |

该 Objects 用例的准备阶段获益于共享声像资源和按需生成 HOA 表。持续 DSP 的实现未变。
CLI RSS/总耗时另用小型 fixture 记录，受启动和 I/O 影响，不作大型节目性能结论。

Rust fmt/Clippy、C++ 格式/clang-tidy/cppcheck、198 个私有 FFI 函数与 62 个结构体的头文件
校验、许可证/SBOM 和冻结参考登记通过。常规及受控数值构建记录标记 `ear.implementation=rust`；
未链接旧库时旧 SIMD 字段为 `not-linked`，不会沿用缓存值而误报标量参考失败。

## 复现与退役

```sh
cmake --preset debug -DMR_ADM_BUILD_LIBEAR_REFERENCE_TESTS=OFF
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality mr_adm_ffi_header_check
cmake --preset release -DMR_ADM_BUILD_LIBEAR_REFERENCE_TESTS=ON
cmake --build build/release --target mr_adm_libear_reference_tests mr_adm_libear_benchmark
build/release/mr_adm_libear_reference_tests comparison.json
build/release/mr_adm_libear_benchmark
```

复用现有 macOS 与 Windows canonical 构建，保留 Windows 本地改动。
Linux 使用同一源码的容器挂载；临时原生产物位于外置 cache，不复制源码或 Cargo registry。
完成后将 macOS Release 和 Windows canonical 的参考开关恢复为 OFF。
Linux ARM64 的全套复现需显式增加 `-DMR_ADM_STRICT_FP=ON`。

`libear` 对照登记于 `tests/reference/retention.json`；按[参考保留政策](RUST_REFERENCE_RETENTION.md)
在发布、独立回归与二期数值范围评审完成后退役。当前继续保留，独立测试不能单独替代完整
算法差分覆盖。既有 `ear_post` 冻结单元未退役。
