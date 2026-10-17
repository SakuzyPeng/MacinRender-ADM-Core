# 覆盖先行（四期 4.2）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)四期 4.2。
> 本切片只增加用例、门禁与一致性工具的能力，不改动生产代码，也不改变既有的 PCM 或内核输出。
> 它为 4.3–4.8 的结构迁移补上门禁覆盖：导入投影、语义策略、各后端的平滑与后置增益，以及 `room-binaural`。

## 新增内容

| 部分 | 内容 | PCM id | 内核 |
| --- | --- | ---: | ---: |
| ① 观察内核设门禁 | `ear-*.10-layout.f64`（3 份 EAR 布局表）与 `scene.*`（Scene 空间数学） | — | 5 份转为门禁 |
| ② 离线平滑与后置增益 | VBAP / HOA / 双耳的 2048 帧离线对象平滑；VBAP 与 Triple Balance 的 −23 LUFS 响度目标和 −6 dBTP 峰值归一 | 7 | — |
| ③ 语义策略与 DS 矩阵 | 5 个带语义策略的离线用例、2 个 DirectSpeakers 矩阵用例、6 个带元素身份与本用例策略的 Scene 回放 | 7 + 12 | — |
| ④ `room-binaural` exact 档 | 3 个离线用例、4 个文件流式重放、4 个 Scene 回放，以及 `room.*` 内核 | 3 + 4 + 8 | 2 |

合计：PCM id 从 242 个增至 283 个（离线 79、Scene 88 个配置各两个 epoch、文件流式 28），
内核测量从 235 份增至 237 份并全部设为门禁，门禁 308 条（283 条用例、25 条内核模式）。

### ② 离线平滑与后置增益

| 用例 | 渲染器与布局 | 内容 |
| --- | --- | --- |
| `smoothing-saf-714` / `smoothing-hoa3` / `smoothing-binaural` | VBAP 7.1.4 / HOA3 / 双耳 | `objects-room` 四个 250 ms Cartesian 块，`--object-smoothing-frames 2048` |
| `saf-loudness-minus23` / `saf-peak-minus6` | VBAP 7.1.4 / 5.1 | 响度目标 −23 LUFS；峰值归一到 −6 dBTP |
| `triple-loudness-minus23` / `triple-peak-minus6` | Triple Balance 7.1.4 / 9.1.6 | 同上 |

EAR 已有平滑用例 `ear-window`；Triple Balance 拒绝离线平滑，不加平滑用例。本机确认三个平滑用例与同参数不平滑的渲染
不同，即平滑确实生效。

### ③ 语义策略与 DirectSpeakers 矩阵

新增两种 fixture（`tests/tools/make_fixture.cpp`）：

- `policy-objects`：4 个命名 AudioObject，分属两个 content，归同一个 programme。
  - AO_1001 “Dialogue Lead”：importance 9、dialogue 1、channelLock（maxDistance 0.5）、jumpPosition（50 ms）；
  - AO_1002 “Dialogue Alt”：importance 6、dialogue 1、objectDivergence 0.5 / 30°；
  - AO_1003 “Ambience Wide”：importance 3、extent 与 diffuse，第二块 headLocked；
  - AO_1004 “FX Cartesian”：importance 5、Cartesian、screenRef、等尺寸 extent。
  - 每个对象有两个 500 ms 块，位置在块间变化。
- `directspeakers-lfe`：5.1 声道床，LFE1 的 audioChannelFormat 带 `frequency typeDefinition="lowPass"`（120 Hz），
  让策略的 `direct_speakers.lfe` 过滤能命中（渲染器按标签识别 LFE，原 `directspeakers` 没有 lowPass）。

JSON 输入定义在 `scripts/consistency/phase2_common.py` 的 `JSON_FIXTURES`，采集时用 `save()` 写进 `fixtures/`，
与其他 fixture 一起登记哈希；比较器要求 manifest 中的 `json_fixtures` 与目录内容都与清单一致。
用例的新字段 `files` 把选项映射到这些文件，CLI 采集展开为 `--flag fixtures/<name>`，
`mr_adm_phase2_render` 填写与 CLI 相同的 `RenderOptions` 字段。

| 用例 | 渲染器 | 覆盖 |
| --- | --- | --- |
| `policy-identity-ear-714` | EAR 7.1.4 | `id`、`name_glob`、`content`、`programme`、`importance_min/max`、`dialogue_id`、`track_uid` 匹配，global 后按数组顺序合并、叶字段后写有效（FX 先按名字静音、再按轨道 UID 取消静音） |
| `policy-lock-saf-714` | VBAP 7.1.4 | `channel_lock`（收紧距离、启用、关闭）、`divergence` 缩放与范围上限、`interpolation.max_ms` 与 `honor_jump_position` |
| `policy-position-binaural` | 双耳 | 绝对位置、`offset`、`lock_azimuth` / `lock_elevation`，Cartesian 对象经策略转为极坐标，按 content 头锁定，忽略 jumpPosition |
| `policy-direct-speakers-ear-51` | EAR 5.1 | global 的 `lfe` 规则、按标签的增益 / 位置 / 头锁定 / 静音规则，逐条按各自过滤应用 |
| `policy-triple-714` | Triple Balance 7.1.4 | 等比 extent 缩放与上限、增益、关闭 diffuse |
| `ds-matrix-ear-51` / `ds-matrix-saf-51` | EAR / VBAP 5.1 | `--direct-speakers-routing matrix`：加权、拆分与静音路由覆盖全部非 LFE 声道 |

每条对象规则都至少匹配一个对象，渲染不产生未匹配规则的警告；本机语义报告确认各规则改变了对应字段。
这些用例的 `render-scene/offline.06-policy.i32` 是第一批与 `05-import` 不同的场景检查点，4.4 迁移语义策略时按它们对照。

Scene 回放新增版本 5（`mr_adm_phase2_scene`）：用例带 `semantic.identity`（元素的 ADM 身份：AO_1001、名称、
轨道、importance、dialogue、content 与 programme）和 `semantic.policy`，后者在 6144 处取代写死的 global 策略。
VBAP 4+7+0、双耳 cloud、Triple Balance 7.1.4 size 各一，固定与碎片分块各一份。策略按名称、content、importance
与轨道 UID 匹配对象规则，VBAP 与双耳另带按 programme 的极坐标偏移（Triple Balance 保持房间立方体内的 Cartesian 位置，
不加偏移）。VBAP 与 Triple Balance 的两份按既有规则要求固定与碎片分块逐位相同。

### ④ `room-binaural` exact 档

**房间包。** `tests/fixtures/room/phase2/` 提交 `tests/unit/room_fixture.py` 生成的非个人合成包：`room.json`、
`hrir.json` 及 9 个资产，共 390,780 字节，其中 `phases.f32` 占 332,100 字节。传播相位表用 Python `math.sin/cos`（平台 libm）生成，
因此提交的字节就是各平台的输入，由 `phase2_common.ROOM_FIXTURES` 逐文件钉住 SHA-256，`.gitattributes` 对该目录设
`-text`，source fingerprint 也把它当作二进制。重新生成只用维护脚本 `tests/tools/make_room_phase2_fixtures.py`，
改动后同步更新哈希。采集把包复制到 `fixtures/room/`，比较器校验 manifest 的 `room` 与目录内容。

| 用例 | 内容 |
| --- | --- |
| `room-exact` | `objects-extent-multi`，包内默认 HRIR |
| `room-exact-sofa` | `objects-room`，以已钉住的 `simple.sofa` 作为听者 HRIR |
| `room-exact-screen` | `policy-objects`，`--screen-config` 给出参考屏幕与重放屏幕，覆盖屏幕 JSON（N01 的屏幕残留） |
| `stream-room-extent-*` / `stream-room-cartesian-*` | 文件流式版本 4：带 cloud 与不带 spread 两组，各固定与碎片分块；实时覆盖、头姿、seek 与 4441 帧房间尾声 |
| `scene-room-binaural-cloud{0,1}-48000-48000-*` | Scene 回放版本 6：48 kHz，exact 档，带房间尾声，不切换后端 |

文件流式与 Scene 用例都在用例里写明房间尾声（`ROOM_TAIL_FRAMES = 4441`），工具按它计算期望帧数，
并核对渲染器报告的 `appended_tail_frames()`，所以帧数不依赖实测输出。房间文件流式同样要求固定与碎片分块逐位相同。

`room.*` 内核（`rust/crates/mradm-ffi/examples/phase2/room.rs`）直接用 Rust API 对提交的包做 exact 档准备：
`room.10-hrir.f32` 是 6 个方向的时域 HRIR 查询，`room.20-impulse.f32` 是 4 组位置与头姿的直达和早期响应。

**未纳入：换房。** 计划中的“换房”用例没有加入。换房是异步的：`IRoomControl::request` 交给准备线程，
新房间在准备完成后的下一次 `process` 才开始过渡，开始的样本取决于线程调度，回放无法把它钉在固定帧上。
要覆盖它，需要在生产接口上增加同步点，这超出 4.2 不改生产代码的范围；`mradm-dsp` 的 `room_replay` 冻结回放
仍在私有预置上同步排队覆盖这条路径。

## 发现

**Live 立体声 VBAP 在扬声器对之外静音。** `0+2+0` 只有 ±30° 两只扬声器，`mradm-dsp` `vbap.rs` 的二维配对
只有前方一对能覆盖，声源方位落在 ±30° 之外时所有对都给出负增益，输出为零，而不是落到最近的扬声器。
4.2 的 Scene 双耳回放在 8192 处切到立体声 VBAP，最初的策略偏移把声源移到前方中心以外时，回放工具的“活动区间静音”检查
报错，由此发现。离线 VBAP 不支持 `0+2+0`，因此只影响 Scene 流式渲染和实时监听。这是既有行为，4.2 不修改；
用例的偏移改为留在扬声器对之内，是否修正留给维护者决定。

## 工具与目录的改动

- `mr_adm_make_fixture`：`policy-objects`、`directspeakers-lfe`。
- `mr_adm_phase2_render`：`room-binaural` 渲染器，`--direct-speakers-routing matrix`、`--room-quality`，以及经 `files`
  解析的 `--semantic-policy`、`--direct-speakers-matrix`、`--screen-config`、`--room`、`--room-hrir`；字段映射与 CLI 相同。
- `mr_adm_phase2_stream`：版本 4（房间包与尾声）。
- `mr_adm_phase2_scene`：版本 5（语义身份与本用例策略）、版本 6（`room-binaural`）。
- `run-rust-phase2.py` / `compare-rust-phase2.py`：写出并校验 JSON 输入与房间包；fixture 清单改为按相对路径递归登记。
- `phase2-gates.json`：新增 `ear-*.10-layout.f64`、`scene.*`、`room.*` 三条内核门禁与 41 条用例门禁。
- 这些都是一致性工具，计入 `tests` 区域；生产 C/C++ 行数不变（45,090 行，`language-surface.py --check` 通过）。

## 验证

**本机复核（2026-10-16，macOS arm64）。** 修正比较器漏计 Scene 房间尾声、JSON 输入在 Windows
文本模式下产生 CRLF，以及三个回放工具缺少 `ADMRenderRoom` 直接链接依赖的问题。JSON 统一按 UTF-8/LF
字节写出；新增回归测试覆盖 LF/CRLF 环境下的文件字节一致性，以及两个进程、两次重复、两个 epoch 的尾声帧数。

Debug 和 Release 均以 `BUILD_SHARED_LIBS=ON` 成功构建 `mr_adm_phase2_tools`。Debug 的
`mr_adm_phase2_tool_tests` 与 `mr_adm_consistency_build_tests` 两套 CTest 通过；变更文件的 clang-format、
clang-tidy、cppcheck，以及 Rust fmt、C/C++ 规模棘轮检查通过。

Release 使用 config A（额外开启共享库）完成非诊断采集：79 个离线用例、88 个 Scene 配置、28 个文件流式用例，
共 283 个 PCM 输出、237 个内核文件。同进程/新进程重复性、规定的分块一致性及比较器 `validate()` 全部通过。
源码指纹为 `5b31e7c167bba07171c39e8a684b7894a11052861a55ab2d81880e821580e2f8`。
此次复核未执行诊断无扰动采集或三平台比较。

**三平台。** 按四期集中验收的约定，4.2 不单独等待三平台运行，结果回填到本节。
