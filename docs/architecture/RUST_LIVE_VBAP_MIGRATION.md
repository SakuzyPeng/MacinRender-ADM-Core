# Rust Live VBAP 混音与独立渐变迁移

> 算法基线 `577fe2d`，共享集成基线 `283c54d`。Live Scene VBAP 数值状态与混音由 Rust 接管。

## 所有权与接口

`mradm-dsp::live_vbap::Mixer` 按 generation 的原始元素顺序持有当前/目标声像增益、步长和
剩余样本数，以及独立的电平状态。算法库禁止 unsafe。C++ 保留描述符、ID 映射、语义目标、
初始化标记、路由及诊断，释放原数值状态；已有 Rust VBAP/MDAP panner 继续生成目标系数。

C++ 在临时控制状态上编译初始化命令和有序更新，准备借用的单声道 PCM 视图；每次 render
只调用一次混音 FFI。Rust 按事件区间处理，保留每个输出样本的源累加顺序，同偏移命令不
合并。空区间直接跳过，避免同偏移更新反复扫描全部元素。PCM 不复制进 Rust，也不跨调用保留。

私有边界包含 create/destroy/reset/process 和测试用只读状态拷贝。命令使用固定宽度索引、
偏移、时长和系数偏移；所有数组都带显式长度。C++ 以可移动 RAII 管理，generation 配置
准备成功后整体替换，失败保留旧配置。公开 C ABI、renderer 接口、布局、采样率、依赖和
构建开关均不变。共享空间数学、Scene 外层过渡及双耳控制状态留到后续。

## 行为与错误契约

- 两套渐变均保留 float `(target-current)/N`，先输出当前样本再逐样本加步长，最后精确设为目标。
  重复目标重启对应渐变；不复用采用另一种端点规则的 LiveGainRamp。
- 初始化立即生效。更新时 jump 优先，其次非零显式时长，最后默认 smoothing。
  电平、声像各自拥有期限，单独 head_locked 更新继续中性；字段清除保持原有效位语义。
- 事件在对应样本混音前按原顺序应用。PCM 使用 `(input * spatial_gain) * level`，按 generation
  元素顺序累加；缺失/无信号平面只推进历史。零增益不跳过乘法，不增加非有限值清洗。
- render 验证成功后清零请求范围，再累加所有源，输出尾部不变；空 generation 输出静音。
  零帧不推进渐变，但内部初始化命令仍立即生效。公开 Scene 继续拒绝零时长和非有限 PCM。
  EOF 不追加尾音，暂停、epoch、预热、generation 0 和后端切换调度保持原样。
- 整帧先验证 ID、平面长度、事件顺序/范围、字段掩码、目标/系数有限性、尺寸、溢出及别名，
  再修改输出和数值历史，成功后提交语义状态。policy 扩展后的位置/extent 可以超出原始
  producer 输入范围，私有数值校验不缩窄这部分合法输入；公开输入范围仍由 Scene 入口检查。
- 成功帧的诊断按原顺序延后发布；失败不消耗 warn_once 状态，也不改变初始化标记。
  late-invalid 更新、路由失败、短缓冲和配置失败均有恢复测试。OOM 和内部前置条件错误
  遵循 ADR 0005；panic 捕获沿用现有边界，不承诺对 panic/OOM 进行事务回滚。

Rust 内核和 FFI 的处理、reset、状态拷贝及参数拒绝从首次调用起无分配、无锁、无 I/O。
准备、销毁和测试状态克隆在此范围外。C++ worker 的语义、panner 查询、事件/系数传输及
诊断准备允许按需分配和复用容量；没有新增最坏容量预分配，不承诺整个 renderer 零分配。

## 验证

`tests/reference/live_vbap/legacy.h` 冻结完整基线 renderer，`ramps.h` 单独提取数值状态和循环。
两者只用于测试，引用的现有语义/panner 辅助函数未在本批迁移。来源及文件 SHA-256 见
同目录 provenance.json。

Release 同平台对照包含 3,971,688 个浮点值与 6,760 个剩余计数，覆盖 0/1/3/9 元素、
1/2/6/12/24 输出声道、0～1025 帧不规则块和 2.0/5.1/7.1.4/9.1.6/22.2 实际路由。

| 项目 | macOS arm64 Release | Windows x64 canonical Release |
|---|---:|---:|
| 相同目标下混音 PCM 最大绝对差 | `2.384185791015625e-7` | 0 |
| 完整 renderer 新旧 PCM 最大绝对差 | 0 | 0 |
| 当前/目标/步长共 101,400 个值 | 逐位相同 | 逐位相同 |
| 两套渐变共 6,760 个剩余计数 | 完全一致 | 完全一致 |

一般有限 PCM 门限为 `2e-6 + 2e-6 * abs(reference)`；状态、简单路由和增益逐位检查。
独立 Rust 测试另验证事件端点、重复目标重启、同偏移顺序、独立期限、静音推进、零帧初始化、
reset、实例隔离、分块一致、源累加顺序、非有限传播和错误原子性。内核与 FFI 各有首次调用
分配探针；私有状态拷贝只用于测试，生产不逐样本查询。

原生对照覆盖 Objects、DirectSpeakers 位置覆盖/清除、LFE/split-power、divergence、extent、
channel lock、active、head_locked、诊断去重、移动所有权及整帧失败后恢复。现有公开 Scene
渐变回归增加双声道逐位包络断言，保留跨 SceneFrame 和同偏移事件契约。

libadm 更新完成后，重新使用标准构建验证共享工作区：macOS Debug 全套 63/63、Release 定向
9/9、Rust Release workspace 128 项，Windows canonical Release 全套 62/62。
[最终三平台 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37342603203)
验证源码提交 `de57300`：macOS Debug 63/63、Linux Debug 62/62、Windows Debug 62/62。
实现提交为 `41cf469`；首次 Linux 构建发现 C API fixture 依赖已移除的间接 `<cmath>`，
`de57300` 补齐直接包含，并在两端原生配置复查 C API 后重新通过三平台 CI。
最终验收提交只更新文档和证据。源码/测试指纹、配置及完整结果见
[机器可读验收记录](evidence/rust-live-vbap/validation.json)。
C++ 质量检查保留 Scene 测试中未修改函数的两条既有可读性建议。

## 复现与工作区记录

```sh
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build --preset release
build/release/mr_adm_live_vbap_rust_tests comparison.json
ctest --test-dir build/release -R '(live_vbap|scene_.*c_api|vbap|realtime|direct_speakers_matrix|lfe_routing|semantic_policy)' --parallel 8 --output-on-failure
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base 577fe2d --build-dir build/debug
scripts/quality/check-licenses.sh --build-dir build/debug
```

复用既有构建及共享 Cargo 缓存。Windows 原工作树的修改保留，相关文件同步前核对指纹并
备份；验收不把该目录描述为干净检出。

本地开发期间曾与 libadm 迁移交叉。按用户选择，等待其 `283c54d` 完成后重新核验共享
工作区，并执行完整的标准 Debug/Release 构建、回归、数值对照、质量与许可证检查。
最终结果替代此前阶段性记录，不依赖旧的源码覆盖或手动编译产物。Windows 重新核对的
42 个相关源码指纹全部匹配，无需再次覆盖文件；保留其当时的 SOFA/IAMF OFF 配置。
本批单独提交在 `codex/rust-live-vbap`，原 libadm 提交和其分支保持独立。

本批不增加第三方依赖，不推断性能、RSS 或跨平台逐位一致；后者继续留到二期。

## 参考实现退出

- 单元 `live_vbap`（`tests/reference/live_vbap/`）：随默认测试构建编译，由 `mr_adm_live_vbap_rust_tests` 比较。
  引用 `scene_numeric/spatial.h`，因此 `scene_numeric` 须等本单元退役后才能退役。

按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)的通用条件退役；登记与文件哈希见 [`tests/reference/retention.json`](../../tests/reference/retention.json)。当前状态：保留（Rust 实现尚未随正式 tag 发布，独立回归与二期需求待评审）。
