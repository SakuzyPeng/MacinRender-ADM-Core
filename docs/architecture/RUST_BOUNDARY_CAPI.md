# C ABI 由 Rust 实现（六期 6.3）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)六期 6.3，家族清单与对照工具见 [6.1 记录](RUST_BOUNDARY_BASELINE.md)，
> 岛的窄 C 接口见 [6.2 记录](RUST_BOUNDARY_ISLANDS.md)。6.3 分 A（骨架）与 B–F（按家族迁移：版本、上下文与内存 7；
> render / inspect / room / 能力表 80；preview 4；Scene 38；Monitor / OSC 35）。

## 方向与用户决定

原计划由 rustc 产出 cdylib `mradm_capi`，A 批先把现有 C++ C ABI 整体链接进去。Linux 实测：cdylib 会导出依赖 crate 中所有
`#[no_mangle]` 函数，另加的 `--version-script` 也压不住。6.5 删除外壳之前，C++ 外壳与岛还要调用 `mradm-ffi` 的约 412 个 `mradm_*`
入口，所以这期间 cdylib 做不到只导出 164 个 `adm_*`。此外，要让 cdylib 导出仍由 C++ 实现的函数，还得写 164 个 Rust 转发桩并给 C++
定义改名。

用户决定（2026-11-13）：**6.3 不用 cdylib，6.6 再换。**
- 6.3 期间 `mradm_capi` 共享库照旧由 CMake 链接，沿用现有导出控制（ELF 版本脚本、Mach-O `-exported_symbols_list`、Windows `.def`），
  导出面、SONAME（`libmradm_capi.so.1`）、install_name、`.def` 与 GUI 打包都不变；
- Rust 实现的 `adm_*` 放在新 crate `mradm-capi`，它取代 `mradm-ffi` 成为 CMake 链接的唯一 Rust staticlib；
- 6.6 外壳已删、构建改以 Cargo 为主时，`mradm-capi` 改为 cdylib，那时核对 SONAME / install_name / `.def` 与 GUI 打包；
- A 批因此改为骨架，不迁移任何实现。

## A 批：骨架

- **crate。** `rust/crates/mradm-capi`（staticlib）依赖 `mradm-ffi`，并转发其 `sofa` / `diagnostics` / `room-cuda` 特性；`mradm-ffi` 的
  crate-type 改为 `rlib`。`use mradm_ffi as _;` 保证 staticlib 带上 `mradm-ffi` 的全部 `mradm_*` 符号（edition 2018 起未被引用的
  依赖不会链接）。`build.rs` 与 `mradm-ffi` 相同，只把 CMake 选定的 libFLAC 交给 Cargo 自己链接的测试。
- **CMake。** `MRRust.cmake` 经 Corrosion 导入 `mradm-capi`，目标随之改名为 `mradm_capi` / `mradm_capi-static` /
  `cargo-build_mradm_capi`，其余规则（PIC、FLAC 链接顺序、macOS SDK 路径、环境变量）照搬。Windows 的 Rust 归档放在配置构建目录的
  `rust-static/`，与 bundle 的 `mradm_capi.lib` 导入库分开。安装的 Rust 归档仍改名为
  `libmradm_ffi.a` / `mradm_ffi.lib`，`MacinRenderADMCoreConfig` 不变，6.5 随 SDK 删除。
- **入口表。** 共享库内没有代码调用 Rust 实现的导出，链接器不会从 staticlib 中取出它们。`check-capi-exports.py --write-references`
  从 `c_api.h` 生成 `generated/mradm_capi_entries.c`：一张引用全部 164 个声明函数的表，编进 `mradm_capi_bundle`。表名
  `mradm_capi_entries` 不在导出面内；它包含 `c_api.h`，所以 Rust 实现缺失时链接失败。未使用的 `bundle_stub.cpp` 删除。
- **panic 兜底。** `mradm-capi` `guard`：每个导出体经 `catch_unwind` 执行，panic 时返回与 C++ `catch (...)` 相同的回退值
  （`code` 为 `ADM_ERROR_INTERNAL`，`pointer` 为 NULL，`unit` 无返回值）。兜底不持有状态，回调线程与重入语义由各导出自己保持。
- **签名核对。** `check-ffi-headers.py` 另用 cbindgen 渲染 `mradm-capi`，与 `c_api.h` 单向核对：Rust 的每个 `adm_*` 都须在 `c_api.h`
  中以相同签名声明，结构体按字段比较；枚举 typedef 按 `int`（`c_api.h` 以 `static_assert` 保证）、回调 typedef 与内联函数指针按签名、标量 typedef
  按所指类型比较；`ADM_API_NOEXCEPT` / `ADM_API_DEPRECATED` 先去掉。无法解析的导出声明报错。仍在 C++ 的函数不报错，由 `check-capi-exports.py` 在共享库上
  校验导出。`quality_scripts_test.py` 新增对应测试，并要求解析出的函数恰好是 `c_api.h` 声明的 164 个。

## 规模

| 区域 | 6.2 | 6.3 A |
| --- | ---: | ---: |
| production（C/C++） | 21,124 | 21,121 |
| `island-interfaces` | 988 | 988 |
| `rust/crates/mradm-capi`（Rust，不设棘轮） | — | 103 |

`src/adm_c_api` 3,545 → 3,542 行（`bundle_stub.cpp`）。

## 验证（本机 Linux x64，Release，`MR_ADM_BUILD_CAPI_BUNDLE=ON`）

参照为 main `66bfa9d` 的同配置构建（二进制另存），候选为本批源码：
- **CLI 记录**（`cli-transcript.py`，`--tool mradm=参照`）：102 条命令 0 差异，另有 3 条 help 文本只差可执行文件路径；
- **C ABI 采集**（`capi-capture.py`，`--library 参照`）：3,283 行、36 个文件 0 差异；consumer 测试换库后在两个库上都通过；
- **完整一致性采集**（`collect.py --tool mradm=参照`，`compare-collections.py`）：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- **入口表**：把 `adm_api_version_major` 临时移到 `mradm-capi`（C++ 定义删除）后，共享库恰好导出 164 个 `adm_*`，consumer 测试通过；
  去掉入口表后 `check-capi-exports.py` 报缺 1 个。实验未提交；
- ctest 85/85，`mr_adm_rust_quality`（fmt + clippy），FFI 头校验（412 个私有函数、171 个结构体；`c_api.h` 164 个函数中 0 个由 Rust
  实现），`check-capi-exports.py`（164 个），许可证校验（Cargo 清单登记 `mradm-capi`），`language-surface.py --check`。

## 维护者审查修正与验证（2026-11-12）

修复提交 `d4cd4df2` 在 A 批上修正两项问题。

- Windows canonical 配置的 Rust 静态库与 bundle 导入库原本都写入 `mradm_capi.lib`，Ninja 报重复生成规则。
  Rust 归档改放 `rust-static/` 后，MSVC/Ninja 完整构建与导出检查通过，安装规则仍将该归档安装为 `mradm_ffi.lib`。
- 原签名解析跳过带内联函数指针参数的整个声明，公开头的单向比较因而漏检。
  现在按括号层级拆分参数，复用回调签名比较，并检查所有导出声明均被解析。
  38 项脚本测试通过，覆盖内联回调的类型、const、参数个数、返回类型和未知导出，以及无法解析的声明。
  真实 cbindgen 生成的 `adm_render_file` 回调首参 `float` 反例校验失败，改回 `double` 通过。

参照为 main `66bfa9d2` 的同配置 Release 构建。macOS 参照冻结其共享库依赖，Windows 复用 canonical 构建缓存。
所有 CLI 与 C ABI 采集均使用 Release。

| 平台 | 完整 CTest | CLI 命令 | C ABI 采集 | 新旧库 consumer |
| --- | ---: | ---: | ---: | ---: |
| macOS arm64 Debug | 90/90 | 102（Release） | 3,607 行、39 个文件（Release） | 各 2/2（Release） |
| Windows x64 Release | 88/88 | 102 | 3,179 行、35 个文件 | 各 2/2 |

两平台 C ABI 采集均为零差异，共享库均恰好导出 164 个 `adm_*`。
Windows CLI 为零差异。macOS CLI 有一个 APAC M4A 字节差异，其余退出码、正文、日志和产物一致。
两平台各有三项 help 文本因可执行文件路径不同而产生信息性差异。

macOS 参照首次采集的 APAC `mdat` 为 88,942 字节，同一二进制随后三次编码均为 88,941 字节。
候选三次编码也均为 88,941 字节，payload SHA-256 与参照的重复编码和候选首次采集相同。
两者渲染的 WAVE 字节一致。此现象与 [6.2 的 APAC 重复编码记录](RUST_BOUNDARY_ISLANDS.md)一致，原始差异保留在本次验证证据中。

macOS 的 `mr_adm_rust_quality`（fmt + clippy）、FFI 头校验、Cargo 许可证覆盖、依赖清单与 C/C++ 规模棘轮均通过。

## B 批：版本、上下文与内存

7 个导出改由 `mradm-capi` 实现，`adm_c_api.cpp` 中的定义删除：

- **版本。** `adm_api_version_major` / `minor` / `patch` 返回 `c_api.h` 的 `ADM_API_VERSION_*` 宏：`mradm-capi` 的 `build.rs`
  从头文件读出三个值，版本号仍只在 `c_api.h` 一处维护。
- **上下文。** `adm_context_t` 改为 Rust 类型（`context.rs`），只保存上一次失败的消息；消息以字节加结尾 NUL 保存，读取方遇到
  第一个 NUL 停止，与 C++ `std::string::c_str()` 相同。`adm_context_last_error_message` 对 NULL 返回 `""`，消息在下一次改变它的
  调用或销毁之前有效。C++ 上下文中的 `RenderService` 不持有状态，仍在 C++ 的导出改为共用一个静态实例。
- **字符串。** `adm_free_string` 改由 Rust 释放。`c_api.h` 要求 `char**` 输出只能用它释放，因此这些字符串也统一由 Rust 分配
  （`string.rs`，分配前置记录大小的头部），分配与释放使用同一个分配器。原先 C++ 有三种分配方式：`new[]`、
  `make_unique<char[]>`，以及 OSC 快照的 `malloc`（与 `delete[]` 不配对）。Windows Debug 的 C++ 用 `/MDd` 调试堆，
  Rust 链接发布版 CRT，不能由 Rust 释放 C++ 分配的内存，所以 C++ 一侧也一并改用 Rust 分配。
- **私有入口。** 仍在 C++ 的导出经 `src/adm_c_api/capi_ffi.h` 的两个入口使用 Rust 对象：`mradm_capi_context_set_error`
  写上下文消息，`mradm_capi_string` 分配输出字符串（失败时返回 NULL，C++ 照旧返回 `ADM_ERROR_INTERNAL` 或抛 `bad_alloc`）。
  `check-ffi-headers.py` 按 `c_api.h` 的类型双向核对这些入口与 Rust 导出，`quality_scripts_test.py` 新增对应测试。
- **兜底。** `adm_create_context` 与 `mradm_capi_context_set_error` 经 `guard`；版本、读消息、销毁与释放的函数体不会 panic，
  不另包一层。context 句柄使用可失败分配，创建失败返回 NULL，空消息与清空操作不分配内存。
  消息更新先用 `try_reserve` 预留内容与结尾 NUL，失败返回 `ADM_ERROR_INTERNAL` 并保留原消息，成功后再替换。
  C++ 的 `store_last_error` 将失败转成 `bad_alloc`，由既有边界返回 `ADM_ERROR_INTERNAL`，catch 内的消息更新为尽力写入。
  故障注入回归覆盖句柄、消息扩容与输出字符串的分配失败、重试、消息保留、无分配清空与超长消息拒绝。

生产 C/C++ 21,121 → 21,098 行（B 初版为 21,100 行，`capi_ffi.h` 计入 production）。

B 初版在 Linux x64 Release 上对照 main `620f4e8` 的同配置构建：
- C ABI 采集 3,283 行、36 个文件 0 差异，consumer 测试在两个库上都通过；
- CLI 记录 102 条中 101 条相同，`render-vbap-714-mka` 的 MKA 少 1 字节：候选这次随机得到的 TrackUID 只有 7 字节
  （最高字节为 0，概率约 1/256），归一化把 UID 清零但不能抹平长度，上层 Tracks / TrackEntry 的大小随之各减 1。
  参照连录三次、候选再录两次都与参照首次逐字节相同，C++ 与 Rust 都不参与这段编码；
- 完整一致性采集：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- ctest 85/85、`mr_adm_rust_quality`、FFI 头校验（`c_api.h` 164 个函数中 7 个由 Rust 实现，私有入口 2 个）、导出校验 164 个、
  clang-format / clang-tidy、规模棘轮。本机 cppcheck 2.13 不支持仓库的抑制语法，cppcheck 留给 macOS Quality CI。

### B 批原生复核与分配失败修正

复核目标为 `566390b6`，分配失败修正为 `4939cfc3`。两平台构建并测试该修正提交，复用各自的原有 Cargo 与 FetchContent 缓存。

- macOS arm64 Debug（SOFA 开启）与 Windows x64 canonical MSVC/Ninja Release（SOFA/IAMF 关闭）均完成 C ABI 共享库、
  静态链接测试与消费者目标构建。定向 CTest 各 7/7，换库消费者测试各 2/2，`mradm-capi` 单元测试各 11/11。
- 新增的 5 个故障注入回归验证句柄和输出字符串分配失败返回 NULL、错误消息扩容失败返回 `ADM_ERROR_INTERNAL`、
  原消息与借用指针保留、失败后重试、无分配清空及超长消息拒绝。
- macOS 的 Rust fmt、`mradm-capi --all-targets` clippy、改动 C/C++ 的 clang-format / clang-tidy / cppcheck 与规模棘轮通过。
  FFI 头校验覆盖 412 个 `mradm-ffi` 函数、171 个结构体、7 个 Rust 公开导出与 2 个私有入口。两平台共享库均恰好导出 164 个 `adm_*`。
- macOS 的 C API fixture 因多声道监听设备不可用，走测试既有的设备不可用分支。

## C 批：拆分与共享类型

用户决定（2026-11-13）：
- C 拆成 C1（查询与房间）与 C2（选项、取消、渲染、结果与 export），每批各自对照后提交；
- D（preview，4 个导出）并入 C2：preview 与离线渲染共用同一条渲染路径（预导入场景、准备缓存），并入后不需要结果对象的桥，
  也不需要 preview 读选项与取消令牌的桥；
- E（Scene）、F（Monitor）中仍在 C++ 的导出经 `capi_ffi.h` 的临时私有入口读取 Rust 对象（选项转回 `RenderOptions`、
  取消令牌转为 `stop_token`、房间转为 `RoomProfile`），E、F 迁走时删除这些入口。

## C1 批：查询与房间

17 个导出改由 `mradm-capi` 实现，`adm_c_api.cpp` 中的定义与 `adm_scene_info_t` / `adm_room_t` 两个 C++ 结构删除：
`adm_render_stage_from_string`；`adm_probe_file` 与 `adm_destroy_scene_info`、6 个 `adm_scene_info_*`；`adm_inspect_file_json`、
`adm_inspect_file_xml`、`adm_policy_template_json`；`adm_room_load`、`adm_room_destroy`、`adm_room_manifest_json`、
`adm_room_inspect`、`adm_room_inspect_with_quality`。

原计划放在 C1 的 5 个能力表 JSON（`adm_capabilities_json` 等）移到 C2：它们要读 C++ 平台岛 `mradm_platform_islands()`，
是 Rust 第一次按符号名调用 C++。C2 的渲染同样要按名调用音频岛与 Apple 岛，GNU ld 的静态库顺序在 C2 一并处理。

- **同一组调用。** 每个导出照 C++ 服务原来的顺序调用同样的 Rust 入口（`mradm_scene_import`、`mradm_scene_view`、
  `mradm_scene_inspect_json`、`mradm_scene_template`、`mradm_wav_reader_open`、`mradm_wav_chunks_*`、`mradm_room_*`），
  消息缓冲同样是 1024 / 512 字节，所以消息、定位（`input=` / `path=` / `room-binaural`）与截断不变。为此 `mradm-ffi` 的
  `adm`、`room`、`scene`、`wav` 模块与用到的字段改为 `pub`，由 Rust 直接按路径调用，类型由编译器核对。
- **错误。** `error.rs` 复刻 C++ 的错误形态：`scene_error` / `wav_error` 把 1–6 以外的码归为 `internal_error`，`map_error`
  把未知码归为 INTERNAL，`store_last_error` 存 `message: locator`；C++ 异常（含消息存不下时的 `bad_alloc`）走各导出原来的
  catch：probe / inspect / 模板 / XML 写各自的 “unexpected exception …” 消息，房间只返回 INTERNAL。结果对象与复制都用可失败
  分配，失败按原消息返回 INTERNAL。
- **probe。** 导入失败时照旧退回原始 WAVE 信息（1–65535 声道），否则返回导入错误；声道数照旧经 `uint16_t` 截断。
- **XML。** `io::get_axml` 的检查原样移来：CHNA trackIndex 为 0、axml 缺失或为空都报 io_error 并带 `input=` 定位；读取中
  内存不足时消息为 “读取 AXML 失败：” 加 C++ `bad_alloc::what()` 的平台文字。
- **Windows 路径。** C++ 把调用方的窄路径按 `std::filesystem::path` 的代码页转成 UTF-8（`wav_path_utf8`）。Rust 照 MSVC STL
  的 `__std_fs_code_page` 选代码页（C 区域设置为 UTF-8 时用 UTF-8，否则按 `AreFileApisANSI` 取 ACP / OEMCP），
  `MultiByteToWideChar` 带 `MB_ERR_INVALID_CHARS`；转换失败与 C++ 抛异常同样走 catch。已知差别：XML 查询中转换失败的消息，
  C++ 附带系统的 `system_error` 文字，Rust 写 “路径无法转换为 UTF-8”。其他平台路径字节不变。
- **房间桥。** `adm_room_t` 改为持有 `mradm-ffi` 的房间包句柄。仍在 C++ 的 `to_room_config`（Scene 流的房间、Monitor / Scene
  切房间、C2 前的 `adm_render_options_set_room`）经 `mradm_capi_room_profile` 取得同一房间包的新句柄（`RoomProfileHandle::share`，
  共享 `Arc`），由 `room_internal::Access::adopt` 包成 `RoomProfile`。

生产 C/C++ 21,098 → 20,845 行（`src/adm_c_api` −265，`room_internal.h` +12）。

本机 Linux x64 Release，参照为 main `0fa10f3` 的同配置构建：
- C ABI 采集 3,283 行、36 个文件 0 差异（含 probe、缺失输入、房间加载 / 缺失 / 非房间包、两种质量与越界比例的检查，以及带房间的
  Scene 流），consumer 测试在两个库上都通过；
- CLI 记录 102 条 0 差异（3 条 help 文本只差可执行文件路径）；
- 完整一致性采集：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- ctest 85/85、`mr_adm_rust_quality`、FFI 头校验（`c_api.h` 164 个函数中 24 个由 Rust 实现、`adm_room_config_t` 与 Rust
  一致，私有入口 3 个）、导出校验 164 个、clang-format / clang-tidy、规模棘轮；`mradm-capi` 在 `x86_64-pc-windows-msvc`
  上 `cargo check` 与 clippy 通过（Windows 路径转换）。本机 cppcheck 2.13 不支持仓库的抑制语法，cppcheck 留给 macOS Quality CI。

## 下一步

C2：选项、取消、渲染、结果、export、能力表 JSON 与 preview（D）。
