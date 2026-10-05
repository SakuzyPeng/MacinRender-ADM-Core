# Rust 共享 PCM 混音与状态迁移

> 2026-10-04：通用扬声器时间线、EAR 双总线累加及监听固定矩阵使用 Rust，基线为 `4286f3e`。

## 所有权与接入

`mradm-dsp::pcm_mix` 持有预计算数值表、各实例的游标和工作缓冲；算法库继续禁止 unsafe。
`mradm-ffi::pcm_mix` 使用不透明句柄和带长度的数组，C++ RAII 适配层位于 `src/adm_dsp/pcm_mix.h`。
没有新增第三方依赖、功能开关或公开 ABI。基线的增益溢出发布检查继续保留。

准备阶段由 C++ 完成布局、ADM 和 libear 系数生成，再将普通 float 或 EAR double 系数及块信息
复制到 Rust。不可变 `Plan` 由 `Arc` 共享；C++ 准备对象只保留对象 ID、输入通道和扬声器标签，
临时系数及传输数组随后释放。每个离线渲染/stream 使用独立 `Mixer`，配置自己的最大块长、
默认插值长度和额外对象平滑开关。销毁原准备句柄不影响仍持有表的实例。

- VBAP 与 Triple Balance 的通用路径通过整表/单轨操作累加到现有交错输出；元数据选择、插值和游标由 Rust 完成。
- EAR 的分离声道输入、列式 direct/diffuse 缓冲及端点暂存由 Rust 持有，再输出交错双总线。
  C++ 保留 FIR 去相关、补偿延迟、wav71 顺序转换、文件 I/O、计量和线程调度。
- Triple Balance 保留点声源递推、panner、size DSP 和 checkpoint。动态表按 1024 帧块预留 3 个事件，
  C++ 传输缓冲同样预留容量。更新完整验证后原地替换；处理每条动态曲线时重新定位，保留原来
  单次调用使用局部游标的规则，因此同一窗口可重复用于不同增益的干声分支。
- 固定 downmix/upmix 矩阵由 Rust 保存。布局到矩阵的规则以及 `DownmixStream` 控制转发留在 C++，
  内层读取仍只接收一次原大小请求，短读只覆盖已产出的帧。I/O 暂存仍按需增长；不扩大支持的布局转换范围。

## 数值与运行契约

通用路径保留 `(input × spatial_gain) × output_gain × live_gain` 及原源轨道累加次序。
普通时间线插值以 double 运算后回到 float；EAR 常规插值直接使用原始 double 系数，
额外对象平滑的端点查询则先将系数转 float，再执行原有插值。外层端点混合使用 float。
EAR 静态零系数继续跳过；普通混音和固定矩阵仍执行原有乘法，不新增 NaN/Inf 清洗。

首块、jump、默认/显式插值、有效时长钳制、重复起点、空轨道和间隙均保留。
额外对象平滑仍由原外层处理块首尾决定，包括首尾满足条件时对块内间隙的既有行为。
迁移没有重新划分渲染网格；不同对象平滑分块不承诺得到同一结果。

零帧不推进游标。reset 清空游标和暂存，下一次按调用方绝对起点定位；它不消费音频，
也不替调用方重置去相关器等其他状态。Triple Balance 恢复已有 checkpoint 后重新生成短曲线，
没有把新的 Rust 对象地址或额外状态加入 checkpoint。

Rust 处理、reset、容量内动态更新和相应 FFI 从准备后的第一次调用起无分配、无锁、无 I/O。
这不包括创建/销毁、现有 C++ panner、文件读取、Downmix I/O 暂存扩容或整个渲染器。
参数校验失败不改变输出、游标或已有曲线；检查尺寸/乘积溢出、完整帧、通道/轨道索引、
时间溢出、排序、系数有效性和动态容量。C++ 下混在读取源之前检查输出长度。
FFI 捕获 panic；调用方仍负责指针有效性、对齐、互不别名和句柄独占访问。

## 验证

`tests/reference/pcm_mix_legacy.h` 冻结基线的旧数学实现，仅用于测试。
`mr_adm_pcm_mix_tests` 比较完整 C++→FFI→Rust 路径，覆盖 1/2/12/64 输入、1/2/6/12/24 输出、
1/7/37/128 帧块、空轨道、重叠/重复块、间隙、插值钳制、平滑、live gain、非零初始输出、
共享表独立实例、随机窗口 reset、动态更新/重复窗口和固定矩阵。

| Release 同平台旧/新最大绝对差 | 样本数 | macOS arm64 | Windows x64 |
|---|---:|---:|---:|
| 通用混音 | 394,560 | `5.9604644775390625e-8` | 0 |
| EAR 双总线 | 394,560 | `1.1920928955078125e-7` | 0 |
| 动态曲线 | 1,536 | 0 | 0 |
| 固定矩阵 | 18,084 | 0 | 0 |

一般有限 PCM 门限为 `|new-old| ≤ 2e-6 + 2e-6×|old|`。Rust 独立测试另以精确值验证
恒等/单路路由、增益、重复调用和时间边界，并构造 double 系数落在 float 舍入边界两侧的用例，
严格区分 EAR 两条精度路径。错误测试核对失败原子性，分配计数覆盖首次处理和动态更新。

13 组 macOS Release CLI 前后对照全部达标，其中 10 组逐位相同，最大绝对差
`3.5762786865234375e-7`。用例包括 EAR 的 point/extent/Cartesian/bed/HOA/window，
VBAP 的 point/extent/bed/window，以及 Triple Balance 的 Cartesian point/window/22.2。
Triple Balance 点声源使用 `--speaker-spread-mode none`；专有 bed/size、动态控制及 seek 由既有原生 fixture 覆盖。
这些是小型合成输入，不代表所有节目、主观听感或跨平台逐位一致。

- macOS Debug：58/58 CTest；Release 受影响回归：19/19。
- Rust Release：5/5 混音测试、8/8 分配测试；FFI 错误/所有权测试随 workspace CTest 通过。
- Windows canonical Release：57/57，SOFA ON，三个旧库参考开关 OFF。
- Rust fmt/Clippy、改动 C++ 格式/clang-tidy/cppcheck、参考头检查及许可证/SBOM 检查通过。
- 两端保留同一组 139 个公开 `adm_*` 导出；私有 Rust 入口不外泄。Windows 附带 C++ 导出不属于稳定 ABI。
- 最终三平台 CI 的提交、运行链接和结果记录在机器可读验收文件中。

Windows 原工作区的改动和原始字节保留。同步时额外合入 `4286f3e` 的增益发布修复，
Monitor 和 realtime fixture 的两份既有 Windows 差异通过三方合并保留；测试源码与本地并非完全相同的干净检出。
没有新增独立工作区。本地恢复标准 Debug/Release 目录，复用现有 FetchContent 源码和共享 Cargo 产物根目录。

机器可读记录：[validation.json](evidence/rust-pcm-mix/validation.json)、
[macOS 数值](evidence/rust-pcm-mix/macos-comparison.json)、[Windows 数值](evidence/rust-pcm-mix/windows-comparison.json)、
[Release CLI 对照](evidence/rust-pcm-mix/macos-audio.json)。本轮未作吞吐/RSS 或主观试听结论。

## 复现

```sh
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base 4286f3e --build-dir build/debug
scripts/quality/check-licenses.sh --build-dir build/debug
cmake --build --preset release
build/release/mr_adm_pcm_mix_tests comparison.json
python3 scripts/consistency/compare-pcm-mix.py \
  --reference /path/to/4286f3e-release/mradm --candidate build/release/mradm \
  --pcm-bits build/release/mr_adm_pcm_bits --fixtures /path/to/fixtures --output comparison-cli.json
```

CLI 输入由已有 `mr_adm_make_fixture` 生成；比较器会删除临时输出，只保留数值和指纹。
窗口对照沿用原块网格和 preroll。试听、PCM 对照和性能证据始终使用 Release。
