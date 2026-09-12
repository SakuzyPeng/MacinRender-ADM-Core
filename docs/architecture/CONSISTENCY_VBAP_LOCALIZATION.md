# 3D VBAP 数值差异定位：2026-09-09

`saf-5_1_4-extent` 的 Linux/Windows 原始 PCM 差异，已通过逐位复核的重放定位到 `tanf`：只替换这一个返回值，即可复原 Windows 的全部 480,000 个输出采样。macOS 侧还存在独立的 BLAS 旋转、点积和求逆差异；三角化 RNG 也会改变索引顺序乃至几何拓扑。

## 证据来源与校验

- 完整三平台运行：[34322676035](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/34322676035)，提交 `ac43df9` 的完整 SHA 见机器报告 `run.headSha`。六个 Release A/B job 及 Compare platforms 全部成功，A 为 **5/16**、B 为 **8/16**，既有门禁全部通过。
- B 组共八个诊断变体：各平台的原生 RNG / 统一 RNG，以及 macOS 的 OpenBLAS / OpenBLAS + 统一 RNG。两种输入 fixture 在八个变体中 SHA-256 一致。
- 三个平台的 native 诊断 PCM 均与各自未插桩的 B 基线逐位相同。这样，中间检查点能够对应最初发现的两个 PCM 分歧。
- `mr_adm_vbap_probe` 从真实渲染读取源位置、spread、布局与增益。分步调用 SAF 与完整 `generateVBAPgainTable3D_srcs` 比较，再将拆开的 spread / gain 算术逐位对照 SAF 原函数。八变体、两测例、四次 seed 运行共 **64 次探针校验通过**；seed 1 另与真实渲染增益比较，seed 1 重复结果一致。
- 机器报告：[consistency-vbap-localization-2026-09-09.json](consistency-vbap-localization-2026-09-09.json)，包含构建记录、输入哈希、每阶段首个不同位模式、PCM 比较与因子替换结果。

Windows 的探针通过一个 C 桥接文件调用 SAF 内部的 BLAS 接口，避免预编译 OpenBLAS 的 MSVC C 复数类型进入 C++ 头文件。桥接只转发原函数。

## 原始分歧与统一 RNG 后的结果

下表为 B 组不同采样数，每个测例均有 480,000 个 float32 采样（48,000 帧 × 10 声道）。

| 配置与测例 | macOS / Linux | macOS / Windows | Linux / Windows |
|---|---:|---:|---:|
| native，cartesian | 133,910 | 133,910 | 0 |
| native，extent | 190,150 | 144,000 | 94,150 |
| 统一 RNG，cartesian | 96,000 | 96,000 | 0 |
| 统一 RNG，extent | 370,801 | 327,634 | 139,144 |

源参数、扬声器方向和 33 个顶点分量逐位相同。原生 RNG 下，三角形索引首先出现差异：macOS/Linux 的无序三角形集合相同，但索引顺序不同；Windows 的集合也不同。不能把索引重排与几何拓扑变化混为一谈。

统一 RNG 后，54 个三角形索引在三平台及 macOS OpenBLAS 变体中一致，但最终 PCM 仍有上表差异。该干预也可能选中另一条对角线：本次 macOS cartesian 的 native / 统一 RNG 对照有 288,000 个采样不同，`max_abs=0.0354639`。统一 RNG 是定位控制项，尚不是保持既有声音的生产修复。

## Linux / Windows 的 extent：`tanf` 足以解释全部 PCM 差异

极坐标 fixture 的 `Width=30`，现有 `mdap_spread_degrees` 将其乘 60 后饱和为 180°。这是既有的 extent 量纲问题，本轮未改变其渲染行为。

SAF 随后计算 `spread_rad=(spread/2)*pi_f/180`，得到相同的 binary32 输入：`0x3fc90fdb`，即 `1.5707963705062866`。它略大于真实的 π/2，因而 `tanf` 返回一个很大的负值：

| 平台 | `tanf` 返回位模式 | 返回值 |
|---|---|---:|
| Linux | `0xcbae8a4b` | -22,877,334 |
| Windows / macOS | `0xcbae8a4a` | -22,877,332 |

Linux/Windows 的旋转矩阵、首个 ring 向量和 SGEMM 生成的全部 24 个 ring 分量一致。只有 tangent 相差 1 ULP，随后 27 个 spread 方向分量中有 19 个不同。保持 Linux 的 ring 不动，替换 tangent，即可逐位得到 Windows 的全部 spread 方向。

`replay-vbap-gains.py` 先逐位复核两端的 486 个 dot、归一化前增益、范数、最终增益及完整 PCM。对本批 Linux/Windows 的长度 3 点积，`f32 乘积 → f64 顺序累加 → f32 窄化` 的算术模型匹配所有原始 dot；只有复核成功才执行因子替换。结果如下：

| 保留 Linux 计算，其余替换为 Windows 数据 | native 对 Windows 的不同采样 | 统一 RNG 对 Windows 的不同采样 |
|---|---:|---:|
| 不替换 | 94,150 | 139,144 |
| 仅替换 tangent 返回值 | **0** | **0** |
| 仅替换三角形索引与逆矩阵 | 94,150 | 139,144 |

这是经过原始计算逐位校验的算术重放，未修改输入 ADM，也未将 Linux 渲染器重新链接到另一套 libm。它解释了这两个配置下该 fixture 的全部 Linux/Windows PCM 差异；不构成任意内容或任意长度 BLAS 运算的契约。

求逆差异仍真实存在：统一 RNG 后，相同顶点和索引产生的 162 个逆矩阵元素中，Linux/Windows 有 3 个不同，第一个位于下标 93：`0x00000000` 与 `0xb2aaaaaa`（约 `-1.9868214e-8`）。但单独替换这些几何数据没有消除上述 PCM 差异；cartesian 的最终 PCM 在两平台甚至始终相同。

## macOS：SGEMM 与 SDOT 是独立的差异边界

macOS 的 Accelerate/OpenBLAS 对照保持源参数、三角形索引、spread 三角函数值、旋转矩阵和首个 ring 向量一致。`getSpreadSrcDirs3D` 内首次不同出现在 `cblas_sgemm` 的 ring 旋转输出：

| 测例 | 首个不同 ring 分量 | Accelerate | OpenBLAS |
|---|---:|---|---|
| cartesian | 3 | `0x3d82d788` | `0x3d82d787` |
| extent | 5 | `0xbf3504f3` | `0xbf3504f2` |

求逆路径也有独立差异。统一 RNG 后，macOS/Linux 的逆矩阵有 14 个位模式不同，其中 10 个是数值差异，其余为零的符号差异。这与上述 SGEMM 的固定输入实验是两条独立证据。

进一步在本地 arm64 OpenBLAS 0.3.31 中重放 CI macOS 的**同一份逆矩阵与 spread 方向**，`utility_svvdot → cblas_sdot` 仍不同：extent 为 119/486 个 dot，cartesian 为 100/486 个。以下两种明确的算术模型精确复现本批 dot：

- CI macOS Accelerate：先将三个乘积分别舍入到 f32，再按 `(p0+p2)+p1` 以 f32 累加；初始 `+0` 的处理也保留。
- 本地 OpenBLAS：按 0、1、2 顺序执行三次显式 f32 FMA，从 `+0` 开始。

固定输入后，切换到前一种算术模型，两个测例的全部 dot 与 11 维增益（含两个 dummy）均恢复为 CI macOS 位模式。后一种模型则逐位复现本地 OpenBLAS 的实际调用结果。具体计数与表达式保存在机器报告的 `local_openblas_replay_of_ci_macos_inputs` 中。

这证明了点积算术本身的影响，范围限定于此次捕获的长度 3 输入。项目的严格 FP 编译选项没有约束系统预编译 BLAS 内部的 FMA 与求和顺序。

## 复现

触发完整的同批次平台采集：

```sh
gh workflow run consistency.yml --ref claude/rust-rewrite-difficulty-2dzh3x \
  -f localize=true -f vbap_only=true
```

重放本次 Linux/Windows extent 证据（`mr_adm_pcm_bits` 使用 Release 构建）：

```sh
gh run download 34322676035 --name consistency-linux-b --name consistency-windows-b \
  --name consistency-macos-b --dir artifacts/vbap-34322676035
vbap_artifacts=artifacts/vbap-34322676035
vbap_linux="$vbap_artifacts/consistency-linux-b/localization/native/vbap/kernels/saf-5_1_4-extent/seed-1"
vbap_windows="$vbap_artifacts/consistency-windows-b/localization/native/vbap/kernels/saf-5_1_4-extent/seed-1"
python3 scripts/consistency/replay-vbap-spread.py "$vbap_linux" "$vbap_windows"
build/release/mr_adm_pcm_bits extract \
  "$vbap_artifacts/consistency-linux-b/fixtures/objects-extent.wav" /tmp/vbap-input.pcmbits
python3 scripts/consistency/replay-vbap-gains.py "$vbap_linux" "$vbap_windows" \
  --input-pcm /tmp/vbap-input.pcmbits \
  --first-pcm "$vbap_artifacts/consistency-linux-b/pcm/saf-5_1_4-extent.pcmbits" \
  --second-pcm "$vbap_artifacts/consistency-windows-b/pcm/saf-5_1_4-extent.pcmbits"
```

离线脚本会拒绝不能逐位复现的原始检查点。C++ 探针也可用 `MR_ADM_TRACE_DIR=out build/release/mr_adm_vbap_probe <本地渲染检查点> 1 <参考 kernel 检查点>` 对照固定几何、逆矩阵、spread 以及两种点积算术。

## 后续修复范围

生产修复需要分别处理稳定三角化与索引顺序、规范的小矩阵/点积运算，以及极坐标 extent 的单位与接近 π/2 的 spread 计算。只统一 RNG 或只替换求逆函数都不能使这两个测例三平台收敛。本轮新增诊断与证据，未替换默认渲染数学路径，也未扩大一致性门禁。
