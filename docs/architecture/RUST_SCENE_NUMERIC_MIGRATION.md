# Rust Scene 过渡、空间数学与 Live 双耳迁移

算法基线为 `726db31`。共享空间计算、Scene worker 过渡和 Live 双耳数值状态由 Rust 接管。
公开 C ABI、`ILiveSceneRenderer`、支持范围和外层渲染网格保持不变，没有新增依赖或构建开关。

## 所有权与调用边界

- `scene_math` 提供坐标、长度、距离、角度、divergence、extent、spread、头部旋转和
  PoseBridge 姿态转换。公共 C++ 函数保留签名，由库内适配器调用；`ADMCore` 安装包同时携带
  Rust 归档和系统链接依赖。最近扬声器查询使用固定大小的栈上传输缓冲，不限制扬声器总数。
- `scene_transition` 由 Scene worker 独占，持有后端淡化位置、最后输出帧、generation 锚点
  和过渡计数。C++ 继续管理 renderer、线程、队列、有理数时长、预热、裁剪与排空。
- `live_binaural` 持有每元素的当前/目标、11 组独立计数、方向、diffuse、卷积和尾音状态，
  以及复用的 FFT/频谱工作区。C++ 保留描述符、ID、语义目标和初始化标记，在暂存控制上
  解析路由并提交有序命令与借用的 PCM 视图；每次 render 只有一次混音调用。
- HRTF 私有句柄现持有 `Arc<Filters>`，新实例共享只读表。几何、频谱和缓存不复制到每个
  声源或预检查副本。C++ HRTF 缓存及其生命周期规则保留，私有句柄改变不影响公开 ABI。
- 双耳和 Apple 的旋转实例在处理前创建，参数更新复用存储。Rust 内部直接调用数学内核。
  C++ 继续决定角色、标签回退、语义适用规则、诊断文本与设备参数发布。

## 数值与时间线契约

通用 extent、离线双耳、HOA 和 Live cloud 保持独立的几何入口，保留归一化次数、轴向、
float/double 转换、采样槽位及累加顺序。17 点采样表仅在 Rust 生产代码中保留，旧表位于
测试参考。PoseBridge 的轴向与 renderer 的场景轴向没有混用。

macOS arm64 基线会收缩部分乘加。半径、通用切平面展开、方向距离和四元数乘法显式保留
相应顺序；C++ 根据既有平台和 `MR_ADM_STRICT_FP` 配置传递私有算术标记。普通数学路径
继续按其原精度计算。极点与 channel-lock 阈值分支不能用一般 PCM 容差替代；本轮对照
包含这些边界，也避免用编译期折叠代替运行时输入。

Scene 后端淡化仍在源采样率下运行 2048 帧，从 `1/2048` 开始；跨过终点的 slice 完整混合，
随后才完成后端切换。generation 过渡仍在输出采样率下运行 `max(1, rate*10/1000)` 帧，
从 `(position+1)/total` 开始，在重采样后、裁剪/收集前推进。隐藏帧更新历史；强制静音推进
计数并清零最后输出帧。零帧不推进状态，epoch reset 清历史，暂停保留历史。尾音 flush、
EOS 时长钳制和补零仍由原 C++ 调度完成。

Live 双耳继续按事件、1024 帧上限和最近字段期限分段。同偏移事件保留顺序，head_locked
立即生效，其他字段各自保留 jump/显式/默认期限；方向使用原 remainder 最短路径。
gain/diffuse 在卷积历史前生效，LFE 保留 `i/N` 增益，源按 generation 顺序累加。
缺失 PCM 推进历史，静音休眠和恢复规则不变，EOF 不由新内核额外补齐。

## 整帧错误原子性与实时范围

C++ 先完成语义准备。Rust 再以预分配的控制、32 样本 diffuse 和尾音计数副本重放整帧，
检查实际使用的方向、频谱和 diffuse 输出。成功后才清零请求范围并执行正式处理。
预检查不复制整套卷积历史或整帧 PCM，也不执行两遍 FFT。

参数或语义拒绝不修改输出、历史、初始化标记、语义目标或诊断去重状态。诊断按原有
事件/声源顺序生成数值标记，成功后由 C++ 发布。generation 配置先准备候选实例再替换。
Rust reset 复用存储，逻辑上清空 generation；内部零帧初始化继续允许。

私有 FFI 检查长度、完整帧、索引、标志、时间算术和可写缓冲重叠，panic 沿用现有捕获。
OOM、panic、内部前置条件错误及外部诊断回调异常不属于可恢复事务回滚承诺。
保留原有 PCM 有限性规则：非 LFE 输入在现有 diffuse/卷积边界检查，LFE 直接路径保留
IEEE 传播；公开 Scene 提交的非有限 PCM 拒绝规则不变。

Rust 内核和 FFI 从首次处理起无分配、无锁、无 I/O，覆盖预检查、处理、参数更新、reset
和参数拒绝。C++ 语义、命令与诊断缓冲允许增长并复用容量；不承诺整个 renderer 零分配。

## 验证与复现

冻结参考位于 `tests/reference/scene_numeric`，包括所有被迁移的数学辅助、PoseBridge、
Scene 标量循环和完整 Live 双耳 renderer。既有 HOA、Live VBAP 参考也改用冻结辅助函数，
避免新旧双方共用本轮迁移实现。来源与适配说明见该目录 provenance.json。

数值比较只使用 Release。同平台一般有限值要求 `abs(new-old) <= 2e-6 + 2e-6*abs(old)`；
简单路由、端点、canonical length 和明确的恒等结果逐位检查；计数、有效位、槽位、路由和
诊断顺序单独检查。输出拉取长度一致、离线/旧 stream、窗口、实时控制等既有回归保留。

初次验收的源码提交为 `f7f38de`；共享数学与过渡实现分别提交为 `08113fd`、`a1b2e01`。
[三平台 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37434454445)
在该源码提交通过 macOS Debug 66/66、Linux Debug 65/65、Windows Debug 65/65。
本机 macOS Debug 66/66、Release 定向 19/19；Windows canonical Release 65/65。
Rust Release workspace 164 项通过，既有物理 >4 GiB WAVE 测试按默认配置忽略。

| Release 同平台参考 | 比较浮点值 | macOS 最大绝对差 | Windows 最大绝对差 |
|---|---:|---:|---:|
| 空间数学，包含角度 | 47,802 | `6.103515625e-5`（角度） | 0 |
| Scene PCM、最后输出帧及锚点 | 3,166,080 | `1.1920928955078125e-7` | 0 |
| Live 双耳完整 renderer 与错误恢复 PCM | 59,829 | 0 | 0 |

所有值通过规定的逐项容差；端点、路由与关键状态另有精确断言。动态双耳继续使用原分段
规则，不扩大任意 SceneFrame 重分块的逐位一致承诺。

首次调用分配探针覆盖内核和 FFI 的查询、更新、预检查、处理、reset 与拒绝。Rust fmt/Clippy、
改动 C++ 检查、许可证/SBOM、安装后的独立 ADMCore 消费者均通过。C++ 检查保留 Apple
既有的 10 条非阻断建议。本批未增加依赖，Cargo.lock 与公开 C ABI 头文件均保持不变。

两端 C ABI bundle 均保留全部 139 个 `adm_*` 入口，没有导出私有 Rust DSP/ADM/WAVE
符号。Windows 延续原有 C++ 实现符号导出方式，这些符号不属于稳定 C ABI。
完整配置、52 项源码/测试/构建文件指纹、冻结参考及数值摘要保存在机器可读记录中。

```sh
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build --preset release
build/release/mr_adm_scene_numeric_rust_tests scene-comparison.json
build/release/mr_adm_live_binaural_rust_tests binaural-comparison.json
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base 726db31 --build-dir build/debug
scripts/quality/check-licenses.sh --build-dir build/debug
```

复用现有工作区、标准构建目录和共享 Cargo 缓存。Windows 保留原有未提交修改，同步前检查
源文件指纹并备份原字节。原有 Triple Balance 链接修改由并行提交 `a7d0e74` 独立保存，本批保留该祖先提交。最终记录见 [机器可读证据](evidence/rust-scene-numeric/validation.json)。跨平台逐位一致
继续留到二期，本轮不据迁移推断性能提升。

## 复查修正

macOS arm64 默认构建的切平面叉乘也需要遵循 `contract` 策略。遗漏融合乘加会让部分
extent 方向跨过半度边界，选择不同的离线 HRTF 网格。`cross_compat` 现保留这一策略；
Rust 测试覆盖融合／非融合的消减结果，C++ 冻结参考对照增加五组运行时输入，并精确比较
实际 HRTF 网格索引。修正前新增测试失败，修正后通过。

安装配置在查找依赖前保存本包前缀，防止 CMake 3.24–3.29 的依赖配置覆盖
`PACKAGE_PREFIX_DIR`。新增独立配置测试模拟依赖安装到不同前缀；在 CMake 3.29.6
实际验证了修正前失败、修正后通过，现有 CMake 4.2.3 也通过。

本轮本机 macOS Debug 全量 67/67、Release Scene／Live 双耳／包配置定向 3/3 通过，
Rust fmt/Clippy 与改动 C++ 质量检查通过。上方机器可读记录保留初次验收的提交及指纹。
