# Rust WAVE 容器元数据迁移

> 基线：`fae5606`（另含 `cf3208c` 的无关清理）。前序批次：[Rust BW64 迁移](RUST_BW64_MIGRATION.md)、
> [Rust 浮点 WAVE 迁移](RUST_DR_WAV_MIGRATION.md)。

前两轮把 WAVE 样本读写迁入 `mradm-wav` 后，C++ 仍保留五处手写的 RIFF chunk 扫描与改写。本轮把它们的字节布局与
chunk 表处理全部移入 `mradm-wav`，C++ 不再自行解析或拼装 RIFF/RF64/BW64 头：

| 原 C++ 位置 | 职责 | Rust 入口（`mradm-wav`） |
|---|---|---|
| `wav_io.cpp` `write_wav_metadata` | 追加 BWF v2 `bext`，HOA3 输出写 AmbiX `ambi`，更新 RIFF 大小或 `ds64.bw64Size` | `bext_payload` + `append_bext` |
| `wav_io.cpp` `finalize_wav_layout` | 按声道置换、扬声器掩码、ADM `chna`/`axml`、`fact` 与容器选择重写渲染输出 | `LayoutRewriter`（`new` / `step` / `finish`） |
| `scene_importer.cpp` `read_wave_adm_metadata` | 读取 `axml` 与 `chna` | `Reader::read_chunk` + `Chna::decode_import` |
| `scene_importer.cpp` `rewrite_bwf_replacing_axml` | export：只替换 `axml` payload，其余 chunk 逐字节复制 | `replace_chunk` |
| `channel_bed_importer.cpp` `scan_axml` | 导入路由：是否存在 `axml` | `has_chunk` |

C++ 继续持有：

- 文件编排：临时路径分配、`TempPathGuard`、备份后改名替换、export 的临时文件与回退复制；
- 进度与取消：`finalize_wav_layout` 每 2048 帧调用一次 `step`，在两次调用之间检查取消并上报进度；
- 语义：`bext` 描述文本（`renderer=… layout=…`）的组成、何时写 `ambi`（`output_layout == "hoa3"`）、
  各布局的掩码/置换/ADM 元数据（`wav_output.cpp`），以及 CHNA → 声道映射的导入规则（UID 规范化、拒绝 trackIndex 0）。

## 边界

- 公开 C++ API 只新增 `include/adm/audio_io.h` 中的 `read_wav_adm_chunks`、`replace_wav_axml` 与 `wav_has_chunk`；
  `write_file_metadata`、`finalize_wav_layout`、`import_scene`、`get_axml`、`write_scene` 的签名不变。C ABI 不变。
- 私有 FFI（`src/adm_audio/wav_ffi.h`）新增 9 个函数和 3 个 `#[repr(C)]` 结构体（`MradmWavBext`、
  `MradmWavChnaTrack`、`MradmWavLayoutOptions`）。字节串一律以指针 + 长度借用一次调用；布局重写用不透明句柄
  `MradmWavLayout`，`begin` 以 `create_new` 排他创建目标文件，删除与安装仍由 C++ 负责。
- 读取 chunk 与 CHNA 采用两次调用：容量为 0 时只返回是否存在与大小/条目数，第二次按精确容量复制。多个可写
  输出缓冲区必须互不重叠，FFI 在写入前检查。
- 所有路径沿用 `wav_path_utf8`：Windows 上按当前代码页把原生窄路径转为 UTF-8，与其他 WAVE I/O 一致。

## 字节兼容契约

新实现对同一输入产生与旧 C++ 逐字节相同的输出，包括：

- `bext`：602 字节固定 payload；文本字段最多复制宽度减 1 个字节并以 NUL 填充；`OriginationDate` 取 ISO 文本前
  10 字节，`OriginationTime` 由 `Thh:mm:ss` 改写为 `hh-mm-ss`；Version 2；`LoudnessValue` 与 `MaxTruePeakLevel`
  为百分之一单位、远离零取整（与 `lround` 相同），超出 int16 时按模回绕（与 C++20 整数转换相同），未提供时为
  `0x7FFF`。`ambi` 已存在时原位覆盖，否则追加在 `bext` 之后。
- 布局重写：容器选择规则不变——带 ADM 元数据的整数 PCM 写 BW64；源为 RF64/BW64 且未要求 RIFF 时保留 64 位
  容器；总大小超过 4GB 时升级 RF64。chunk 顺序仍为 `ds64`、`fmt `、`chna`、`fact`、`data`、`axml`；`fmt ` 的
  extensible 有效位深写容器位深；`fact` 帧数截断到 uint32；CHNA 标识用空格补齐，记录末尾字节为空格。
- export：保留源容器标签，重算 RIFF 大小或 `ds64.bw64Size`；不超过 4GB 的 `data` 写实际大小，超过时写
  `0xFFFFFFFF` 哨兵；奇数长度 chunk 以 0 填充；`ds64` 大小表非空时拒绝。
- 路由探测：容错语义不变——读到截断的 chunk 头或越界即结束扫描，只有无法打开或不是 RIFF/RF64/BW64 WAVE 时失败，
  因此 8-bit 等不支持的样本格式仍交给 channel-bed 路径报出它自己的错误。

错误码保持一致（参数错误、不支持、I/O 错误分别对应 `invalid_argument`、`unsupported`、`io_error`）；
Rust 返回的错误消息为中文，措辞与旧英文消息不同。

## 行为差异

以下差异都发生在旧实现会产生损坏文件或依赖平台未定义结果的输入上：

- **`bext` 追加前完成全部检查。** 已有 `ambi` 长度不是 16 字节时，旧实现先追加了 `bext` 再报错，留下大小未更新
  的文件；现在不写任何字节。
- **容器外的尾随字节不再被吸收。** 文件物理长度超过声明的容器长度时，旧实现在物理末尾追加并把尾随字节并入容器；
  现在报 I/O 错误且不修改文件。本项目写出的文件不会出现这种情况。
- **非有限的响度或真峰值写为未指示（`0x7FFF`）。** 旧实现对 NaN/Inf 调用 `lround`，结果依平台而定。
- **ADM 读取与 export 使用严格的 chunk 表解析。** 旧扫描器会容忍截断或越界的 chunk 表；导入路径在此之前本来就要用
  Rust reader 打开文件，因此导入行为不变。`get_axml`（`inspect`、`render_service`）与 `write_scene` 现在同样要求
  文件能被 Rust reader 接受。同名 chunk 重复时取第一个（旧实现读取 `axml` 时取最后一个，CHNA 合并全部）。
- **export 在写出前完成全部检查。** 非 `data` chunk 超过 4GB 现在与 `ds64` 大小表非空一样在写出任何字节前报错；旧实现写到一半才发现，随后删除临时文件。
- **布局重写的临时文件以排他方式创建。** 路径仍由 C++ 预先挑选不存在的名字，竞争时不再覆盖他人文件。

## 测试

- `mr_adm_wav_container_tests`（默认 ctest）：把旧实现冻结为 `tests/reference/wav_container/legacy.cpp`，
  对运行时生成的 fixture 逐例比较新旧输出的字节或错误码。fixture 覆盖 RIFF/RF64/BW64、PCM16/24/32 与 float32、
  普通与 extensible 头、空 data、奇数长度 data/LIST/AXML、JUNK、带保留记录的 CHNA、已有 `ambi`、非空 `ds64` 表。
  - 布局：恒等、`prefer_riff`、强制 extensible + `fact`、立体声掩码 + 置换、ADM（BW64 与奇数 AXML）、16 声道反序，
    以及掩码数、置换、AXML/CHNA 成对、CHNA 范围/重复/宽度八类错误；
  - 元数据：完整字段、空字段、只有日期、负的 0.5 舍入、超长字段、HOA3，各连续写两次；
  - 读取：AXML、CHNA 映射与 `get_axml`；
  - export：0、1、2、4097 字节的 AXML；
  - 路由：每个 fixture 及其截断 1/5/9 字节、尾随垃圾、8-bit + AXML、非 WAVE、过短与缺失文件。

  另有只针对新实现的检查：无效 `ambi` 与尾随字节在写入前拒绝且文件不变，CHNA 的保留记录与 trackIndex 0 的上报，
  布局重写失败或取消时源文件不变且不残留临时文件。本地共 256 例一致；故意把 `bext` 版本改为 1、CHNA 填充改为
  NUL、取消 `ds64` 表检查，测试都能报出差异。
- Rust 独立回归（`mradm-wav/tests/edit.rs`，不使用旧实现或生产 writer 组装 fixture）：`bext` 字段偏移、取整、
  回绕与非有限值；RIFF/RF64 大小更新与 `ambi` 原位覆盖；布局置换后的逐字节位置、容器选择、chunk 顺序与早结束；
  export 的其余 chunk 保真；探测的容错；CHNA 保留记录。
- FFI 测试：空选项、重复 finish、排他创建、两阶段读取的容量检查、输出缓冲区重叠、空指针 id 与字段。

## 验收记录

2026-10-06，Linux x86_64（GCC 13）：

- 默认 Debug CTest 68/68 通过（新增 `mr_adm_wav_container_tests`）。
- `mr_adm_rust_quality`（rustfmt + clippy `-D warnings`）通过；`mr_adm_ffi_header_check` 通过，共 207 个函数、
  65 个结构体。
- Release 端到端比较：分别用 `cf3208c`（C++ 容器代码）与本改动构建 `mradm`，对同一批输入运行，共 210 组全部一致。
  - 输入 12 个：`mr_adm_make_fixture` 生成的 4 种 24-bit ADM（objects-point、objects-extent-multi、directspeakers、hoa）
    及其 float32 RF64 版本、一份 float32 BW64 ADM，以及 5.1 float32、5.1 int16、7.1.4 float32 三个 channel-bed。
  - render 到 WAV 14 种：7.1.4（f32/i24，含置换与掩码）、5.1（f32/i16）、0+2+0、双耳（f32/i24，ADM Binaural）、
    9.1.6（f32/i24，ADM DirectSpeakers）、22.2 i24、HOA3（f32/i24，ADM + `ambi`）、5.1 i24 响度归一化、5.1 trim。
    channel-bed 跳过 HOA3，共 162 组：153 组逐字节相同，其余 9 组只有 `bext` 的 OriginationDate/Time（渲染时刻）不同，
    置零这 18 字节后逐字节相同。
  - `inspect` 摘要与 `inspect --xml` 共 24 组输出相同（channel-bed 的 `--xml` 两边同样报缺少 AXML）。
  - `export`（无策略与中性策略模板）共 24 组：ADM 输入 18 组逐字节相同，channel-bed 的 6 组两边同样报错。

## 参考实现退出

单元 `wav_container`（frozen，`tests/reference/wav_container/`）在默认测试中运行，不需要 CMake 开关。按
[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)的通用条件退役；登记与哈希见
[`tests/reference/retention.json`](../../tests/reference/retention.json)。冻结内容逐字复制自基线的 `wav_io.cpp`（`TempPathGuard`、进度辅助、chunk 扫描与写出辅助、`finalize_wav_layout`、`write_wav_metadata`）、`scene_importer.cpp`（`normalize_uid`、`read_wave_adm_metadata`、`rewrite_bwf_replacing_axml`）和 `channel_bed_importer.cpp`（`scan_axml` 及其读取辅助）；只加了按源文件划分的命名空间、`using` 声明和文件末尾供测试调用的导出包装，`finalize_wav_layout` 包装不传进度回调。
