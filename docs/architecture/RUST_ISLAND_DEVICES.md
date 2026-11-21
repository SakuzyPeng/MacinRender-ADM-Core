# 七期 7.3：输出设备

> 状态：Linux x64 完成（2026-11-21），macOS arm64 与 Windows x64 的原生复核待维护者，见末节。
> 切片与用户决定见[审计七期一节](PCM_CONSISTENCY_AUDIT.md#七期平台岛收缩)，清单见 [7.1 记录](RUST_ISLAND_BASELINE.md)。

7.3 之前，监听会话与 Scene 输出经 C++ 设备岛 `mradm_device_islands` 打开设备。它把三种设备包成 `IAudioOutputDevice`，再转成 `MradmDeviceTable` 交给 Rust：
- miniaudio 的硬件设备；
- miniaudio 的 null 后端；
- 平台的系统空间 sink（macOS `AVSampleBufferAudioRenderer`，Windows `ISpatialAudioClient`）。

7.3 起由 Rust 打开和驱动这些设备：
- miniaudio 仍是 C 库（用户决定 2026-11-19：暂不替换），Rust 经窄 C 接口直接驱动；
- 两个 sink 直接给出 `MradmDeviceTable`；
- C++ 设备岛、`miniaudio_device.cpp` 与 `audio_output_device.h` 删除。

## 结构

| 部分 | 位置 | 内容 |
| --- | --- | --- |
| miniaudio 的窄 C 接口 | `src/adm_realtime/miniaudio_island.{h,c}`（目标 `mr_adm_miniaudio`） | miniaudio 实现所在的唯一编译单元；`MradmMiniaudioIsland` 只做五件事：打开不启动、读设备宽度与原生格式的声道数、启动、关闭、枚举。设备 id 以原始字节传出 |
| Rust 设备 | `mradm-ffi` `render::miniaudio` | id 的十六进制 token、多声道设备检查、拉取回调与静音补齐、重复启动的拒绝、错误消息；设备是一张 `DeviceTable` |
| 设备表 | `mradm-ffi` `render::device_island` 的 `DEVICES`，门面 `mradm_ffi::api::islands::devices()` | 由 Rust 组装：按 kind 打开 miniaudio 或系统空间 sink，枚举走 miniaudio；`mradm-capi` 与房间设备探针都用它 |
| 系统空间 sink | `src/adm_realtime/system_spatial_island.{h,cpp}`（仅 macOS / Windows 编译） | `MradmSystemSpatialIsland` 的 `open`；头里的 `device_table<Sink>` 模板按 sink 有哪些方法填表，没有的方法留空，Rust 视为无操作 |

miniaudio 的配置与设备结构体很大，且随平台变化，因此不在 Rust 里手写它们的布局。C 接口只传标量、原始 id 字节和一个定长记录 `MradmMiniaudioFormats`。这个记录与三个岛记录（`MradmMiniaudioIsland`、`MradmMiniaudioFormats`、`MradmSystemSpatialIsland`）都由 `check-ffi-headers.py` 与 Rust 逐字段核对。`MradmDeviceIslands` 与 `MradmDeviceEnumeration` 只在 Rust 内部使用，已从 C 头删除。

miniaudio 改按 C 编译，原来是在 C++ 编译单元里编译。房间设备探针的采集垫片 `tests/manual/capture_tap.c` 改用 `mr_adm_miniaudio` 的实现。

## 保留的行为

| 行为 | 7.3 之后 |
| --- | --- |
| 设备 token | 枚举仍输出 `ma_device_id` 的小写十六进制。只有长度恰为 `2 × sizeof(ma_device_id)` 的小写十六进制 token 才指定设备；空 token、大写、奇数长度与长度不符一律打开默认设备 |
| 多声道检查 | 超过两声道时，设备内部声道数 ≤ 2，或原生格式全部为 1–2 声道，就关闭设备并返回 `unsupported`，消息不变。原生格式声道数为 0（任意）的不算立体声 |
| WASAPI / ALSA | 超过两声道时仍设 `noAutoConvertSRC` / `noAutoChannels` |
| 拉取 | 音频线程上调用引擎的 pull，不足的帧补零；pull 返回值不变 |
| 错误码与消息 | `audio device already started`（1）、`miniaudio null context init failed` / `device init failed` / `device start failed`（6），Linux 上系统空间输出仍为 `System spatial output is unavailable on this platform`（2）|
| sink 的异常 | sink 的 `start` 与工厂抛出时仍返回 100 与异常消息；控制调用抛出仍结束进程 |
| 停止与重启 | `stop` 关闭设备并等回调返回；停止后可再次 `start`；销毁前先停止 |

删除的只有内部接口：`IAudioOutputDevice`、`enumerate_output_devices`、`make_miniaudio_device` 和没有调用者的 `actual_sample_rate`。公开 C ABI 仍导出 164 个函数，共享库里没有 `ma_*` 符号外泄。

## 验证（Linux x64）

参考实现为 main `b4d31ee`。参考与候选用同一 Debug 配置分别构建（vendored FLAC / Opus）。

- **CTest**：22 项全部通过，其中 C 消费者测试的 Scene 输出走 null 后端 12 声道，因此经过多声道检查与 `MradmMiniaudioFormats`。
- **C ABI 对照**：两个临时 C 程序分别链接参考与候选的 `libmradm_capi`，输出逐字节相同（stderr 中 ALSA / JACK 的诊断也相同）：
  - 设备枚举 JSON；
  - Scene 输出 null / 系统空间 / 立体声硬件，token 取空、`""`、`zz`、短十六进制和枚举出的 null 设备 token；
  - 7.1.4、5.1 与双耳监听的创建和播放，默认设备与无效 token 各一次。
- **新的 Rust 测试**（`mradm-ffi` `render::miniaudio`）：token 往返与拒绝规则、短拉取补零、多声道检查的判定表、null 后端的实时拉取与重复启动拒绝和停止后重启。
- **故障注入**：去掉补零、把多声道检查的「全部」改成「任一」、接受大写十六进制，三处都被测试抓到。C 头多一个字段时 `check-ffi-headers.py` 报出字段数不一致；开发中这个错误曾让 C 消费者测试段错误。
- **sink 模板**：在 Linux 上用两个仿照 Apple / Windows sink 方法集的类实例化 `device_table`，在项目的警告选项与 ASan / UBSan 下运行：Apple 型的每个控制都接上，Windows 型缺的控制留空，错误与异常转成 2 / 100 与消息；`system_spatial_island.cpp` 的 Windows 分支用 `-D_WIN32` 做语法检查。
- **质量**：`mr_adm_rust_quality`（rustfmt、clippy `-D warnings`）、`mr_adm_ffi_header_check`、`check-capi-exports.py`、cppcheck（照 `cppcheck.sh` 的参数）与新文件的 clang-tidy 通过。

## 规模

| 区域 | 7.3 之前 | 7.3 之后 |
| --- | ---: | ---: |
| 生产 C/C++ | 5,248 | 4,888 |
| 岛（`adm_apple`、`adm_windows`） | 3,021 | 3,019 |
| 岛接口 | 1,009 | 1,220 |
| 合计 | 9,278 | 9,127 |

岛接口删去设备岛的 183 行，新登记 394 行：miniaudio 接口 183 行（`miniaudio_island.c` 137、`.h` 46），sink 接口 211 行（`system_spatial_island.h` 175、`.cpp` 36）。被删的 `miniaudio_device.cpp`（215 行）与 `audio_output_device.h`（135 行）原属生产区域。

## 发布说明

- 无公开行为变化：C ABI、CLI 与设备枚举的 token 都不变。
- miniaudio 改按 C 编译；构建多一个静态库 `mr_adm_miniaudio`，链接清单把它放在采集垫片之后、`mr_adm_islands` 之前。

## 维护者复核

Linux 上的 sink 代码没有编译过，需要在原生平台上确认：

1. macOS arm64：Release 构建与 CTest；用 `mr_monitor_spatial_check`（`tests/manual/system_spatial_monitor_check.cpp`）或 GUI 检查 AVSampleBufferAudioRenderer 监听（7.1.4 播放、暂停与继续、seek、音量、头追踪）；7.1.4 硬件设备与立体声耳机设备各开一次监听，后者应得到多声道检查的 `unsupported`。
2. Windows x64（`build\win-canon`）：Release 构建与 CTest；ISpatialAudioClient 监听，以及未启用空间音效时的 `unsupported`。
3. 两个平台的 CLI 记录与 7.2 一样做一次 134 条命令对照（7.3 不涉及编码，预期 0 差异）。
