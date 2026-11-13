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

### C1 审查修正与原生验证

复核目标为 `9ddeed2a`，分配失败修正为 `c96d155d`：
- Rust 房间句柄分配失败时，由传入值的析构负责释放已加载的房间包，不再二次释放；C++ 房间桥在分配
  `RoomProfile` 和 `shared_ptr` 控制块期间分别由唯一所有者负责清理。
- 错误消息、定位字符串及 Windows UTF-16 到 UTF-8 的复制使用可失败分配；失败沿原有异常分支返回
  `ADM_ERROR_INTERNAL`，不通过 Rust 的不可恢复分配入口。正常消息、定位和路径编码规则保持不变。
- 新增故障注入回归覆盖消息与定位复制、XML 定位、非法房间配置、房间句柄和 C++ 共享所有者分配，
  验证返回值、空输出和失败后的重试。C++ 回归在修复前的房间桥上发生段错误，修复后通过。

两平台构建并验证修正提交，复用原有 Cargo 与 FetchContent 缓存：
- **macOS arm64 Debug：** `mradm-capi` 26 个单测、C ABI／房间／质量脚本的 8 项定向 CTest、
  两组共享库消费者测试通过；141 项 probe／inspect／策略模板／房间查询与 main `0fa10f3f` 对照无差异。
- **Windows x64 canonical MSVC/Ninja Release（SOFA/IAMF off）：** `mradm-capi` 25 个单测、相同的
  8 项定向 CTest 和两组消费者测试通过；以 main `0fa10f3f` 的 DLL 为参照，在同一修正构建中运行
  `capi-capture.py --family render`，2,293 行与 23 个文件无差异，包含查询、房间、选项与离线渲染。
- 两平台共享库均恰好导出 164 个 `adm_*`。macOS 的 FFI 头校验、Rust fmt／clippy、
  clang-format／clang-tidy／cppcheck 和规模棘轮通过；生产 C/C++ 20,845 → 20,841 行。
- 两平台消费者测试中的 Triple Balance 多声道监听设备不可用，走测试既有的设备不可用分支。

## C2 批：选项、取消、渲染、结果、export、能力表与 preview

67 个导出改由 `mradm-capi` 实现，`adm_c_api.cpp` 中的定义与 `adm_render_options_t` / `adm_cancel_token_t` /
`adm_render_result_t` / `adm_preview_session_t` 四个 C++ 结构、C ABI 的 `RenderService` 单例、进度与日志收集器删除（C++ `RenderService` 留给 CLI）：
`adm_create_render_options` / `adm_destroy_render_options` 与 39 个 `adm_render_options_set_*`；取消令牌 4 个；
`adm_render_file` / `_ex` / `_ex2`；`adm_render_result_*` 9 个与 `adm_destroy_render_result`；`adm_export_file`；
`adm_capabilities_json`、`adm_layouts_json`、`adm_output_formats_json`、`adm_input_layouts_json`、`adm_support_matrix_json`；
preview 4 个（D 并入）。

- **选项。** Rust `adm_render_options_t` 持有与 C++ `RenderOptions` 相同的缺省值；每个 setter 照原样校验（NULL 选项除
  `set_room` / `set_room_quality` / `set_screen_config_json` 外返回 OK），逗号列表照 `split_csv_list` 去掉两端空白，
  输出文件名照 `std::filesystem::path::filename` 的规则（含 Windows 根名）。渲染时转成 `mradm-ffi` 的
  `EngineRenderRequest`，与 C++ `RenderService` 传给 Rust 状态机的字段一致。
- **取消。** 令牌复刻 `std::stop_source` 的语义：渲染或 preview 会话开始时取得当前停止状态，`adm_cancel` 可在任意线程置位，
  `adm_reset_cancel_token` 换上新状态，已取得的状态不受影响、比令牌活得更久。reset 时内存不足与 C++（`noexcept` 中抛出）同样终止进程。
- **渲染。** `render.rs` 照 `RenderService::render` 驱动同一个 `mradm-engine` 分步状态机（begin / 岛 / next / resume /
  prepared / result），错误消息与定位不变；音频岛 `mradm_audio_island`、平台岛 `mradm_platform_islands` 与（macOS）
  Apple 岛 `mradm_apple_islands` 由 Rust 按符号名调用。本机 GNU ld 下没有出现静态库顺序问题：岛与 C ABI 同在共享库 /
  可执行文件的链接单元内。进度 v1 / v2 的阶段名、fraction 钳位、阶段与操作编号映射与日志级别收集照原样移来。
- **回调异常。** C++ 调用方可以从进度回调抛异常，原实现返回 `ADM_ERROR_INTERNAL`、不产出结果、保留原输出并清掉临时文件（C ABI 测试
  覆盖）。C++ 异常不能穿过 Rust 栈帧，所以回调经 `adm_c_api.cpp` 中的 `mradm_capi_progress_v1` / `_v2` 两个 C++ 跳板调用，
  跳板捕获异常并告诉 Rust 中止；行为不变。是否改变这一语义另行决定，不在本批。
- **结果。** `adm_render_result_t` 改为 Rust 类型，访问器与销毁照原样；消息、输出路径、日志与报告由 Rust 持有到销毁。
- **Windows 结果路径。** 引擎给出 UTF-8 输出路径，C++ 经 `std::filesystem::path::string()` 转回窄路径。Rust 照同一代码页规则
  先转 UTF-16、再以 `WC_NO_BEST_FIT_CHARS` 转窄；转换不了时与 C++ 抛异常同样返回 `ADM_ERROR_INTERNAL`、不产出结果。其他平台字节不变。
- **export。** 照 `RenderService::export_file` 的顺序：导入（不路由）、视图检查、应用语义策略、写回 AXML、`mradm_wav_replace_axml`，
  定位字符串不变。
- **能力表。** 5 个 JSON 由 Rust 经平台岛取事实后调用同样的 `mradm-engine` 发现文档入口；C++ `catalog.cpp` 只剩 CLI 把文档解析回行。
- **preview。** 会话创建时复制选项并取得取消状态，与原实现一样；窗口渲染（v1 / v2）走同一条渲染路径。
- **给 E / F 的桥。** 仍在 C++ 的 `adm_create_monitor_ex` 与 `adm_monitor_switch_backend` 经 `capi_ffi.h` 的
  `mradm_capi_options` 取得选项的只读视图（文本、列表、屏幕与房间包），在 C++ 侧转回 `RenderOptions`，用完以
  `mradm_capi_options_release` 释放；F 迁走时删除。
- **`mradm-ffi`。** `engine`、`engine_render`、`engine_service`、`render` 模块与用到的字段改为 `pub`（`doc(hidden)`），
  由 `mradm-capi` 按路径直接调用，类型由编译器核对。`check-ffi-headers.py` 另剥去 cbindgen 输出中的 `extern` 导入声明
  （Rust 调用的 C++ 岛与跳板）。

生产 C/C++ 20,841 → 19,855 行（`src/adm_c_api` −986）；Quality CI 的 cppcheck 修正（`capi_ffi.h` 登记为私有 FFI 记录、删去迁走导出留下的 `cppcheck-suppress-end` 与空分节注释）后为 19,835 行。

本机 Linux x64 Release，参照为 main `e2fc6ef` 的同配置构建：
- C ABI 采集 3,283 行、36 个文件 0 差异（含选项 setter 的校验、离线渲染与进度 v1 / v2、取消、结果访问器、export、能力表与
  preview 窗口），consumer 测试在两个库上都通过；
- CLI 记录 102 条 0 差异（3 条 help 文本只差可执行文件路径）；
- 完整一致性采集：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- ctest 86/86、`mr_adm_rust_quality`、FFI 头校验（`c_api.h` 164 个函数中 91 个由 Rust 实现、2 个结构体与 Rust 一致，
  私有入口 5 个）、导出校验 164 个、clang-format / clang-tidy、规模棘轮；`mradm-capi` 在 `x86_64-pc-windows-msvc` 上
  `cargo check` 与 clippy 通过（Windows 结果路径转换）。cppcheck 同样留给 macOS Quality CI；macOS（Apple 岛按名调用）与
  Windows 的原生构建与对照待维护者。

## E 批：Scene 流与 Scene 输出

38 个导出改由 `mradm-capi` 的 `scene.rs` 实现：Scene 流的创建（4 个入口，含房间、屏幕 JSON 与房间档位）、销毁、消息、输出格式、
epoch、generation、提交帧、结束、拉取、状态、切换后端（含 `_ex`）、听者朝向、语义策略 JSON 与诊断（2 个）；Scene 流的房间切换、
房间状态与声源 JSON；Scene 输出的创建、销毁、消息、播放 / 暂停、epoch、音量、状态与 HpTF（8 个设置 / 查询）。
`adm_c_api.cpp` 中的定义、`adm_scene_stream_t` / `adm_scene_output_t` 两个 C++ 结构与只服务它们的辅助函数删除。

- **同一组调用。** 每个导出照原来的 C++ 薄壳（`SceneStreamEngine`、`SceneOutputSession`、`SceneRendererRequest`、
  `RoomControl`、`prepare_hptf`）的顺序调用同样的 `mradm-ffi` 入口（`mradm_engine_scene_*`、`mradm_engine_scene_output_*`、
  `mradm_room_control_*`、`mradm_dsp_hptf_settings_*`、`mradm_scene_screen_parse`、`mradm_room_validate_screens`），消息缓冲与
  原来相同（512 字节起，配置 generation 与语义策略另加 1024，渲染器请求另加布局与路径长度，输出设备另加 4096，屏幕 JSON 另加输入长度），
  所以消息、定位与截断不变。为此 `mradm-ffi` 的 `engine_scene`、`engine_scene_output`、`room_ffi`、`device_island`、`hptf` 与
  `hptf::settings` 模块及用到的记录字段改为 `pub`（`doc(hidden)`），由 `mradm-capi` 按路径调用。
- **校验与消息。** 记录的 `struct_size` 与数组步长照 `load_sized_array` / `output_struct_valid` / `write_sized_output` 处理；各项校验的
  顺序与消息照原样（例如渲染器配置先查 `struct_size`，再查取值，再查布局与 SOFA）。Scene 流的消息与诊断文本在锁内保存，返回的
  `const char*` 有效期不变；Scene 输出的错误消息照旧只存消息、不带定位，`_ex` 入口不加锁、错误消息由调用方释放。
- **设备岛。** Scene 输出经 `mradm_device_islands()` 打开设备，由 Rust 按符号名调用；设备抛出异常（代码 100）照旧返回
  `ADM_ERROR_INTERNAL`、不写上下文消息。
- **房间 JSON。** 状态与声源文档由 `mradm-json` 写出，键序、数字与转义与 nlohmann `dump()` 相同；未知状态或非 UTF-8 消息照旧返回
  `ADM_ERROR_INTERNAL`（nlohmann 抛异常的情形）。
- **HpTF 信息。** 左右是否相同照 `HptfCoefficients::operator==` 逐项比较（IEEE 浮点相等）；单声道查询在左右不同时照旧返回
  `ADM_ERROR_UNSUPPORTED` 与原消息。
- **输入队列预算。** 引擎按 C++ `live_scene` 记录的 `sizeof` 计入输入字节（帧 104、平面 40、初始状态 80、更新 120 字节）。
  Rust 取 64 位标准库（libstdc++ / libc++ / MSVC 发布配置）下的这组值。已知差别：MSVC 调试配置开启迭代器调试后
  `std::vector` 为 32 字节，原 C++ 在该配置下计入的帧与平面字节更大，Rust 不再随 C++ 调试配置变化。
- **Windows SOFA 路径。** C++ 把调用方的窄路径交给 `std::filesystem::path`；Rust 照 C1 的代码页规则转 UTF-8。转换失败时原消息
  附带系统的 `system_error` 文字，Rust 写 “路径无法转换为 UTF-8”（与 C1 的 XML 查询相同）。
- **静态链接。** 只使用 C ABI 的静态链接程序（两个 Scene C ABI 测试）不再引用 `adm_c_api.cpp` 中的任何 C++ 定义，而 Rust 库按名
  调用 C++ 的进度跳板与各岛；这些静态库在链接行上位于 Rust 库之前，单遍链接器（GNU ld、lld）不会回头取。`mr_adm_c_api` 静态库因此
  在支持 `LINK_GROUP:RESCAN` 的平台上向使用方加 `--undefined=` 这五个符号，链接器在扫描对应静态库时即取出它们。共享库与
  macOS / Windows 的链接器不受影响。
- **薄壳移出生产。** C ABI 不再使用 `SceneStreamEngine`、`SceneOutputSession` 与 `SceneRendererRequest`，它们只剩测试与手动
  benchmark 使用，移到 `tests/support/scene_stream` 与 `tests/support/live_scene`（新测试支持库 `mr_adm_scene_stream_test_support`），
  算法不变。顺带修正 `hptf_parameters_test.cpp` 中局部变量与成员函数同名（cppcheck `shadowFunction`）。

生产 C/C++ 19,835 → 18,018 行（`src/adm_c_api` −907，三个薄壳移出 −910）。

本机 Linux x64 Release，参照为迁移前的同配置构建（源码 `2ca9aff`，与 main `6ab5ab9` 只差注释与抑制清单）：
- C ABI 采集 3,283 行、36 个文件 0 差异（含 Scene 流的创建、配置、提交、拉取、状态、诊断、切换、策略，房间流与 NULL 设备输出），
  consumer 测试（含 Scene 流 consumer）在两个库上都通过；薄壳移动、格式与单元测试加入后重录仍为 0 差异；
- CLI 记录 102 条 0 差异（3 条 help 文本只差可执行文件路径；首次记录中 1 条为 Opus MKA 随机 TrackUID 的长度差，重录一致）；
- 完整一致性采集：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- ctest 86/86、`mradm-capi` 单测 42 个（新增 7 个：渲染器配置校验顺序、步长数组、`struct_size` 输出、房间 JSON、HpTF 左右比较、
  空句柄、消息分配失败）、FFI 头校验（`c_api.h` 164 个函数中 129 个由 Rust 实现、25 个结构体与 Rust 一致）、导出校验 164 个、
  clang-format、clang-tidy、本机 cppcheck（改动文件）、规模棘轮、许可证；`mradm-capi` 在 `x86_64-pc-windows-msvc` 上 clippy 通过。
  两平台的后续原生构建与定向测试见下；本轮未重录 CLI／PCM 全量对照。

### E 审查修正与原生验证

复核提交 `20ceee14`，修正提交 `fbe5b554`（2026-11-13）：房间状态与声源 JSON 的普通堆分配在失败时会直接终止宿主，
外围 `catch_unwind` 无法恢复。改为通过 `try_reserve` 扩展输出缓冲；字符串转义与 Grisu2 浮点格式仍由 `mradm-json`
写出，其标量写入器现在接受可失败的 `fmt::Write`，不再分配临时转义／指数字符串。键序、数字和转义保持原输出。

- 逐次注入 JSON 构造及最终 C ABI 字符串复制的分配失败，验证返回 `ADM_ERROR_INTERNAL`、输出保持 `NULL`，随后重试成功。
  覆盖长 Unicode／控制字符消息、最大整数、科学计数法、有限浮点极值及非有限值；未知状态和非法 UTF-8 仍不发布输出。
- 复用原有 Cargo 与 FetchContent 缓存，重新构建 C ABI 共享库、Scene／房间／HpTF 相关目标和迁移后的测试支持库。

| 平台与配置 | `mradm-capi` 单测 | `mradm-json` 测试 | 定向 CTest |
| --- | ---: | ---: | ---: |
| macOS arm64 Debug，SOFA on | 44/44 | 7/7 | 10/10 |
| Windows x64 canonical MSVC/Ninja Release，SOFA/IAMF off | 43/43 | 7/7 | 10/10 |

JSON 测试包含 nlohmann/json 3.12.0 的约 400 万组浮点黄金对照、解析／转义回归与可失败输出器测试。
定向 CTest 覆盖 C ABI fixture／C 头、Scene 流式渲染／Scene 输出、房间、HpTF 参数与双耳接口、峰值保护、回调分配和质量脚本。
两平台的 `mradm-capi`／`mradm-json --all-targets` clippy 通过；macOS 的 Rust fmt、改动 C/C++ 的
clang-format／clang-tidy／cppcheck、规模棘轮与许可证检查通过。
FFI 头检查确认 164 个公开函数中 129 个由 Rust 实现，25 个公开结构体一致；两平台共享库均恰好导出 164 个 `adm_*`。
验证范围为上述正确性与错误恢复检查，不作为性能或完整节目 PCM 对照结论。

## F 批：Monitor、HpTF 文本解析与 OSC 头追踪

最后 35 个导出改由 `mradm-capi` 实现，`c_api.h` 的 164 个函数全部在 Rust：
- `monitor.rs`：Monitor 的创建（2 个）、输出设备 JSON、销毁、消息、播放 / 暂停 / 定位 / 循环、实时覆盖、听者朝向、切换后端、
  HpTF 设置（4 个）与查询（2 个）、输出设备切换、状态、电平、日志（2 个）、房间切换与房间状态 / 声源 JSON，以及
  `adm_hptf_parse_parametric_eq`；
- `osc.rs`：OSC 头追踪接收器的创建、销毁、启动、停止、姿态、状态、快照 JSON 与错误消息。

`adm_c_api.cpp` 只剩两个进度回调跳板（27 行）；`capi_ffi.h` 删除，`mradm-capi` 不再导出任何私有入口
（`mradm_capi_options`、`mradm_capi_room_profile`、`mradm_capi_context_set_error`、`mradm_capi_string` 改为 crate 内函数），
C ABI 目标不再链接 nlohmann_json。

- **同一组调用。** Monitor 照 C++ `MonitorSession` 的顺序：照预览导入场景（`prepare_preview_scene`：导入、`importer` 警告、
  语义策略及其 `semantic-policy` 日志）、读场景格式、创建会话并回放这些日志行、以同一请求记录启动；之后各操作调用同样的
  `mradm_engine_monitor_session_*` 入口，消息缓冲同为 64 KiB（覆盖的定位另按最长对象 ID 加长），岛抛出（代码 100）照旧落到各导出的
  “unexpected exception …” 消息。HpTF 设置与查询、房间切换与 JSON 复用 E 批的 Rust 函数（房间 JSON 含 `fbe5b554` 的可失败分配）；
  会话没有 HpTF 快照时照旧报告默认的旁路信息。
- **消息与怪癖照旧。** Monitor 的错误消息为 `message: locator`；实时覆盖失败照原样总写 `message: locator`（即使定位为空）并返回未映射的
  错误码；`adm_monitor_log_count` 照原样截断为 32 位；输出设备 JSON 只转义 `"`、`\`、`\n`、`\r`、`\t`；HpTF 文本解析的
  错误消息、行定位与 `band=N` 定位照 `parse_parametric_eq`；OSC 的姿态与状态照原样写满结构并把 `struct_size` 设为本库大小。
- **共享库的岛符号。** 原来 C++ 的 Monitor 导出会把引擎静态库中的设备岛与平台岛对象带进 `libmradm_capi`；F 之后共享库内只有
  Rust 引用它们，GNU ld 链接共享库时允许未定义符号，于是 `mradm_device_islands` 在加载时才报缺失（`room_96k` / HpTF 立体声的
  Python C ABI 测试发现）。E 批给静态 C ABI 使用方加的 `--undefined=` 列表改为共用，共享库同样加上；新库的未定义符号与迁移前相比只少不多
  （不再拉进 C++ 的 JSON、异常与文件系统代码）。
- **C++ SDK。** `MonitorSession`、`OscHeadTrackingReceiver` 与 `parse_hptf_parametric_eq` 不再被 C ABI 使用，只剩手动探针与测试；
  它们声明在公开 C++ 头 `include/adm/monitor.h` / `head_tracking.h` / `hptf.h` 中，按路线留到 6.5（删除 C++ 外壳与 SDK）。
- **校验脚本。** `check-ffi-headers.py` 的 `CAPI_FFI_HEADERS` 为空：`mradm-capi` 没有私有入口，只有对 C++ 跳板的 `extern` 导入；
  `quality_scripts_test.py` 改用自带的私有头测试双向核对，并断言仓库当前没有私有入口。

生产 C/C++ 18,018 → 16,705 行（`src/adm_c_api` 1,340 → 27）。

本机 Linux x64 Release，参照为 main `cad0993` 的同配置构建：
- C ABI 采集 3,283 行、36 个文件 0 差异（Monitor 与 OSC 按采集范围只到参数校验），consumer 测试在两个库上都通过；
- 差异探测（临时脚本，未入库）在同一进程分别加载两个库，59 项 0 差异：本机 Monitor 能落到无硬件的后备设备创建，因此覆盖了
  7 种渲染器 / 布局的创建与失败消息、语义策略错误、缺失输入、输出设备 JSON，以及已创建 Monitor 的覆盖（新旧结构尺寸与非法值）、
  朝向、定位、循环、HpTF 参数设置与查询、状态、电平、房间 JSON 与切换、切换后端（成功与失败）、切换设备与日志；HpTF 文本解析
  7 种输入 × 3 种容量；OSC 接收器的 5 种配置、状态、姿态、错误消息与快照 JSON；
- CLI 记录 102 条 0 差异（3 条 help 文本只差可执行文件路径）；
- 完整一致性采集：PCM 输出 294、fixture 220、比较 989、重放 176、产物 256、日志 666 全部相同；
- ctest 86/86、`mradm-capi` 单测 48 个（新增 4 个：设备名转义、HpTF 文本解析消息、覆盖结构的旧尺寸、空 Monitor）、
  FFI 头校验（`c_api.h` 164 个函数全部由 Rust 实现、31 个结构体与 Rust 一致、私有入口 0 个）、导出校验 164 个、共享库未定义符号对照、
  clang-format、clang-tidy、本机 cppcheck、规模棘轮、质量脚本测试；`mradm-capi` 在 `x86_64-pc-windows-msvc` 上 clippy 通过。
  两平台的原生构建与定向验证见下。

### F 审查修正与原生验证

复核提交 `889454cd`，修正提交 `3bcef796`（2026-11-13）。HpTF 文本解析的 `band=N` 错误定位原先通过
`usize::to_string` 分配临时字符串，分配失败会直接终止宿主。现在先在栈缓冲中格式化，再通过可失败的复制生成定位。
新增故障注入测试覆盖编号定位的分配失败及重试，并验证公开解析接口返回 `ADM_ERROR_INTERNAL`、保留输出值且可继续使用。

| 平台与配置 | `mradm-capi` 单测 | 定向 CTest |
| --- | ---: | ---: |
| macOS arm64 Debug，SOFA on | 50/50 | 10/10 |
| Windows x64 canonical MSVC/Ninja Release，SOFA/IAMF off | 49/49 | 11/11 |

定向 CTest 覆盖 OSC 收发与协议参考、Scene 流式渲染与 Scene 输出 C ABI、HpTF 参数与双耳接口、文本解析参考、
C 头及质量脚本，Windows 另含完整 C ABI fixture。Windows 的两组共享库消费者测试通过。
两平台的 Monitor／OSC 参数与空句柄采集均为 90 行，与主线 `cad0993a` 逐字节一致，共享库均恰好导出 164 个 `adm_*`。
两平台 `mradm-capi --all-targets` clippy 通过，macOS 的 Rust fmt、FFI 头校验、改动 C/C++ 的
clang-format／clang-tidy／cppcheck 与规模棘轮通过。构建复用既有 Cargo、FetchContent 与 canonical 缓存。

## 下一步

6.3 的 C ABI 迁移完成。后续：进度回调的 C++ 跳板保留到另行决定回调异常语义；`MonitorSession` / `OscHeadTrackingReceiver`
等 C++ SDK 外壳在 6.5 删除；6.6 改为 cdylib 时再核对 SONAME / install_name / `.def` 与 GUI 打包。
