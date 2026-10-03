# Dolby Atmos Renderer 房间布局研究

`make_dac_probe.py OUTPUT_DIRECTORY` 根据本机 Renderer 安装目录中的 XSD 生成普通 7.1.4 与三层 22.2
物理房间 XML，并调用 `xmllint` 验证。不会修改或调用 Renderer，也不会覆盖已有输出。

这些文件目前仅通过 schema 检查，**尚未被 Renderer 成功导入**，不能视为可用房间配置。
试验记录和限制见 `docs/architecture/DAR_222_FEASIBILITY.md`。

应用导出的 `.atmoscfg` 是 SQLite 文件，不是这些 XML 的另一种扩展名；改扩展名不能当作格式转换。

## 同源 ADM 的无窗口 re-render

后续重复实验使用一条命令：

```sh
python3 scripts/research/dar_layouts/run_headless_rerender.py \
  --adm /absolute/path/to/master.wav \
  --output-dir local/dar-rerender-new
```

默认顺序会先导出当前配置已有的布局，再导出另一种；`--layouts 7.1.4` 或
`--layouts 9.1.6` 可只选一种。脚本要求 Renderer 5.5 正在运行、输出目录不存在或为空，
当前仅有一条未映射的 7.1.4／9.1.6 re-render。它会自动编译临时桥、读取 ADM、
导出多声道 WAV、用 FFprobe/FFmpeg 验证、写入 `run.json`，并恢复原母版、re-render 布局、
导出设置及无窗口网关实例。开始前会备份应用设置数据库；结束时对 key 24（re-render）
和 key 251（房间）逐字节哈希核对。需要 Xcode clang、Homebrew Qt 头文件和
FFmpeg/FFprobe；批量脚本自身不依赖 NumPy。

`dar_batch_bridge.cpp` / `dar_batch_lldb.py` 是脚本使用的单动作事件循环桥。
它调用的是本机 Renderer 的私有 Qt 网关，不是 Dolby 承诺稳定的公开 API；
应用升级后脚本会拒绝非 5.5 版本，仍需重新验证。

早期的一次性研究工具如下，保留用于追溯入口与失败原因：

`gateway_shim.cpp` 和 `export_gateway_inventory.cpp` 只读检查 Renderer 的 Qt 网关与内嵌 QML。
`headless_export_gateway.cpp`、`headless_rerender_run.cpp`、`headless_layout_switch.cpp`
通过 LLDB 加载到**正在运行**的 Renderer，随后把指定 ADM 探针的网关调用排到应用事件循环；
不创建导出窗口，也不使用 Atmos 压缩码流。`debug_gateway.py` 的相关命令带有固定文件和布局假设，
不能盲目对其他 Renderer 状态运行；后续请使用上面的批量脚本。

本次实际获得 7.1.4 和 9.1.6 离线 WAV，并把 7.1.4 临时布局恢复至原 9.1.6。
`compare_adm_probe.py` 用 FFmpeg 和 NumPy 对同一 ADM 的 Dolby／Release SAF 输出逐段测量声道 RMS。
证据和当前结论见 `docs/architecture/ATMOS_NATIVE_222_FEASIBILITY.md`；
原始记录位于 `local/dar-222-20260925/` 与 `local/dar-calibration-20260925/`。

## 7.1.4／9.1.6 兼容模式数值实验

`mradm render --renderer triple-balance` 选用独立的 Cartesian 房间点源内核；
不指定时仍用原 SAF VBAP。当前只支持直接 ADM 输入的 7.1.4／9.1.6、固定 standard geometry、
Cartesian Objects 和无声的普通 bed。48 kHz 等尺寸对象已接入独立有状态内核，覆盖静态尺寸和带尺寸运动；
其他采样率的非零尺寸、独立三轴尺寸和未验证标志返回 unsupported。
`--speaker-spread-mode none` 可有意忽略尺寸，得到点源输出。`mdap` 与兼容模式互斥。
此模式尚未接 GUI、实时接口或 C ABI；22.2 不在本次验收范围。
当前尺寸规则、门槛和结果见 `docs/architecture/DAR_NATIVE_SIZE_ALIGNMENT.md`。

从已生成的 suite 复测（需 Renderer 5.5 运行；若 `dar/run.json` 已存在，会先验证 ADM、
导出布局及每个参考 WAV 的哈希，再复用）：

```sh
python3 scripts/research/dar_layouts/run_compat_suite.py \
  --suite-root local/dar-compat-bank-20260925 \
  --channel-map local/dar-compat-suite-20260925/channel-map.json \
  --run-name my-run
```

对不存在的 `--suite-root` 加 `--profile point-bank`、`motion`、`motion-holdout`、
`gain-control` 或 `size-sequence`
可从 AC-4 项目的 DAMF writer 构造新 ADM，并自动完成 Dolby 导出、Release 自有渲染、
同源核对和数值报告。候选输出和 JSON 报告写入每个案例的 `--run-name` 子目录，
总结写入 suite 根目录的 `RUN_NAME-summary.json`。point-bank 用独立点位验收带符号增益向量，
motion 测包络和事件时刻，size-sequence 用显式关闭尺寸的点源输出保留诊断基线；
尺寸完整评分使用下面的长 PRBS 与尺寸运动套件。
声道映射必须先用 `freeze_channel_map.py` 对同一 ADM 的带名称 multi-mono 与 interleaved
导出逐样本冻结；现有冻结映射见 `local/dar-compat-suite-20260925/channel-map.json`。

Renderer 5.5 对本批直接 ADM 探针中的 `audioBlockFormat/gain` 表现为忽略：静态 0／−6／−12 dB
对象的导出增益均为 1，变增益运动探针也是如此。兼容模式按该可重复的参考行为处理；
这并不代表 ADM 规范要求忽略 gain，也不应用于其他 renderer。后续对象语义研究已验证
`audioObject/gain`、mute 与起止字段的直接 ADM 行为，见下方对象语义套件。
尺寸大于零时参考输出还含显著非相干分量，
不能只用静态点源增益解释。后续扫描和调用链记录已修正早期声道集合的判断：尺寸分支使用
11 个固定非 LFE 位置，同时仍有尺寸相关的原对象分支；9.1.6 的小尺寸仍可包含宽声道和顶中声道。
转换工具还给非零 size 附加 `diffuse=1`；用 `isolate_diffuse_adm.py` 构造的
`diffuse=0` 同源 ADM 导出了逐字节相同的 WAV，说明本批残差是 size 路径的行为。
完整结果和失败边界见 `docs/architecture/DAR_ROOM_COMPAT_ALIGNMENT.md`。

## 等尺寸对象的后续系统识别

`make_calibration_suite.py` 新增 `size-impulse`、`size-prbs-long`、`size-transfer`、
`size-superposition`、`size-warm-impulse`、`size-warm-bank`、`size-warm-bank-geometry`、
`size-motion`、`size-motion-boundary`、`size-route-scan`、`size-spatial-train`、
`size-spatial-validation` 和 `size-two-object`。它们仍由案例描述生成 DAMF，再核对最终 ADM BWF 的
对象绑定、坐标、三轴尺寸、时间和实际 PCM。`run_compat_suite.py` 可导出并测量；
仅需单布局的研究案例可用 `--reference-run` 指向已完成且恢复设置的导出报告。

例如复测已有的长 PRBS 参考：

```sh
python3 scripts/research/dar_layouts/run_compat_suite.py \
  --suite-root local/dar-size-prbs-long-20260925 \
  --channel-map local/dar-compat-suite-20260925/channel-map.json \
  --run-name my-size-measurement
```

`measure_size_field.py` 把纯增益、RMS 幅度、声道功率、能量占比、
三分之一倍频程互谱矩阵、相干性和静音尾部分开报告。同一命令加 `--candidate-size` 会构建 Release、
渲染自有尺寸输出，并对长 PRBS 调用完整评分；该参数也支持 size-motion、size-motion-boundary 和 size-route-scan。
早期拟合失败记录见 `docs/architecture/DAR_SIZE_IDENTIFICATION.md`；新独立内核与最终验收见
`docs/architecture/DAR_NATIVE_SIZE_ALIGNMENT.md`。研究素材在 `local/` 分批保存。
`prune_generated_pcm.py` 可在核对哈希后移除指定的可重建 DAMF 源 PCM 与研究候选 WAV，
同时写出保留路径、哈希和大小的清理记录；本轮记录在 `local/dar-size-prune-20260925.json`。

## Renderer 5.5 参数与增益调用链采集

一键生成前方／原点／内部点 × size=0、0.01、0.1、0.2、0.25、1 的 18 个案例，
完成两布局无追踪控制、真实回调采集、Release 点源回归与两次重新初始化复测：

```sh
python3 scripts/research/dar_layouts/run_gain_suite.py \
  --channel-map local/dar-compat-suite-20260925/channel-map.json \
  --output-dir local/my-gain-suite
```

可用 `--cases` 换成同格式案例列表，或 `--adm` 复用已生成的最终 BWF。批次仍沿正常 ADM 导出
路径推进，**不是独立直调的 panner**。不要求手动 UI 操作；前提是本机已授权的 Renderer 正在运行，
且符合上文的单条 re-render 配置。增加依赖为 NumPy；不需要 IDA 即可复测已锁定版本的入口。

单布局采集入口 `run_gain_probe.py` 支持 `trace --adm FILE` 或 `batch --cases CASES.json`，
都需要 `--layout`、`--profile scripts/research/dar_layouts/renderer55_gain_profile.json`、`--output-dir`。
`--without-breakpoints` 生成无追踪控制；随后用 `--baseline-run CONTROL/export/run.json` 强制
核对完整 PCM。`measure_gain_trace.py --trace-root DIR --channel-map FILE` 生成统一参数与验证报告。
`summary.json/success` 仅表示导出、捕获及恢复成功，最终证据状态看 `gain-validation.json` 的
`passes_capture_validation` 和 `suite-report.json` 的 `success`。

套件成功后默认把完整 trace 无损压成 `trace.json.gz`，并在再次核对 PCM 和文件哈希后移除与保留控制
完全相同的 traced WAV；用 `--keep-intermediates` 可保留原文件。分析器可直接重读压缩 trace 和原控制。
`storage-manifest.json` 保存清理前哈希、大小及保留路径。已有研究目录可显式调用
`compact_gain_artifacts.py --root local/研究目录` 做同样的受限整理。

静态分析使用 `ida_gain_locator.py`（本机 IDA 9.1 `idat -A -S...`，等待自动分析），
`locate_gain_runtime.py` 汇总运行时代码快照的字符串引用、RTTI、虚表和 compact unwind 边界。
同一个主要数据库供后续定点分析复用；`DAR_IDA_FUNCTIONS` 可选择地址，`DAR_IDA_OUTPUT` 指定局部证据目录。
已安装文件与 UUID 必须匹配；运行时按加载基址重定位。原持续 LLDB 方案的异常冲突证据仍保留，
实际可用模式是一次附加安装驻留桥后分离，沿 Qt 事件循环导出并在正常调用范围内临时采集。

字段、实际命中路径、调试器限制与原始失败记录见
`docs/architecture/DAR_GAIN_CALL_CHAIN.md`。采集入口与生产内核分离，生产渲染不依赖运行中的 Renderer。

## 对象增益与生命周期

`run_semantic_suite.py` 串联最终 ADM 生成、驻留 Qt 参考导出、Release CLI、捕获验证与数值评分。
增益／mute／对象时间是源字段；显式 semantic policy 的用户音量与静音单独生效。
规则和支持范围见 `docs/architecture/DAR_OBJECT_SEMANTICS.md`。

```sh
python3 scripts/research/dar_layouts/run_semantic_suite.py \
  --output-dir local/my-semantic-boundary \
  --channel-map local/dar-compat-suite-20260925/channel-map.json --candidate
python3 scripts/research/dar_layouts/run_semantic_suite.py \
  --phase final --freeze local/my-semantic-boundary/acceptance.json \
  --seed 0x26092653 --output-dir local/my-semantic-final \
  --channel-map local/dar-compat-suite-20260925/channel-map.json --candidate
```

可用 `--resume` 续跑；`--only CASE_IDS` 和 `--skip-trace` 仅用于诊断，不能生成完整冻结依据。
代码、评分器或候选二进制变化会重新比较；已清理 WAV 的参考从经哈希确认的无损缓存读取。
需要重新追踪而控制 WAV 已删除时，重新导出真实控制并核对重复性，不伪造控制导出记录。

生成器对双对象先构造完整的双对象 DAMF／ADM，从它提取配套 DBMD；缓存保存来源母版与 DBMD 哈希。
最终写出前核对 PCM 格式／字节数、CHNA 数目与唯一绑定，以及 DBMD 与对应声道拓扑母版的字节一致性；
写出后核对 ADM 对象绑定。DBMD 保持不透明，不通过猜测私有偏移来修改声道计数。
`test_semantics.py` 覆盖错误 DBMD 缓存、旧单对象 DBMD 混入、CHNA 重复绑定及评分边界。

参考桥在进程层面互斥，响应 JSON 原子发布；结束核对母版、布局、导出设置和持久化 key 24／251。
单例失败、状态恢复失败、追踪干扰和数值失败分别保留，不能用失败导出作为接受参考。

## diffuse／objectDivergence 追踪

`trace_spatial_semantics.py --output-dir local/my-spatial-semantics --resume` 生成严格字段隔离的配对 ADM，
比较 7.1.4／9.1.6 完整参考 PCM。位置、尺寸、时间、PCM、CHNA 和 DBMD 在配对之间保持一致。
`analyze_spatial_trace.py --trace-root DIR` 汇总扩展捕获中的 XML 标签分发、diffuse 布尔存储及 OMO 输入／输出标记。
已有混音验证器继续核对实际消费者；新增研究不会自动放开生产 CLI 的字段限制。
完整结论与地址证据见 `docs/architecture/DAR_DIFFUSE_DIVERGENCE.md`。

## 非静音 7.1.2 bed

```sh
python3 scripts/research/dar_layouts/trace_bed_semantics.py \
  --output-dir local/my-bed-semantics \
  --channel-map local/dar-object-semantics/final/channel-map.json \
  --capture --compare-saf --compact
```

生成逐路脉冲、独立宽带及字段隔离 ADM，导出两布局，沿正常 OMO／OAR 路径捕获并验证 PCM，随后使用
Release 对比现有 label 路由和显式矩阵。标签交换是保留的预期拒绝案例，不用失败输出代替参考。
复跑核对输入与缓存哈希、恢复状态；`--compact` 无损压缩中间 WAV／trace，并核对压缩前后哈希。
`--only impulses,prbs` 可缩小实验范围，`--generate-only` 只生成。

`analyze_bed_trace.py` 使用实际 PCM 指针关联 OMO 输出与 OAR 输入，覆盖 adapter 将 LFE 移到首位的情况。
标准矩阵只用于基础路由诊断。当前 CLI 已接入单个标准 7.1.2 bed，完整证据和边界见
`docs/architecture/DAR_BED_SEMANTICS.md`。验证实际 CLI 接入与混合对象：

```sh
cmake --build --preset release --target mradm
python3 scripts/research/dar_layouts/verify_bed_integration.py \
  --reference-root local/dar-bed-semantics \
  --channel-map local/dar-object-semantics/final/channel-map.json \
  --output-dir local/dar-bed-integration
```

该套件复用已验证 bed 参考，新增混合点源／等尺寸／运动 ADM，检查用户电平、静音、重复性、裁剪、叠加
与 LFE 来源，保存评分、语义报告和恢复记录，压缩可再生中间音频。比较音频只使用 Release。

真实音乐比较还要验证封装：`compare_real_916.py` 分别报告 PCM 指标和 WAVEFORMATEXTENSIBLE／mask／ADM
chunk 行为，不能只因 16 路 PCM 的顺序正确就宣称播放器布局一致。`triple-balance` 9.1.6 WAV 跟随参考的
mask=0、无 AXML／CHNA；需要 CoreAudio 自动识别 9.1.6 的 A/B 使用带 Atmos_9_1_6 标签的 CAF，并验证其
解码 PCM 哈希分别等于对应 WAV。

## 自有 22.2 扩展

`verify_room_222.py --template local/dar-bed-semantics/template.wav --output-dir local/room-222/cli`
逐路核对 22 个非 LFE 输出、标准 bed、双 LFE 策略、尺寸／运动、裁剪、重复渲染、尾部和 CAF CICP_13 标签。
配合 `mr_adm_room_222_tests` 检查解析尺寸积分、边界和任意分块的独立状态。所有 CLI 音频使用 Release。
本套件不启动 Dolby；22.2 无 Dolby 数值参考，结论限定于自有几何与状态规则，详见
`docs/architecture/ROOM_222_EXTENSION.md`。
