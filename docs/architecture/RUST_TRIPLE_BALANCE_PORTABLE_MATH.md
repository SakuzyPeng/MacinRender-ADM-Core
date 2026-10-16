# Triple Balance 可移植数学函数与文件流式重放（三期 3.4）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)三期 3.4，审计项 N12，以及 S07 中 Triple Balance 的部分。

## 范围

`mradm-dsp` `triple_balance::panner` 的点声源、22.2 room、size 积分格点与 direct / spread 混合原先以 `const PORTABLE: bool`
泛型提供两套实现：Scene 流式渲染的 `live_*` 入口取 `true`（`libm`，一期 1.3），离线与文件流式入口取 `false`，调用平台的
`f64::sin/cos` 与 `f32::sin/cos/powf`。离线入口的 size 积分每次重新计算，Scene 入口则读取预先算好的格点。另有两个平滑常量
`1 - exp(-512/1200)`（位置）与 `1 - exp(-512/960)`（size）用 `f32::exp` 计算，常量参数可能由编译器按宿主库折叠（N12）。

3.4 去掉 `PORTABLE` 泛型：

| 位置 | 原实现（离线、文件流式） | 3.4（全部标准模式入口） |
| --- | --- | --- |
| `point`、`room`、`room_mix`、`raw`、`size_mix` | `PORTABLE=false`：平台 `sin/cos/powf` | 唯一实现：`libm::sin/cos/sinf/cosf/powf` |
| size 积分格点 | 每次调用重新积分 | 与 Scene 共用一次构建、跨会话共享的格点（`OnceLock`，Scene 在进入无分配处理边界前预先构建） |
| `Position::advance`、`Processor::control` 的平滑系数 | `f32::exp` | `libm::expf` |

`live_*` 入口随之并入同名函数，Scene、离线与文件流式渲染从此是同一份代码。D 模式（`d_mode`）自引入起就使用 `libm`，
3.4 只为它建立独立的内核与 PCM 门禁。`tests/triple_balance.rs` 扫描 `src/triple_balance` 下的全部源文件，平台初等函数重新出现即失败。

C++ 侧（`src/adm_render_triple_balance/`）只做输入校验、床路由与编排；文件流式覆盖中唯一的浮点运算是尺寸比例的单次乘法，
不受乘加融合影响。

## 数值影响（Linux x64）

内核探针在迁移前后对比：size 积分格点 72,900 个值中 46 个改变（最大差 6.0e-8）；点声源、22.2 room、混合与 D 模式的值不变。
两个平滑常量的 `libm::expf` 与 `f32::exp` 结果逐位相同。

PCM：见下文验证。

## 新增用例

原有 Triple Balance 离线用例都使用 `--speaker-spread-mode none`，size 路径与 D 模式没有 PCM 覆盖。一致性夹具新增 `objects-room`：
一个 Cartesian 对象，四个 250 ms 块在房间内移动，宽 = 高 = 深依次为 0 / 0.25 / 0.625 / 0.125，diffuse 为 0（Triple Balance 只接受
等尺寸 extent）。

| 用例 | 输出 | 说明 |
| --- | --- | --- |
| `triple-size-714` / `triple-size-916` | 7.1.4 / 9.1.6 | 标准模式 size 路径（积分格点与混合、位置与 size 平滑） |
| `triple-size-222` | 22.2 | room size 路径 |
| `triple-size-window` | 9.1.6，0.2—0.7 s | 带 preroll 的窗口渲染 |
| `triple-d-916` / `triple-d-222` | 9.1.6 / 22.2 | D 模式 |
| `stream-triple-{714,916,222}-*` | 7.1.4 / 9.1.6 / 22.2 | 文件流式：对象增益、联动的等比 size 缩放、seek、静音、清空 |

离线用例标注 `slice: 3.4`，记在二期冻结参数集之外；`mr_adm_phase2_render` 新增 `--triple-balance-mode`。文件流式重放沿用 3.2 的规则：
固定与碎片分块分别成例，同进程两遍、新进程与两种分块逐位相同，每个用例交付 74,200 帧。PCM 门禁由 216 个增至 228 个。

## 内核门禁

| 文件 | 内容 |
| --- | --- |
| `triple-balance.10-point.f32` | 9 × 9 × 5 的位置网格在 7.1.4、9.1.6、22.2 上的点声源增益 |
| `triple-balance.20-room.f32` | 同一网格、4 个 size 的 22.2 room 增益与 direct / spread 混合 |
| `triple-balance.30-size.f32` | 量化后的内部坐标、5 个 size 的积分格点增益与混合 |
| `triple-balance.40-d-mode.f32` | D 模式在 9.1.6 与 22.2 上、4 个 size 的增益 |

4 个文件全部设为门禁（模式 `triple-balance.*`）。内核文件由 224 个增至 228 个，门禁由 219 个增至 223 个。

## 验证
