# AVPlayer Atmos 调用链探针

自建、优化编译的 AVPlayer。`none` 保持无 audio mix/tap 的基线，`pre` / `post` 安装只复制音频的
`MTAudioProcessingTap`。全部通过正常实时播放驱动，不使用离线 reader 渲染。
播放器默认只静音自己，不改默认设备。设置 `ATMOS_PROBE_AUDIBLE=1` 时正常播放该进程。
定量采集必须使用正常播放：本机静音播放约 3.45 秒后会在 tap 中持续返回零。
输入文件和已有输出目录不会被覆盖。

当前实验与结论见 `docs/architecture/ATMOS_AVPLAYER_PLAYBACK_CHAIN.md`。
不加入产品构建或 CTest；输出放入本地研究目录，最多播放 8 秒。

## 编译和采集

在仓库根目录执行，选择一个尚不存在的运行目录；`media` 指向本地、非加密的单音轨 E-AC-3 JOC 文件。

```sh
run_dir="$PWD/local/atmos-playback-new"
media='/absolute/path/to/Atmos.m4a'
mkdir -p "$run_dir/bin"
xcrun clang++ -std=c++20 -O2 -g -fobjc-arc -Wall -Wextra -Werror \
  -Wno-deprecated-declarations scripts/research/atmos_playback/player_probe.mm \
  -framework AVFoundation -framework Foundation -framework MediaToolbox \
  -framework AudioToolbox -framework CoreAudio -framework CoreMedia \
  -o "$run_dir/bin/player_probe"
for mode in none pre post; do
  "$run_dir/bin/player_probe" "$media" "$run_dir/$mode" "$mode" 57 6 \
    > "$run_dir/$mode.log" 2>&1
done
for name in audible-pre audible-pre-repeat; do
  ATMOS_PROBE_AUDIBLE=1 "$run_dir/bin/player_probe" \
    "$media" "$run_dir/$name" pre 56 7 > "$run_dir/$name.log" 2>&1
done
```

参数依次是媒体、输出目录、tap 模式、起点秒数、时长秒数。
`tap.f32` 是交错的原始 float32 副本，原始声道顺序不变；`tap.json` 包含实际 ASBD 和逐块时间戳。
声道数由系统选择，不能仅凭 12ch 推断具体布局，必须配合属性 trace 验证。

## 只读调用链观察

默认记录公开 AudioCodec / AudioConverter / AudioUnit API 参数和调用栈。
LLDB 停顿会影响实时调度，因此这些运行的 PCM 不作为定量参考。

```sh
for mode in none pre; do
  xcrun lldb --batch \
    -o 'settings set target.disable-aslr false' \
    -o "command script import $PWD/scripts/research/atmos_playback/trace_chain.py" \
    -o run -o "atmos-save-symbols $run_dir/symbols-$mode.json" \
    -- "$run_dir/bin/player_probe" "$media" "$run_dir/trace-$mode" "$mode" 57 3 \
    > "$run_dir/trace-$mode.log" 2>&1
done
```

`ATMOS_TRACE_PATTERNS` 可传一个 JSON 正则表达式数组，替换默认断点；按本机导出的符号选择。
本次 `dolby-v2-*.log` 使用以下模式：

```json
[
  "^ACDDPAtmosDecoder::(Initialize|SetCurrentOutputFormat|ProduceOutputBufferList|CopyUDCOutputToABL|UpdateChannelMappingMatrix)\\(",
  "^ddp_udc_int_jocd_process_one_frame$",
  "^oamd_framer_get_metadata$",
  "^omg_panner_(init|process|program_metadata_set)$",
  "^point_panner_(get_gains|compute_gains.*)$",
  "^size_panner_(get_gains|normalise_pseudo)$",
  "^VBAP::calculateVBAPGains\\(",
  "^AudioDSP::Core::VBAP::calculateVBAPGains\\(",
  "^AUSpatialMixer.*::(Render|Process)\\(",
  "^oar_process_safe$",
  "^obj_render_process$",
  "^omg_process$"
]
```

私有函数只按名字定位，不使用固定结构偏移；返回断点只读取，不修改返回值。
每个断点位置单独限流，避免一个函数达到限额后把同一正则匹配的其他函数一并禁用。
函数存在或断点未命中都不是该路径实际执行／不存在的充分证据，以实际栈和属性为准。

## 本次固定片段汇总

`summarize.py RUN_DIRECTORY` 专用于本次 57–62 秒 / 48 kHz / 7.1.4 实验，依赖 NumPy。
输入需要 `pre`、`post`、`audible-pre`、`audible-pre-repeat` 及播放器日志，
还有 `trace-{none,pre}.log` 和 `dolby-v2-{none,pre}.log`。
它校验格式和时间覆盖，比较两组 tap，调用 AudioFile API 封装实时 PCM 为带正确标签的 CAF，
并生成 `summary-final.json`；遇到已有结果拒绝覆盖。定量参考只使用正常播放的前置 tap，
从 56–63 秒的播放中提取 57–62 秒，避开起播和停止边缘；要求两次结果逐样本一致。
封装没有再次渲染音频。

## 原生 Atmos 指定布局与离线探针

`offline_probe.cpp` 使用 AudioFile + AudioConverter，不创建播放器或音频设备。
详见 `docs/architecture/ATMOS_NATIVE_OFFLINE.md`。
需要显式启用本进程的私有工厂注册；默认不隐式更改组件可用性。

```sh
xcrun clang++ -std=c++20 -O2 -g -Wall -Wextra -Werror \
  -Wno-deprecated-declarations scripts/research/atmos_playback/offline_probe.cpp \
  -framework AudioToolbox -framework CoreFoundation -o "$run_dir/bin/offline_probe"
ATMOS_REGISTER_JOC=1 ATMOS_CODEC_SELECTOR=joc \
  "$run_dir/bin/offline_probe" "$media" "$run_dir/offline-714.caf" \
  0x00c0000c joc 55 64 > "$run_dir/offline-714.log" 2>&1
```

参数依次为媒体、输出 CAF（`-` 表示只统计）、CoreAudio 布局 tag、`base|joc`、
保留区间的起止秒数。保留区间按未裁 priming 的解码器时间轴计算；总是从文件开头顺序解码。
探针不会覆盖已有文件。当前 `joc` 模式默认只开放 7.1、5.1.2、5.1.4、7.1.2、7.1.4、9.1.6。

`ATMOS_REGISTER_JOC=1` 复现实际观察到的 AVPlayer 进程内注册，工厂来自系统 AudioCodecs 的
`ACAC3DecoderNewFactory` 导出，属于私有实现；没有修改系统文件或其他进程。
`ATMOS_CODEC_SELECTOR=joc` 使用与 AVPlayer 相同的指定组件创建方式；`base` 选择普通 E-AC-3 组件用于诊断。
不设 selector 时使用普通 `AudioConverterNew`。
`ATMOS_ALLOW_UNVERIFIED_LAYOUT=1` 可复现其他布局的边界行为，但成功返回可能伴随丢声道或全静音。

`trace_layouts.py` 可在 LLDB 中只读观察本机 `ACDDPAtmosDecoder` 的布局设置／映射调用，
并保存相关函数的反汇编；通过 `ATMOS_LAYOUT_TRACE_DIR` 指定一个已创建的本地目录。
它不改进程数据、返回值或系统组件。实际原生解码格式仍可用 `trace_chain.py` 的
`AudioCodecInitialize` / `SetCurrentOutputFormat` 断点记录，避免把 AudioConverter 外层格式当成原生输出格式。

## OAR 22.2 几何边界

`trace_oar.py` 记录实际原生解码的 OAR 配置；`oar_layout_probe.cpp` 直接查询扬声器集合、位置和 OAR 内存，
并初始化一个额外的 24 声道组合。它只作配置容量控制，该组合不是 22.2，也不输出音频。
二者对系统组件哈希做版本保护。

```sh
xcrun clang++ -std=c++20 -O2 -g -Wall -Wextra -Werror \
  -Wno-deprecated-declarations scripts/research/atmos_playback/oar_layout_probe.cpp \
  -framework AudioToolbox -o "$run_dir/bin/oar_layout_probe"
"$run_dir/bin/oar_layout_probe" > "$run_dir/native-layout-survey.jsonl"
```

`ida_oar_tables.py` 可在本地 IDA 数据库中导出位置表，`ATMOS_IDA_TABLE_OUTPUT` 指定输出目录。
结论与边界见 `docs/architecture/ATMOS_NATIVE_222_FEASIBILITY.md`。
