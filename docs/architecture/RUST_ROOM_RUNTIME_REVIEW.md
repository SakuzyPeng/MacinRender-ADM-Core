# ROOM 运行时迁移审查与对照

审查对象为 `8510d15f`（4.6 E：ROOM 运行时、文件流式渲染和 Scene 流式渲染迁入 Rust），
修复提交为 `158fc3e5`。输出对照参考实现为主线 `72c188b1` 的 C++ `room_internal::Runtime`、
`RoomStream` 和 `LiveRoomRenderer`。本次证据记录于 2026-10-25。

## 修复

- 覆盖参数校验原先通过 FFI 借用整个可变 `FileStream`，与 `IRenderStream` 允许的控制线程校验／工作线程
  处理并发契约冲突。独立的 `validate_overrides` 和私有 FFI 现在不接收流句柄；工作线程设置目标也复用
  同一校验逻辑，保留错误码、消息和 ROOM 浮点环境。
- 状态查询仅独占借用 `RenderRoomControl.message`，请求和声源查询仅借用 `control` 字段。
  C++ 继续串行化状态调用及消息复制，状态轮询与其他控制请求可以并发。
- 待销毁实例队列改为八槽固定数组。准备线程取走队列后，工作线程仍有完整容量，避免下一次停放实例
  时重新分配。实例仍在准备线程的锁外销毁，原有容量上限和提前返回规则保持不变。

新增测试覆盖并发校验与 seek／处理的 PCM 重复性、状态轮询与换房请求／声源查询，以及独立字段借用。
分配探针只计算渲染线程 `process()` 内的分配：零声源、零帧泵送三次同房切换，`8510d15f` 为三次分配，
修复版为零。该探针隔离的是队列容量回归，不代表整个宿主工作线程或完整会话零分配。

## 对照方法

macOS arm64 和 Windows x64 分别使用既有 Release 构建、Cargo 与 FetchContent 缓存。
Windows 使用 canonical MSVC／Ninja 构建树。两平台关闭一致性插桩和 ROOM CUDA；Windows 保持通用
SOFA feature 关闭，但原生 ROOM SOFA 仍由离线用例覆盖。

| 每个平台的覆盖 | 用例数 | 检查 |
| --- | ---: | --- |
| ROOM 离线渲染与 CLI | 9 | API 两次重复、CLI 与 API PCM 一致；点源、DirectSpeakers、LFE、同起点块、窗口／增益、i24、屏幕和 SOFA |
| ROOM 文件流式渲染 | 16 | 四种素材 × cloud／none × 固定／碎片分块；实时覆盖、头姿、seek、FIFO、完整尾声和独立帧数预期 |
| Scene 流式渲染 | 74 | 每项两个 epoch、两次重复；ROOM、VBAP、Triple Balance、内置双耳、采样率转换、后端切换和输出 DSP；比较状态、事件及输入 |
| ROOM 非默认浮点环境 | 24 | 三种素材 × 四种舍入模式 × 下溢清零开关；两次重复及调用方环境恢复 |

每个平台共比较 197 份最终 PCM（9 + 16 + 148 epoch + 24）。Scene 用例取自既有目录，排除依赖通用
SOFA feature 的 14 项，以保持两平台用例一致。异步房间切换的开始时刻不固定，不纳入逐位重放；其请求、
失效、容量和生命周期行为由 ROOM 集成测试及 Rust 回归测试检查。

沿用 [ROOM 准备迁移采集器](evidence/room-preparation-review-20261025/collect.py) 和
[浮点环境驱动](evidence/room-preparation-review-20261025/room_fenv.cpp)，参数固定在
[用例目录](evidence/room-runtime-review-20261025/catalog.json)。采集后删除临时 PCM，保留二进制、素材、
输出及时间线指纹。本次为正确性和 PCM 对照，不作性能或实时零超时验收结论。

## 结果

两平台各 197 份 PCM 均与 `72c188b1` 逐位相同，`158fc3e5` 的这些输出在两平台间也逐位相同。
素材和用例目录指纹、Scene 状态／事件／输入、浮点环境检查全部一致。完整结果及文件指纹在
[summary.json](evidence/room-runtime-review-20261025/summary.json)，四份平台／版本记录保留独立采集证据。

- macOS：49 项 FFI、6 项 render 库、6 项 ROOM 运行时、1 项队列分配测试通过；Debug 的 ROOM、Scene C API、
  Scene 输出 C API、实时引擎四组集成测试，以及 Release ROOM 集成测试通过。
- Windows：49 项 FFI、6 项 render 库、8 项 ROOM 事件表、6 项 ROOM 运行时、1 项队列分配测试，
  以及 canonical Release ROOM 集成测试通过。
- 本地 Rust 格式、两个 crate 的全部 targets Clippy（`-D warnings`）、改动 C++ 的格式／clang-tidy／cppcheck、
  私有 FFI 头校验（331 个函数、135 个结构体）及 C/C++ 规模棘轮通过。

两平台均恢复执行前的工作区、构建配置和标准目标，原分支保持干净。未跟踪的构建、测试和采集日志保存在
`local/review-phase46e-20261025/`。对照记录未包含完整 CTest、通用 SOFA Scene 或 ROOM 96 kHz 矩阵验收。
