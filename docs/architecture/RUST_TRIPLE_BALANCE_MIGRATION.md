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

实际平台结果、实现提交及 CI 提交收录于同目录 evidence/rust-triple-balance 验收记录。
性能和跨平台位一致不由本次语言迁移结果推断。
