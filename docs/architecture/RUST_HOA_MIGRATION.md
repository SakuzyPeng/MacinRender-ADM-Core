# Rust HOA 编码、状态与计量前处理迁移

> 基线 `ba96bac`。三阶 ACN/SN3D 编码与计量前处理由 Rust 接管，输出仍为 16 声道 `hoa3`。

## 所有权与接入

`mradm-dsp::hoa` 持有球谐系数生成、extent 采样、时间线查询、插值、PCM 累加、
三组 diffuse 历史以及计量前的 LFE 分离/7.1.4 解码。算法库继续禁止 unsafe；
`mradm-ffi` 提供私有不透明句柄、显式长度、`uint64` 帧位置及现有错误码边界。
C++ 通过可移动 RAII 管理句柄，没有新增依赖、构建开关、GUI 参数或公开 ABI。

C++ 保留 ADM 语义、位置偏移、divergence 展开、标签解析及原有排序。准备时按原始
场景遍历顺序提交数值描述与时间块排列，Rust 先计算系数再应用排列。direct 与三组
16 系数 diffuse 表通过 `Arc` 共享；C++ 释放临时描述，只保留对象 ID、输入通道和格式摘要。
被时长裁剪为空的块保留系数，仍可作为下一块的插值前驱。

每个离线渲染或 stream 有独立 Encoder：游标、端点和 diffuse 状态互不共享。
具有 diffuse 的轨道预分配三组 `1024 × 16` 浮点历史；没有 diffuse 的轨道不分配这部分历史。
最大块保持 `max(1024, object_smoothing_frames)`。批量编码向调用方输出累加。

MeterPreprocessor 与 Encoder 共享只读表，独立持有查询游标及计量副本，
由原来的串行计量线程独占。它输出 12 声道解码 PCM 和单声道 LFE；C++ 继续调用已有
Rust 计量器并组装报告。双缓冲、future 回收与写文件顺序保持不变。
已有 HOA 输入解码仍属 EAR/libear，不在此迁移范围内。

## 数值与状态契约

- 保留三阶 SN3D 公式、ADM/HOA 坐标转换及重复归一化。长度固定为 double
  `(x² + y²) + z²` 后 sqrt 再转 float；不替换为三参数 hypot 或重排求和。
- 保留 17 点 extent 表、半径公式、零方向/极点兜底、diffuse 最小 width/height 和源分支顺序。
  与其他后端共享的 C++ 辅助函数保持原样，测试参考冻结自己的长度、半径及采样表。
- `upper_bound` 的重复起点选择、首块、间隙、jump、显式/默认插值及有效时长钳制保持原样。
  普通时间插值用 double 后转 float；对象平滑按原处理块首尾用 float 插值，不在块内事件处分块。
- diffuse 保留 32 个 tap、极性、`1/sqrt(32)` 和逐 tap 累加顺序，先读历史再写当前输入。
  三个分支独立推进；静音、零系数和块间间隙不隐式清历史。EOF 不追加尾音。
- live gain 继续在输入进入编码和 diffuse 历史之前应用。seek 从目标帧重新起块，
  清历史并将环形位置设为 `target % 1024`，保留 live gain 斜坡；暂停不增加 reset。
  离线窗口仍按原固定网格预热一个完整块，只写出和计量窗口内帧。
- 计量复制 encoded PCM 后逐轨减去 LFE，再按原 11×16 AllRAD/max-rE 矩阵解码，
  12 声道中的 LFE 槽固定为零。原输出 W 通道不变；LFE-only 查询不额外套对象平滑，
  独立 LFE TP 与空间 TP 按原规则合并。
- 零帧调用不推进游标或历史。无效尺寸、指针长度、索引、参数、系数或时间溢出在处理前拒绝，
  不修改该调用的输出或历史。音频非有限值继续传播，不做隐式清洗；reset 可清除被污染的历史。
- Rust 内核、FFI、reset 和参数拒绝从首次处理开始无分配、无锁、无 I/O。
  准备/销毁、C++ 读取、控制发布和线程调度不在此承诺中。指针存活、对齐及可变句柄独占
  仍由调用方保证；FFI 拒绝可写缓冲彼此及其与输入的别名，panic 不越过 C 边界。

原 `hoa.01-polar`、`hoa.02-direction`、`hoa.03-normalized`、`hoa.04-coefficients`
诊断检查点保留。Rust 返回原始遍历顺序中的首个阶段值，既有诊断开关下由 C++ 写文件；
诊断模式经过 Release 构建及实际输出验证，之后恢复普通 Release 配置。

## C++ 边界修复

`HoaStream::process` 在读取或消费 FIFO 前检查输出容量；prepared 与输入计划的声道/采样率
必须相符。原先未检查的输出不足请求现在返回 `invalid_argument`。

离线与流式读取少于计划帧数时返回 `io_error`，不把未填满的缓冲送入编码器。
正常末块仍按实际声明的剩余帧数读取，输出时长不变；错误不会补零或被当作成功 EOS。
窗口结束位置通过剩余帧数钳制后相加，避免超长 `frame_count` 回绕。
这些错误边界与数值兼容结果分别测试。

## 验证与复现

冻结参考位于 `tests/reference/hoa/legacy.h`，来源指纹见同目录 `provenance.json`。
参考只隔离命名空间、提取可独立调用的计量循环，并调整格式；真实语义与既有 fixture 继续回归。

一般有限系数/PCM 的 Release 同平台门限为 `2e-6 + 2e-6 * abs(reference)`。
指定计量 fixture 的误差门限为 `1e-4 LU/dB`，缺失/静音状态一致；精确路由、LFE W-only、
零槽及简单增益有独立逐位检查。原生探针对 Cartesian/极坐标、extent、divergence、重复起点、
空块、间隙、平滑、冷 seek 和计量预处理进行旧/新对照。

Rust 测试另覆盖独立延迟冲激、环形回绕、任意分块、zero/reset、非有限历史、错误原子性、
共享准备表与跨线程编码/计量隔离；内核与 FFI 各有首次调用分配探针。
原生 fixture 强化为 float32 离线/流式逐位相同，并覆盖正常短末块、异常短读、容量拒绝、
非对齐 seek、格式不匹配和超长窗口等于整段切片。

2026-10-05 原生验收：

| 项目 | macOS arm64 | Windows x64 |
|---|---:|---:|
| CTest | Debug 61/61；Release 定向 10/10 | canonical Release 60/60 |
| 系数最大绝对差 | `2.980232238769531e-7` | 0 |
| 编码 PCM 最大绝对差 | `7.450580596923828e-8` | 0 |
| 计量解码 PCM 最大绝对差 | `1.7881393432617188e-7` | 0 |
| LFE PCM 最大绝对差 | 0 | 0 |
| LUFS 差 | `1.1716336700828833e-7 LU` | 0 |
| TP 差 | 0 dB | 0 dB |

共对照 1,502,812 个数值，全部满足门限；Windows 同平台对照全部逐位相同。
21 组 macOS Release CLI 对照全部通过：13 组未修改后端控制用例逐位相同，8 组 HOA 用例
最大 PCM 绝对差为 `7.152557373046875e-7`。
Rust Release workspace 共 98 项测试通过，其中 HOA 6 项、分配探针 11 项、FFI 17 项。
Rust fmt/Clippy、改动 C++ 格式/clang-tidy/cppcheck、冻结参考及许可证/SBOM 检查通过。
两个原生平台均保留原有 139 个公开 `adm_*` 入口，没有私有 Rust 入口外泄。

[最终三平台 CI](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37295043944)
验证实现提交 `52bf106`：macOS Debug 61/61、Linux Debug 60/60、Windows Debug 60/60，
首轮全部通过。最终验收提交只更新文档及证据。

```sh
cmake --build --preset debug
ctest --preset debug --parallel "$(getconf _NPROCESSORS_ONLN)" --output-on-failure
cmake --build --preset release
build/release/mr_adm_hoa_rust_tests comparison.json
python3 scripts/consistency/compare-hoa.py \
  --reference /path/to/ba96bac-release/mradm --candidate build/release/mradm \
  --pcm-bits build/release/mr_adm_pcm_bits --fixtures /path/to/fixtures --output audio.json
cmake --build build/debug --target mr_adm_rust_quality
scripts/quality/check-changed.sh --base ba96bac --build-dir build/debug
scripts/quality/check-licenses.sh --build-dir build/debug
```

复用原工作区、Debug/Release 构建及共享 Cargo 缓存。Windows 使用既有 canonical Release，
同步前核验相关文件与基线/上次上传指纹并备份原始字节，保留其余修改；不把该原生验证描述为
干净 Git 检出。平台结果、源文件指纹及 CI 提交见[机器可读验收记录](evidence/rust-hoa/validation.json)。
数值明细：[macOS](evidence/rust-hoa/macos-comparison.json)、
[Windows](evidence/rust-hoa/windows-comparison.json)、
[Release CLI](evidence/rust-hoa/macos-audio.json)、
[诊断检查点](evidence/rust-hoa/diagnostics.json)。
跨平台逐位一致继续留到二期，性能改进不由本次迁移推断。
