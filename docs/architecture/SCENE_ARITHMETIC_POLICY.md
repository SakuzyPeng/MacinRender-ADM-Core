# Scene 算术规则统一

> 2026-10-06，数值一致性二期的首个切片。
> 决议：[ADR 0014](../adr/0014-scene-arithmetic-policy.md)。

## 实现

应用所有 Scene 生产调用统一选择 `scene-separate-v1`，固定乘法/加法的独立舍入和运算顺序。
复用已实现的 Rust 非融合路径，保留坐标约定和 float/double 精度。C++ 入口不再根据
Apple ARM64 或全局 `MR_ADM_STRICT_FP` 切换算术模式。Live 双耳的插值、旋转、最近方向
与 PCM 增益过渡使用同一选择。

独立 C++ 对照按明确的相同规则编译，冻结源码保持原字节。Scene、Live 双耳、Live VBAP、
HOA 四个对照目标局部禁用隐式 FMA；这个目标属性不传播至生产库或第三方依赖。
`MR_ADM_STRICT_FP=OFF` 下仍执行完整对照；全局严格 FP 开关保留为其他数值研究工具。

新增四组从 IEEE binary32 分步舍入得到的固定输入/输出位模式。它们在融合末次乘加时相差
1 ULP，检查实际 C++→Rust 生产入口，覆盖了仅调整参考编译参数无法守护的策略选择。
构建测试同时确认参考选项只作用于参考目标。原有极点、channel-lock 阈值、HRTF 网格、
seek、短块、分配与错误原子性检查不放宽。

## 验证与复现

平台结果、配置、源码指纹和日志摘要存放于 `evidence/scene-arithmetic/validation.json`。
本轮使用已有工作区、macOS Debug/Release、Windows canonical 与 Linux ARM64 容器。
所有正式结果重新基于恢复并合并 dr_wav 工作后的源码取得；中途并行修改期间的构建不作验收。

```sh
cmake --preset release -DMR_ADM_STRICT_FP=OFF
cmake --build build/release --target mr_adm_scene_numeric_rust_tests \
  mr_adm_live_binaural_rust_tests mr_adm_live_vbap_rust_tests mr_adm_hoa_rust_tests
ctest --test-dir build/release --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure \
  -R 'scene_numeric_rust|live_binaural_rust|live_vbap_rust|hoa_rust'
python3 tests/unit/consistency_build_test.py
```

Release CLI 对照用 `scripts/consistency/compare-scene-arithmetic.py`，参数与 PCM 比较器相同：
`--reference`、`--candidate`、`--pcm-bits`、`--fixtures`、`--output`。覆盖 EAR、VBAP、
Triple Balance、HOA 编码、双耳点源/extent，以及具有 listener 旋转的双耳用例。
输出为小型合成 fixture；保留样本格式/时长、有限值、SHA-256 和原有 `2e-6*(1+abs(ref))`
门限，不用听感推断代替数值验收。

## 实测结果

三端均关闭全局严格 FP，参考开关保持默认 OFF：

| 平台 | Release 全套 CTest | Scene 比较值 | 最大绝对误差 |
|---|---:|---:|---:|
| macOS ARM64 | 68/68 | 3,214,210 | 0 |
| Linux ARM64 GCC 12 | 67/67 | 3,214,210 | 0 |
| Windows x64 canonical | 67/67 | 3,214,210 | 0 |

Mac/Windows Live 双耳各 59,829 个数值对照通过；固定 IEEE 位模式、channel-lock 阈值与
HRTF 网格选择的精确断言通过。这里的总体误差统计是数值比较，未据此宣称每一项零值符号
或完整音频链路都已跨平台逐位一致。

macOS 17 组 Release 音频前后对照全部逐位相同，包含 listener 旋转的双耳用例。
这是给定小型 fixture 的结论；固定乘加策略仍允许其他临界输入与原平台融合路径产生末位差。
构建策略 13 项测试、Rust fmt/Clippy、私有 FFI 头、C++ 质量与许可证/冻结参考校验通过。
公开 C ABI 在 Mac/Windows 上仍为 139 个导出。新增的 ARM64 Release CI 未在本地工作期间触发。

## 阶段结果与后续

此切片解决共享 Scene 的平台乘加规则分叉。EAR 迁移仍已完成；前一阶段记录中的
Linux ARM64 默认 FP 对照缺口由本阶段按明确规则收敛，历史记录保留。
后续数值一致性工作继续覆盖平台数学函数、FFT 分派与完整软件 PCM 链路。
