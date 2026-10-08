# Rust Triple Balance 数值状态迁移

> 基线 `1972817`；算法、共享准备数据、独立渲染历史和数值快照由 Rust 接管。

## 所有权与接口

`mradm-dsp::triple_balance` 包含固定房间几何、点源增益、Q15 尺寸空间算法、四路递归
去相关、对象控制状态和尺寸混合。算法库仍禁止 unsafe；`mradm-ffi` 负责私有指针边界，
C++ 以可移动 RAII 句柄管理所有权。没有新增依赖、功能开关、公开 ABI 或 GUI 参数。

C++ 在准备阶段提供原始 ADM 坐标、事件帧位置、bed 路由系数及输入通道映射。Rust 编译
普通点源的完整运动曲线，直接生成已有 `pcm_mix::Plan`；尺寸事件表由 `Arc` 共享。
C++ 不保留编译后事件或增益表副本，只保留对象 ID、输入通道、标签及 diffuse/尺寸范围摘要。
源场景仍由原有语义层管理。22.2 报告查询 Rust 的数值节点，C++ 保留标签映射。

每个离线渲染或 stream 创建独立 Session，拥有对象处理器、运动位置、过渡权重、分离输入、
对象输出、备用点源输出和曲线工作缓冲。生产块容量仍为 1024，每个动态点源预留
`1 + ceil(max_frames / 512)` 个曲线块（当前为 3）。动态系数在 Rust 内直接更新 PCM 混音器，
生产处理不逐样本或逐 32 帧子块跨 FFI。低层 C++ 对象/滤波适配器仅供现有测试和研究工具使用。

C++ 继续负责 ADM/bed 语义、覆盖发布、I/O、FIFO、计量、窗口调度和快照 LRU。
固定几何、支持的采样率与布局没有扩大：非零尺寸及 22.2 仍要求 48 kHz。

## 行为与错误契约

- 保留 float32 位置递推和尺寸平滑，时间常数分别为 1200/960 样本；控制网格 512 帧、
  滤波子块 32 帧、生产渲染网格 1024 帧。单事件点源保留原始 ADM 坐标初始化，避免
  坐标往返影响半码量化边界。事件选择、Q15 码和关键生命周期分支另外精确比较。
- 保留空间积分、原有 log2/exp2 近似、重复加法及转换顺序。正有限 binary32 的 frexp 和
  二次幂缩放由安全 Rust 位操作/精确 double 缩放实现，包括次正规数和下溢舍入。
- 保留 96 样本输入延迟、四级 152/200/263/346 延迟、前后包络、静音退出及尾声截断。
  尺寸归零块先输出上一尺寸的插值尾声再重置；干声系数为零时的高度状态继续更新。
- 静态行顺序和尺寸轨道顺序不变；尺寸 DSP、点源/尺寸过渡、对象输出增益、live gain 和
  PCM 累加的位置不变。动态点源每次累加重新定位，允许相同区间以不同增益重复使用。
- 单对象 push 保留 512 帧 pending 缓冲，finish 内部补零完成控制块，但只输出原始有效长度。
  普通零帧不推进状态；EOF 不追加尾音。pause、seek、窗口预热与外层 FIFO 行为保留。
- 创建/处理/更新先检查索引、完整帧、容量、有限参数及时间/长度溢出。每次被拒绝的 FFI
  调用不修改该调用的输出或历史；多轨尺寸混合先检查所有尺寸输入通道。
  尺寸 PCM 继续拒绝非有限值，普通点源/bed 沿用 PCM 混音的传播规则。
- panic 通过现有边界转换为错误码；内部已验证前置条件的失败按 ADR 0005 处理。
  调用方仍负责指针存活、对齐、缓冲不别名及可变句柄独占访问。

2026-10-08 扩展文件输入的密集元数据支持：每个 512 帧控制块顺序消费全部事件，以最后一条
作为控制目标，初始块同样选择最后一条。事件时间允许相等并保留输入顺序，仍拒绝倒序及非法参数。
普通点源、尺寸处理器和监听的备用点源曲线使用相同规则；控制和平滑步长、曲线容量及快照布局不变。
原有每块一条事件的参考语料保持原行为；新增语料验证同块多次更新、首块、同采样点、末块、reset、
任意分块及离线／监听／seek 一致性。语义报告保留所有源事件并标明每块选中的目标。

## 快照与分配边界

数值快照由 Rust 持有，C++ 按需创建并继续管理约每秒保存、LRU 和 seek/loop 锚点。
32 MiB 预算包含 Rust 快照头、状态数组及 C++ list/句柄开销，不复制事件表或整轨 PCM。
预算不足一个快照时继续从零预热；倍率改变清空旧快照，增益和静音不使快照失效。
不同准备表、实例容量或插值配置的快照拒绝恢复。

快照覆盖 pending 输入、滤波历史、事件游标、过渡权重和点源状态。若保存发生在动态曲线
准备后、混音前，额外保存该窗口的起始点源状态，恢复时在预留空间内重建曲线。
正常 stream 检查点仍在原固定块边界保存，不改变寻址或预热策略。

Rust 内核与 FFI 从首次处理开始不分配、不加锁、不做 I/O；探针包括控制更新、短末块、
错误拒绝、reset 和已创建快照的 capture/restore。快照创建、准备/销毁及 C++ I/O/控制发布
不在零分配承诺中，也不将此结论扩大到完整渲染器。

## 验证与复现

测试参考冻结在 `tests/reference/triple_balance/`，保留基线源码 SHA-256；只做命名空间隔离
、格式调整和静态检查注释，包含声像、尺寸处理、运动和混合状态。真实捕获 fixture 继续独立运行。

Release 同平台一般有限值采用 `2e-6 + 2e-6 * abs(old)`；精确路由/简单增益、量化码、
事件及生命周期分支有独立检查。原有 float32 离线/流式、窗口切片、不同 pull 长度和静态
覆盖 policy 的逐位契约继续执行。不同平台之间的逐位一致仍属于二期。

新测试覆盖三布局、量化半步、480 个控制块的状态决策、短块/末块、reset、静音恢复、
快速归零恢复、共享表独立实例、pending/动态曲线快照及错误原子性。
独立内核与 FFI 分配器从首次调用开始计数。

2026-10-05 原生验收：

| 项目 | macOS arm64 | Windows x64 |
|---|---:|---:|
| CTest | Debug 60/60；Release 定向 15/15 | canonical Release 59/59 |
| 17,783,167 个旧/新数值对照 | 最大 PCM 绝对差 `1.3113021850585938e-6` | 全部逐位相同 |
| 480 个控制块的事件与生命周期决策 | 精确一致 | 精确一致 |
| 公开 `adm_*` 符号 | 139，集合保持 | 139，集合保持 |
| 私有 Rust 符号外泄 | 无 | 无 |

其中 7.1.4/9.1.6 的 320 个控制块同时精确核对 Q15 码；22.2 保留连续、无量化的几何规则。

Rust Release workspace 共 90 项测试通过，其中 Triple Balance 7 项、分配探针 10 项、FFI 16 项。
28 组 macOS Release CLI 对照全部通过：13 组逐位相同，15 组尺寸/窗口用例最大 PCM
绝对差为 `8.344650268554688e-7`。这 15 组的 semantic report 同时确认有效三轴尺寸相等、
diffuse 为零且 extent 实际参与渲染。不是主观试听或跨平台位一致结果。
Rust fmt/Clippy、改动 C++ clang-format/clang-tidy/cppcheck、冻结参考检查和许可证/SBOM 检查通过。

实现提交 `31ec95c`，数值探针的逐帧访问修订为 `28a607c`。
[最终三平台 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37278478106)
验证 `28a607c`：macOS Debug 60/60、Linux Debug 59/59、Windows Debug 59/59，全部通过。
此前实现提交的首轮三平台 CI 也全部通过。最终验收提交只更新文档与证据。

```sh
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build --preset release
build/release/mr_adm_triple_balance_rust_tests comparison.json
python3 scripts/consistency/compare-triple-balance.py \
  --reference /path/to/1972817-release/mradm --candidate build/release/mradm \
  --pcm-bits build/release/mr_adm_pcm_bits --fixtures /path/to/fixtures --output audio.json
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base 1972817 --build-dir build/debug
scripts/quality/check-licenses.sh --build-dir build/debug
```

CLI 对照使用语义 policy 将既有 Cartesian fixture 调整为等尺寸并关闭 diffuse，不编辑输入
ADM/WAV。覆盖 13 个既有后端控制用例和 15 个新增尺寸/窗口用例，只保留摘要及指纹。
构建目录、Cargo 产物和依赖缓存均复用。Windows 使用既有 canonical Release 工作区，
同步前校验并备份原始字节；其余已有修改保留，原生结果不描述为干净 Git 检出。

实际结果和源码指纹见[机器可读验收](evidence/rust-triple-balance/validation.json)、
[macOS 数值](evidence/rust-triple-balance/macos-comparison.json)、
[Windows 数值](evidence/rust-triple-balance/windows-comparison.json)、
[CLI 对照](evidence/rust-triple-balance/macos-audio.json)及
[有效语义核对](evidence/rust-triple-balance/semantic-policies.json)。
性能和跨平台位一致不由本次语言迁移结果推断。

## 参考实现退出

- 单元 `triple_balance`（`tests/reference/triple_balance/`）：随默认测试构建编译，由 `mr_adm_triple_balance_rust_tests` 比较。

按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)的通用条件退役；登记与文件哈希见 [`tests/reference/retention.json`](../../tests/reference/retention.json)。当前状态：保留（Rust 实现尚未随正式 tag 发布，独立回归与二期需求待评审）。
