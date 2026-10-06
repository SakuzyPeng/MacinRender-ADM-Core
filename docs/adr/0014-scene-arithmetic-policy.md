# ADR 0014：统一 Scene 的乘加舍入规则

> 状态：已接受。日期：2026-10-06。
> 数值一致性二期的第一个切片，接续 ADR 0008 与 Rust Scene 迁移。

## 问题与依据

Linux ARM64 GCC Release 的旧 C++ Scene 参考默认会融合部分乘加，Rust 生产入口此前
按平台选择兼容模式，只有 Apple ARM64 使用融合路径。因此默认 Linux ARM64 对照有
27 个断言失败，包括极点附近角度和 channel-lock 阈值。仅扩大平台宏不能统一编译器的
融合范围与顺序；全局严格 FP 虽可消除差异，但不是 Scene 算术定义。

## 决策

- 生产 Scene 统一采用 `scene-separate-v1`：保留当前 float/double 精度层次与求值顺序，
  乘法和加法分别舍入。坐标/extent/方向距离、头部旋转与 Live 双耳控制均由相同规则驱动。
- C++ 生产入口使用固定私有算术标志，移除 Apple ARM64 与 `MR_ADM_STRICT_FP` 条件选择。
  现有 Rust 非融合实现作为基线；公式、17 点采样、时序、路由规则与精度层次保持原样。
  显式融合模式仅保留作历史诊断，不作为应用运行时后端或回退。
- 冻结 C++ 参考源码不改写。Scene、Live 双耳、Live VBAP 和 HOA 的参考测试目标单独使用
  `-fno-fast-math -ffp-contract=off`（MSVC 为 `/fp:strict`），按同一数学规则验证。
  生产 C++ 目标不因此获得这些选项，全局 `MR_ADM_STRICT_FP` 仍默认 OFF。
- 四组独立 IEEE binary32 输入/输出位模式验证生产入口的舍入规则，避免仅靠旧实现对照。
  角度门限不放宽；HRTF 网格、channel-lock 分支等继续精确检查。
- 构建记录输出 `scene.arithmetic=scene-separate-v1`。新增 Linux ARM64 Release CI，
  显式关闭全局严格 FP，防止只测 Debug 或 x64 而遗漏优化构建差异。

## 影响与阶段边界

macOS 原先的融合路径改用固定的非融合路径，因此某些输入的最低位和临界点选择可以改变。
这是本阶段明确的算术规则更新，需要 Release 音频及几何/路由验证，不要求复现各平台原有
不同位模式。Windows 与 Linux 生产 Scene 原已选择非融合路径。

公开 C ABI、CLI 和格式保持兼容。既有时间线、seek/reset、分配和错误边界不变。
本切片统一基本算术；平台数学函数与 RustFFT SIMD 等后续工作另行验证，不将本结果扩展
为完整 PCM 的跨平台逐位一致承诺。

验证证据见 [Scene 算术规则记录](../architecture/SCENE_ARITHMETIC_POLICY.md)。
