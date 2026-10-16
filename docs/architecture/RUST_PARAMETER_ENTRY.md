# 参数入口（三期 3.5）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)三期 3.5，审计项 N01、N02、N03 剩余部分与 S08。

## ADM 导入（N02）

`mradm-adm` 把 `gainUnit="dB"` 的增益换算为线性值时用 `10f64.powf(dB / 20)`，把 Cartesian DirectSpeakers 位置投影为方位、仰角与距离时用
`f64::atan2`。两者都调用平台 C 库。3.5 把它们移入 `mradm_adm::numeric`（`gain_from_db`、`cartesian_to_polar`），改用 `libm::pow`
与 `libm::atan2`；`sqrt` 正确舍入，保持不变。`tests/portable_math.rs` 扫描 `mradm-adm` 的源文件，平台初等函数重新出现即失败。
至此，各 Rust crate 的生产代码不再调用平台初等函数（只剩测试代码中的参考计算）。

在 Linux x64 上，内核探针的 2,886 个 dB 值与 4,913 个 Cartesian 位置换算后的 binary32 结果与 glibc 逐位相同。

## 命令行数字与标签（N01）

CLI11 2.5.0 把浮点选项先用 `strtold` 解析再转换为目标类型。`long double` 在 macOS arm64 与 Windows x64 上是 64 位、Linux x64 上是
80 位、Linux arm64 上是 128 位，同一段文字可能经历不同的二次舍入；`float` 选项还要再从 `long double` 舍入一次，且 `strtold` 跟随进程
locale 的小数点。HOA 渲染器解析扬声器标签方位时用 `std::stof`，同样跟随 locale。

3.5 新增 `mradm_dsp::decimal`（C 边界 `mradm_dsp_parse_decimal`，C++ 包装 `dsp::parse_decimal<T>`）：Rust 的 `str::parse` 在所有平台上
正确舍入，与 locale 无关；binary32 参数直接从十进制文字舍入一次。只接受有限值，忽略首尾 ASCII 空白，保留 CLI11 接受的数字分隔符
`_` 与 `'`；不再接受十六进制浮点与 `inf` / `nan`。

| 入口 | 原实现 | 3.5 |
| --- | --- | --- |
| `--peak-limit-dbtp`、`--loudness-target`、`--listener-yaw/pitch/roll`（binary32） | CLI11 `strtold` → `float` | `parse_decimal<float>`，范围检查在解析结果上进行 |
| `--final-gain-db`、`--start`、`--end`、`--room-metres-per-unit`（binary64） | CLI11 `strtold` → `double` | `parse_decimal<double>` |
| HOA `parse_speaker_label` 的方位 | `std::stof` | `parse_decimal<float>`，尾随字符仍被拒绝 |
| 一致性工具 `mr_adm_phase2_render` 的同名选项 | `std::stof` / `std::stod` | 同一解析器 |

UID、扬声器标签键、布局名、渲染器名与文件扩展名的大小写规范化原先用 `std::toupper` / `std::tolower`，标签键还用 `std::isalnum`
筛选字符。这些函数跟随 C locale：宿主程序（例如 GUI）设置单字节 locale 后，UTF-8 序列中的字节可能被当作字母改写或保留。3.5 改为只看
ASCII 的分类与大小写转换。

JSON（语义策略、DirectSpeakers 矩阵、覆盖等）中的数字仍由 nlohmann 经 `strtod` 解析。三个平台的 C 运行时都正确舍入 `strtod`，
随后转 binary32 的步骤与平台无关；一期 1.6 已登记这一依赖，3.5 不改变。

## 增益入口（N03 剩余）

核对后，语义策略（对象、HOA 包、DirectSpeakers）、实时覆盖（EAR、VBAP、HOA、Triple Balance、双耳文件流式、Monitor）与离线后处理的
dB 换算全部经过 Rust 的 `libm::powf` / `libm::pow`，C++ 中已没有 `std::pow` 调用。N03 不再有需要迁移的入口。

## 同起点块的顺序（S08）

EAR、VBAP、HOA、双耳 spreader、双耳 OLA 与 Apple 后端在每个声道内按 `start_sample` 排序块，原先使用 `std::ranges::sort`。这个排序
不稳定：起点相同的块的相对顺序由标准库的实现决定。各标准库对少量元素使用插入排序（稳定），元素超过阈值（16—32 个）后改用
introsort，等价元素的顺序就会随实现改变。

3.5 先在 Linux x64（libstdc++）上复现：一个声道 40 个同起点块时，旧实现生效的是列表中间的一块，而不是文档中的最后一块，四个后端的
输出能量分布都明显不同。规则定义为：**起点相同的块保持场景中的顺序（ADM 文档顺序，导入不重排），最后一块生效**；各处排序改为
`std::ranges::stable_sort`。Rust 侧按 C++ 给出的顺序使用块表，不再排序。

双耳 OLA 与 spreader 的取块器从头查找未结束的块，单独稳定排序仍会选中同起点的第一块。因此双耳在展开 Objects 的 point、cloud、
diffuse、divergence 与 spreader 分量之前，按起点稳定排序并只保留场景顺序中的最后一块；DirectSpeakers 同样在建立独立源之前处理。
被覆盖块的分量不会残留，也不会在获胜块较早结束后重新生效。文件流式渲染的拓扑重建复用这一处理。

`binaural_render_fixture_test` 使用两组乱序、每组 40 个同起点块，与仅含各组最后一块的场景逐位比较 PCM。覆盖 point、cloud / diffuse / divergence、
全 diffuse 与 DirectSpeakers（含被覆盖的 LFE），两种平滑长度，以及离线渲染、文件流式渲染和离线 SAF spreader；获胜块较短，另检查被覆盖块不会恢复。

其他后端也有被覆盖块残留的情况：
- **EAR、VBAP 与 HOA：** Rust `pcm_mix` 与 `hoa` 的取块器会推进到同起点的最后一块，所以稳定排序后生效的是正确的块。但获胜块的插值前驱仍是同起点被覆盖的那一块：不是 jump 的获胜块从被覆盖块的增益渐变，而不是从上一个起点的生效块渐变。
- **Apple：** Apple 后端把 DirectSpeakers 的 LFE 块与非 LFE 块分到不同的总线，被覆盖的 LFE 块会在自己的总线上继续生效。
- **22.2 LFE 路由与 VBAP 高度判断：** 两者都统计被覆盖块。例如被覆盖的 LFE1 加上生效的 LFE2，会让 split-power 判定场景同时含两个语义 LFE 而拒绝渲染。

因此双耳的处理移入 `render_common::last_blocks_at_each_start`。EAR、VBAP、HOA、Apple、22.2 LFE 路由与 VBAP 高度判断都在建立增益、
总线或路由之前调用它，只保留每个起点的最后一块。

`same_start_blocks_test` 对其余后端做同样的比较：
- 覆盖 EAR 与 VBAP（4+5+0 Objects，以及 9+10+3 split-power 的 DirectSpeakers）和 HOA3（Objects 与 DirectSpeakers）。
- 每例用两组乱序、每组 40 个同起点块，与只含各组获胜块的场景比较，覆盖两种平滑长度。离线渲染与碎片分块的文件流式渲染都要求逐位相同。
- 修改前，EAR、VBAP 与 HOA 的 Objects 用例全部不同，EAR 与 VBAP 的 split-power 用例准备失败。
- Apple 后端只在 macOS 上编译，没有加 PCM 回归。

## 新增用例与内核门禁

一致性夹具新增 `params`：声道 0 是 Objects，40 个块都从 0 开始、方向各不相同，增益为 −6.25 dB；声道 1 是没有标签的 DirectSpeakers，
Cartesian 位置 (0.5, 0.75, 0.25)，增益为 −3.5 dB。

| 用例 | 渲染器 / 输出 |
| --- | --- |
| `params-ear` | EAR 7.1.4 |
| `params-saf` | VBAP 5.1 |
| `params-hoa` | HOA3 |
| `params-binaural` | 双耳 |
| `params-binaural-rotated` | 双耳，`--listener-yaw 37.000001 --listener-pitch 1.25e1 --listener-roll -9.9999999`（经命令行解析） |

5 个用例标注 `slice: 3.5`，记在二期冻结参数集之外，PCM 门禁由 228 个增至 233 个。

| 内核文件 | 内容 |
| --- | --- |
| `adm-import.10-gain.f32` | −120 至 +24 dB 每 0.05 dB 及若干端点的导入增益 |
| `adm-import.20-cartesian.f32` | 17 × 17 × 17 的 Cartesian 网格投影为方位、仰角与距离 |
| `decimal.10-parse.f64` | 25 段十进制文字（舍入边界、符号、指数、分隔符、空白、次正规数）的 binary64 与 binary32 解析结果 |

3 个文件全部设为门禁（模式 `adm-import.*`、`decimal.*`）。内核文件由 228 个增至 231 个，门禁由 223 个增至 226 个。

## 验证

**本机（Linux x64，config A）。** 与 3.4 的采集相比，原有 228 个 PCM id 与 228 个内核文件全部不变（Linux 上 `libm` 与 glibc 对
上述导入输入逐位相同，原有用例没有同起点块），5 个 `params-*` 用例与 3 个内核文件为新增。用迁移前的二进制渲染同一夹具作对照，
EAR、VBAP、HOA 与双耳的输出都因生效块不同而明显改变（S08）。比较器自检 233 个 PCM id、253 项门禁无失败。ctest 79 项全部通过；
Rust fmt / clippy、`decimal` 单元测试（含一次舍入与二次舍入结果不同的例子）、源码扫描测试、FFI 头校验（271 个函数）、许可证校验、
clang-format 与 clang-tidy 无告警；命令行越界与非数字的取值仍被拒绝。

**三平台验收。** `75e38ae` 的 Consistency 38879296864、CI 38879300366 与 Quality 38879303103 通过：

- A/B 两种配置下 233 个 PCM id 三平台相同，包括 5 个 `params-*` 用例（含经命令行解析听者朝向的 `params-binaural-rotated`）；253 项门禁无失败；
- 231 个内核文件全部三平台相同，包括 `adm-import.10-gain.f32`、`adm-import.20-cartesian.f32` 与 `decimal.10-parse.f64`；
- 诊断检查点 78,459 个（每对平台 25,952 个用例检查点与 201 个内核检查点）全部三平台相同。

**同起点块修正后（本机，Linux x64，config A）。** 合并后双耳与其余后端改为只保留每个起点的最后一块。与 3.5 的采集相比，
原有 228 个 PCM id 与 231 个内核文件全部不变，只有 5 个 `params-*` 用例改变：
- EAR、VBAP 与 HOA 只有开头 5 ms 默认插值段改变（分别 960、480 与 3,600 个样本，最大差 0.21），获胜块不再从被覆盖块渐变；
- 两个双耳用例的全部样本改变，生效块由同起点的第一块改为最后一块。

比较器自检 233 个 PCM id、253 项门禁无失败。ctest 80 项全部通过（新增 `mr_adm_same_start_blocks_tests`）。
`lfe_routing_test` 原先把 LFE1 与 LFE2 块放在同一轨道的同一起点上；按本规则 LFE2 会覆盖 LFE1。因此“两个语义 LFE”的用例改为把
LFE2 放在另一条轨道上，重复的 LFE1 块改用不同起点。clang-format 与 clang-tidy 无告警。上面的三平台结果对应修正前的 `75e38ae`。

**同起点块修正后的三平台验收。** `c89e81a` 的 Consistency 38907550214 与 CI 38907555184 通过：
- A/B 两种配置下 233 个 PCM id 三平台相同。5 个 `params-*` 用例的新结果与本机采集逐位相同；253 项门禁无失败。
- 231 个内核文件全部三平台相同。
- 诊断检查点 78,459 个全部三平台相同。

同一提交的 Quality 38907559281 因 cppcheck（`uninitMemberVarNoCtor`）报告新测试的后端表成员没有默认初始化而失败；`5686243` 补上初始化，
只改动这一个测试文件。`5686243` 的 Quality 38911391642 与 CI 38911395592 通过。
