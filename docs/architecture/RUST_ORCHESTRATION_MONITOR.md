# Monitor 与实时调度迁入 Rust（五期 5.5）

> 五期 5.5 的记录（2026-11-10 至 2026-11-11）。路线见[审计五期一节](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)，
> 驱动面清单见 [5.1 记录](RUST_ORCHESTRATION_BASELINE.md)，Monitor 经由的文件流式见 [5.3 记录](RUST_ORCHESTRATION_STREAM.md)，
> 共用的输出环与 Windows room worker 服务见 [5.4 记录](RUST_ORCHESTRATION_SCENE.md)。

## 范围

5.5 把实时监听的调度、会话与 OSC 头追踪接收移入 Rust：
- `MonitorEngine` 与 `monitor_session` 移入 `rust/crates/mradm-engine` 的 `monitor` 模块；
- PoseBridge 的 UDP 接收器移入 `rust/crates/mradm-osc` 的 `receiver` 模块。

设备留作岛，由 C++ 外壳经宿主表交给 Rust。这些设备是 miniaudio、Apple `AVSampleBufferAudioRenderer`、Windows `ISpatialAudioClient` 与测试用的捕获设备。C ABI 不变。

| 批次 | 原 C++ | Rust |
| --- | --- | --- |
| A（`a74c923`） | `MonitorEngine` 的调度：worker 与命令顺序、输出环、seek 的 10 ms 桥接与延后的设备 flush、loop（含输出级两段式回绕）、实时编辑与 revision、听者朝向快照、同步与后台预热的后端切换（2048 帧交叉淡化或输出级硬切）、HpTF、LUFS 表、电平与失败状态；`ring_buffer.h` | `monitor`（`mod` / `worker` / `pull` / `native`），输出环与 Scene 共用 `ring`；FFI `mradm_engine_live_monitor_*` |
| B（`a46c67a`） | `MonitorSession`：会话日志、原 `RealtimeStreamFactory` 的打开流程、上次与待用的后端选择、重建时保留的编辑 / 朝向 / HpTF 档案、秒到帧换算、后端切换的检查与回退、输出设备重建与回退到默认设备、HpTF 告警、会话失效后的回答 | `monitor::session`；FFI `mradm_engine_monitor_session_*` |
| C（`b82987c`） | `OscHeadTrackingReceiver`（517 行）：回环 UDP socket、接收线程、来源过滤、姿态排序、遥测缓存与匹配、新鲜度与心跳、快照 JSON、错误文字 | `mradm-osc` `receiver`；FFI `mradm_osc_receiver_*` 与 `mradm_osc_text_*`，Windows 的独占绑定在 `mradm-ffi` `osc::winsock` |

审计列在 5.5 下的其余几项，这次之前就已经迁出，或已没有生产调用者：
- downmix：Monitor 不做扬声器或 HOA 的 downmix，重建时必须保持输出声道数，C++ 会话原来就这样规定。`DownmixStream` 已只剩测试在用，C 把它移到 `tests/support/realtime`。
- 立体声峰值保护：只用在 Scene 输出会话上，5.4 C 已迁入 Rust（`mradm-dsp` `peak_guard`）。
- Windows room worker 的 MMCSS 与定时器：5.4 B 起由 `mradm-ffi` `scene_worker` 提供，Monitor 经宿主的 `room_worker` 调用。C++ 的 `room_worker_wait.*` 只剩两个手动 room 探针在用，C 把它移到 `tests/support/realtime`。
- 设备调度：B 起由 Rust 会话经宿主表驱动设备（start / stop / flush / pause / resume / 销毁）。

### C++ 剩下的部分

`src/adm_realtime` 剩 1,366 行：
- **`osc_head_tracking.cpp`**（126 行）：`mradm_osc_receiver_*` 的薄壳。保留 `OscHeadTrackingReceiver` 的 C++ 类接口，快照结构的遥测字符串字段变更见发布说明。C ABI 与手动探针都没有改。
- **`scene_stream_engine.*`**（518 行）：Scene 流的薄壳，见 5.4 记录。
- **`island_stream.*`**（213 行）：C++ `IRenderStream` 的岛表，Apple 后端与测试流经它交给 Rust Monitor。
- **设备岛**：
  - `miniaudio_device.cpp`（215 行），留作 C 岛；
  - `audio_output_device.h`；
  - Apple 用的 `buffered_media_timeline.h`。
- **私有 FFI 头**：`osc_ffi.h`。

`src/adm_engine` 中，`monitor_session.cpp` 是 B 留下的外壳，提供设备、Apple 岛与各选择的 `RenderOptions`。

测试专用 C++：
- `tests/support/realtime`：`MonitorEngine` 外壳与 `IRenderStreamFactory`（B）、`DownmixStream`、`room_worker_wait`（C）；
- `tests/support/osc`：PoseBridge 解码与 `OscSourceOrder` 的 C++ 值包装（C）；
- `tests/support/dsp`：`ring_buffer.h`、`monitor.h` 与 `monitor_ffi.h`（A）。

### 保持不变的规则

- **字节与文字**：PCM、状态、电平、LUFS、日志、错误码与文字逐项相同；例外见发布说明。
- **实时约束**：
  - 设备回调不等待 worker、不分配；
  - A 的 `tests/monitor_pull_allocations.rs` 覆盖了播放、seek 桥接、HpTF 淡化、欠载、暂停与结束之后的回调。
- **OSC**：
  - 接收器只绑定 127.0.0.1，绑定失败时不改选别的端口；
  - 非回环来源的数据报计入 `rejected_packets`；
  - 每个数据报最多 64 KiB，解码规则仍是 `mradm-osc` `decode`；
  - 快照 JSON 是 nlohmann `dump()` 的字节，现在由 `mradm-json` 写出。
- **棘轮**：每批在同一提交运行 `--update`。

## 发布说明

1. **A**：
   - 岛流的 `process` / `seek` 抛出的异常，由 `std::terminate` 改为带 `what()` 文字的 render_failed 错误；
   - 岛错误文字截断到 4096 字节；
   - `check-ffi-headers.py` 改为也比较函数指针类型的结构体字段；
   - 维护者修正：
     - `8d58857`：播放前初始化回调锁；
     - `b87d5a3`：上下文容量改用标准算法。
2. **B**：
   - Apple 后端的渲染器与准备状态改归其流所有，随流释放，不再在会话期间累积（用户决定）；
   - 同一个 monitor 上的会话调用由 Rust 会话串行化，C ABI 的并发调用不再竞争；
   - 创建设备时分配失败改为终止，不再传出 `std::bad_alloc`；
   - 错误文字经 64 KiB 缓冲区传递。
3. **C**：
   1. **等待方式**：
      - 原接收线程用 20 ms 的 `poll` 等待，再非阻塞 `recvfrom`；现在改为阻塞 `recv_from`，读超时 20 ms（`SO_RCVTIMEO`）。`stop` 等待接收线程退出，实测见下文；
      - 因为没有 `poll` 了，“OSC UDP 等待失败”与“OSC UDP socket 不可用”两条文字不再出现，socket 错误统一报“OSC UDP 接收失败 (错误码)”。
   2. **Linux / macOS 的绑定**：
      - `std::net` 一次调用完成创建 socket 与绑定，失败文字按错误类型选：地址占用、地址不可用、权限被拒报“OSC 回环端口绑定失败，端口可能已被占用”，其余报“OSC UDP socket 创建失败”；
      - “OSC UDP socket 配置失败”只在设置读超时失败时出现；
      - close-on-exec 仍由 `std` 设置（Linux 用 `SOCK_CLOEXEC`，macOS 用 `FIOCLEX`）。
   3. **Windows 的绑定**：保留原步骤与文字，即 `WSAStartup`、`WSASocketW`（不继承句柄）、`SO_EXCLUSIVEADDRUSE`、`bind`，每个接收器各自配对 `WSACleanup`。另加 `WSA_FLAG_OVERLAPPED`，因为读超时只对 overlapped socket 有效。
   4. **线程安全**：
      - `start` 与 `stop` 改由接收器内部串行化，原来要求调用方串行；
      - `snapshot` 与错误查询仍可与 `start` / `stop` 并发；
      - 销毁仍不能与任何调用重叠。
   5. **C++ SDK 头**：`OscHeadTrackingSnapshot` 删除 `info_json` 与 `status_json`，原来只有 `snapshot_json` 用它们，遥测仍在快照 JSON 里。C ABI 不变。
   6. **其他**：
      - 接收线程命名为 `mradm-osc`；
      - Rust panic 在 C ABI 返回 `ADM_ERROR_INTERNAL`；
      - `adm_create_osc_head_tracking` 直接调用 `mradm_osc_valid_source_id` 校验来源过滤。
   7. **移出生产**：
      - `osc_head_tracking_protocol.*` 移到 `tests/support/osc`（`mr_adm_osc_protocol_test_support`）；
      - `downmix_stream.h` 与 `room_worker_wait.*` 移到 `tests/support/realtime`，两个手动 room 探针改为自己编译 `room_worker_wait.cpp`，Windows 上链接 `avrt`；
      - `mr_adm_realtime` 不再链接 `ws2_32`，也不再引入 nlohmann。

## 测试与植入缺陷

| 批次 | 新测试 | 植入并被抓到的缺陷 |
| --- | --- | --- |
| A | `mradm-engine/tests/monitor_engine.rs`（10 个）与 `tests/monitor_pull_allocations.rs` | 6 处：loop 钳制、incoming seek 前不重放编辑、暂停中的 seek 立即 flush、欠载条件、结束标记条件、跳过当前 validator |
| B | `mradm-engine/tests/monitor_session.rs`（10 个，模拟宿主、设备与岛流） | 6 处：编辑不恢复、回退返回恢复错误、不回退设备、保留被拒的编辑、朝向不归中、切换检查的顺序 |
| C | `mradm-osc` `receiver::tests`（6 个：绑定与端口释放、非法来源过滤、绑定失败文字、来源过滤与姿态排序、遥测匹配与过期、快照 JSON 的 nlohmann 字节）；原有的 C ABI 契约测试 `mr_adm_osc_head_tracking_tests` 不改，直接驱动 Rust 实现 | 4 处，由新旧对照探针抓到：遥测缓存 17 → 16（6 处差异）、`changes_reference` 的 `>=` 改成 `>`（228 处）、心跳样本数的 `>=` 改成 `>`（441 处）、删去遥测的会话规则（7,212 处） |

## 验证

| 批次 | 基线 | 对照 | 结果 |
| --- | --- | --- | --- |
| A | `da5e94d` | 新旧探针驱动生产流工厂（VBAP、双耳、HOA、EAR、Triple Balance、room）与一个 C++ 岛流，在手动 sink 上跑脚本会话：播放、编辑、被拒的编辑、播放中与暂停中的 seek、loop、后端切换、HpTF、播放到结束 | PCM、状态、电平、LUFS 与日志在每份构建上跑两次都相同 |
| B | `2a73536` | 新旧探针在手动设备上驱动 `MonitorSession` 的公开 API（VBAP、双耳、Triple Balance、room、HOA）：播放、编辑、暂停中带 loop 的 seek、后端切换、设备切换、设备失败与回退、HpTF 与削波告警、被拒的改声道切换、播放到结束 | PCM、状态、电平、LUFS、错误、HpTF 信息与日志在每份构建上跑两次都相同 |
| C | `b87d5a3` | 新旧探针：原 C++ 接收器（`HEAD` 源码，改名到 `legacy` 命名空间）与新接收器编进同一个进程，收同一串数据报。共 12 个种子，每个 1,500 个数据报，内容包括姿态（推进、重复、回退、换实例 / 会话 / 参考 / 修订、各种采样时钟）、info / status 遥测（各种状态、BOM、浮点 / 大整数 / 转义的附加字段）、非法数据报、协议不符与其他来源；第 3、6、9、12 个种子带来源过滤，偶数种子中途静默 620 ms；另外比较端口占用与非法过滤的启动失败 | 13,203 项比较 0 差异。比较内容：快照的全部字段（时间字段除外），屏蔽端口与时间之后的快照 JSON 字节，`last_error`，启动结果与错误文字 |

- **实现方常规检查**（Linux x64）：
  - A：ctest 83/83 连跑 3 次，`mr_adm_realtime_tests` 50 次，Triple Balance 监听基准 60 s 欠载 0；
  - B：ctest 83/83 连跑 3 次，`mr_adm_realtime_tests` 30 次，会话测试 20 次，监听基准 60 s 欠载 0；
  - C：全量 ctest 81/81 跑 1 次（本机缺 room HRIR 准备所需的 Python 依赖，所以比 83 个少 2 个），`mradm-osc` 单元测试通过；没有重复跑；
  - clippy 与 fmt 通过，C 另对 `x86_64-pc-windows-msvc` 跑了 clippy，覆盖 `osc::winsock`；
  - FFI 头校验：A 后 383 个函数、157 个结构体，B 后 403 / 158，C 后 412 / 159；
  - C 改动的 C++ 文件 clang-format 与 clang-tidy 通过；许可、retention、version 与规模棘轮通过；受影响的手动目标（两个 room 探针、OSC 探针与 C 示例）都能编译。

### 维护者审查与合并验证

2026-11-10 审查 C 的候选 `1a64c18b`（基线 `b87d5a3a`），随后合入主线为 `0f2449de`。
已核对合并提交与候选的 Git tree 相同。

| 主机与提交 | 检查 | 结果 |
| --- | --- | --- |
| macOS arm64，候选 `1a64c18b` | `cargo test --locked -p mradm-osc`，候选 C++ 翻译单元重编译并与候选 Rust 库链接后运行两项 OSC 测试 | Rust 单元测试 16/16，C ABI 契约测试通过，协议差分覆盖 294,704 个数据报（5,641 个接受）并通过 |
| macOS arm64，合并 `0f2449de`，Debug | 用 CMake 构建两项 OSC 测试、`mr_adm_realtime_tests` 与两个 room 手动探针，运行三项定向 CTest | 构建与链接通过，CTest 3/3 通过 |
| Windows x64，候选 `1a64c18b`，canonical MSVC/Ninja Release，SOFA/IAMF off | 构建并运行 `mr_adm_osc_head_tracking_tests`、`mr_adm_osc_protocol_reference_tests`，构建 `mr_adm_room_scene_benchmark` 与 `mr_adm_room_file_device_probe` | CTest 2/2 通过，覆盖端口占用、释放和停止等 C ABI 契约，两个 room 探针含 `avrt` 的链接通过 |

- FFI 头校验通过，412 个函数、159 个结构体与 Rust 一致。`cargo fmt --all -- --check`、相对主线的 C/C++ 规模棘轮及 `git diff --check` 通过。
- Windows 验证二进制来自候选 `1a64c18b`。合并后源码快进至 `0f2449de`，工作区干净，LFS 对象校验通过。

### 完整主机与设备验证

后续完整验证使用 `0761b410`，相对合并提交 `0f2449de` 只有本文件的文档变更。
两台主机均复用既有构建与 Cargo 缓存，使用具备 numpy、scipy、h5py 的 Python 重新配置，纳入三项 Python 工具测试。

| 主机与构建 | 检查 | 结果 |
| --- | --- | --- |
| macOS arm64，Debug | 全量构建，`ctest --parallel 12 --repeat until-fail:3 --output-on-failure` | 87/87，每项运行三次，共 261 次执行全部通过 |
| Windows x64，canonical MSVC/Ninja Release | 全量构建，`ctest --parallel 16 --repeat until-fail:3 --output-on-failure` | 86/86，每项运行三次，共 258 次执行全部通过 |
| macOS arm64，Release | 7.1.4 文件流式渲染监听，经 `AVSampleBufferAudioRenderer` 输出到当前内建扬声器路由，检查循环、暂停、seek 与继续播放 | 短会话功能检查通过，播放头前进，`failed=0`，欠载 0 |
| Windows x64，Release | 7.1.4 文件流式渲染监听，经 `ISpatialAudioClient` 输出到当前默认端点，检查循环、暂停、seek 与继续播放 | 三次短会话功能检查通过，播放头前进，`failed=0`，欠载均为 0 |
| macOS arm64，Release | PoseBridge CLI 0.6.0 的 `simulate` 经 UDP 输入生产 C ABI，覆盖 quaternion / Euler、采样时钟、跨 ±180°、换实例、来源过滤与心跳过期 | 通过，接收 508 个姿态和 21 个遥测数据报，覆盖 4 个来源实例 |
| macOS arm64，GUI Release | 重建并启动 `.app`，运行 GUI 自检，含合成 ADM 输入的离线渲染与 FLAC 编码 | 原生库加载、P/Invoke、监听入口与文件写出检查通过 |

Windows 动态库探针最初将整个 PATH 注册为 DLL 搜索目录，混入 JDK 附带的 `MSVCP140.dll` 并触发访问违规。
限定为 canonical 构建对应的运行库目录后，同一系统空间监听探针三次通过。

### OSC 等待与停止测量

Windows x64 使用 Release 生产 C ABI，在正常桌面环境完成测量，交流电接通、电池 100%，计时前 CPU 采样约 1.12%。
完整 CTest 结束后执行此项，保留日常应用与后台服务。

| 检查 | 结果 |
| --- | --- |
| 回读 Rust `Receiver::start` 所用原始 socket 句柄的 `SO_RCVTIMEO` | 配置为 20 ms |
| 32 次无数据报时的阻塞读取 | 唤醒均值 31.628 ms，最大 32.664 ms |
| 200 次启动后停止，停止前交错等待 0–21 ms | `stop` 均值 14.219 ms，p99 32.586 ms，最大 32.722 ms |
| 每次运行中用 `SO_REUSEADDR` 尝试共享端口，停止后重新绑定 | 200 次共享尝试均被拒绝，停止后的 200 次重绑均成功 |

### 合并后的 Consistency

已核对以下主线工作流完成并通过。

| 合并点 | 运行 |
| --- | --- |
| A（`2a735365`） | [42793410919](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/42793410919) |
| B（`4b1da58d`） | [42854859745](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/42854859745) |
| B 后的维护者修正（`b87d5a3a`） | [42862032746](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/42862032746) |
| C（`0f2449de`，文档补记 `0761b410`） | [42971449429](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/42971449429) |

C 的合并提交对应运行 `42969917705` 被后续文档更新取消，由 `0761b410` 那次成功运行覆盖。
已下载并核对 config A、B 的普通与诊断四份报告，每份均为 294 个 PCM id 相同、0 个不同，三平台齐全，319 项数值门禁全部通过。

### 实机与 GUI 后续项

- GUI 当前提供 AirPods 与手动转头来源，PoseBridge OSC 输入入口尚未实现。本轮按维护者决定完成现有链路验证并记录这一缺口。
- 维护者暂不接入物理头追踪设备，实机采样、头动与听感确认暂缓。
- 维护者主机验证矩阵为 macOS arm64 与 Windows x64，保留已有 Linux 和工作流验证记录。

## 规模

C/C++（`language-surface.py`）：

| 合并点 | 生产 | 测试 | `src/adm_realtime` | `src/adm_engine` | `src/adm_render_common` | 说明 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 5.4 E（`12c823c`） | 26,363 | 49,297 | 3,314 | 1,967 | 3,379 | 5.5 基线 |
| A（`2a73536`） | 25,477 | 49,509 | 2,389 | 1,968 | 3,543 | `MonitorEngine` 只剩外壳；岛表与 FFI 声明加入 |
| B（`4b1da58`） | 25,077 | 49,911 | 2,033 | 1,807 | 3,660 | `MonitorSession` 只剩外壳；C++ `MonitorEngine` 外壳移到 `tests/support/realtime` |
| C（`0f2449de`） | 24,409 | 50,237 | 1,366 | 1,807 | 3,660 | OSC 接收器只剩外壳；三组文件移到 `tests/support` |

- **累计**：5.5 共减少生产 C/C++ 1,954 行。五期至今 32,551 → 24,409，减少 25.0%。
- **Rust**：
  - `mradm-engine` 16,275 → 20,083 行（A、B）；
  - `mradm-ffi` 22,586 → 24,375 行；
  - `mradm-osc` 809 → 1,757 行（C，含测试）。
- **岛**与 CUDA 边界不变。
