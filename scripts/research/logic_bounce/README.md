# Logic Pro 后台 ADM 并轨

已在本机 **Logic Pro Creator Studio 12.3.1 (6682)**、macOS **27.0 / 26A428**、arm64 验证完整流程：
**ADM → 无窗口临时工程 → Apple Renderer / Music → Float32 WAVE → 恢复原工程、清理临时工程**。
正常任务不点击界面、不调用键盘快捷键、不经过并轨设置或保存面板。

它仍需要正在运行的 Logic、一个已初始化的宿主工程和现有图形登录会话。
这是固定版本的私有 ABI 研究工具，不是退出 Logic 后仍可运行的独立渲染器。
需要本机已有的 Xcode clang/LLDB 和调试权限；工具不修改系统安全设置。
实现与验证见 [调查记录](../../../docs/architecture/LOGIC_HEADLESS_BOUNCE.md)。

## 从 ADM 直接输出

在仓库根目录运行；输出父目录必须存在，输出文件及同名任务目录必须不存在：

```sh
python3 scripts/research/logic_bounce/run_headless_bounce.py \
  --adm /absolute/path/logic-compatible-adm.wav \
  --output /absolute/path/apple-reference.wav
```

输入当前要求 48 kHz、1–128 路 PCM，并包含 `axml`、`chna`、`dbmd`。
每次独立创建临时工程，原生导入 ADM，并把 Atmos 配置为 Apple Renderer、Music、Generic HRTF、头追关闭。
等待音源连接和渲染器异步更新完成后，才开始并轨。输出范围按源帧数计算，不依赖整数小节范围。

固定输出：48 kHz、双声道、32 位浮点、间插 WAVE；环绕声并轨开启；
自动模式；正常化、抖动和音频尾音关闭；包含速度信息。
这里的双声道就是 Logic 当前 Apple Renderer 的结果。

每次保留 `<output-stem>.logic-bounce/`，其中有请求、响应、就绪状态、清理结果、音频检查及 `run.json`。
检查完整 RIFF、声道/格式/帧数、样本有限性、PCM SHA-256；默认把全静音输出视为失败。
只有有意导出静音时才用 `--allow-silence`。

ADM 参考流程最多并轨三遍，必须取得连续两遍完全相同的 PCM，才把选中的候选发布到输出路径。
候选保留在任务目录，最终文件用硬链接发布，不额外复制 PCM；不收敛则报告失败。
这是针对实测首遍历史状态残留加入的检查，并不代表已解决 Logic DSP 状态的底层原因。

## 已打开工程的并轨

已有工程应先配置好 Dolby Atmos / Apple Renderer / Music。此模式继承工程设置：

```sh
python3 scripts/research/logic_bounce/run_headless_bounce.py \
  --expected-document 'original-session-preserved.logicx' \
  --output /absolute/path/new-bounce.wav
```

工程显示名称必须准确匹配。省略 clock 时使用 Logic 默认项目/循环范围，可能补齐到整数小节。
例如本轮 6 秒探针的默认范围为 8 秒。已知 120 BPM、4/4 的六秒范围可使用：

```sh
python3 scripts/research/logic_bounce/run_headless_bounce.py \
  --expected-document 'logic-axes-probe.logicx' \
  --output /absolute/path/probe-bounce.wav \
  --start-clock 0x960000000000 --end-clock 0xc30000000000
```

clock 是 Logic 内部时间，不是秒数。`--prepare-only` 只准备并报告范围，不写音频；
在 ADM 模式仍会执行导入、配置和临时工程清理。

## 调度和恢复

首次调用通过 LLDB 加载本地小桥后立即分离。后续请求走仅当前用户可访问的驻留文件队列，
每一步不再暂停 Logic。桥在主 RunLoop 执行原生模型/音频操作；内部唤醒事件不包含键鼠输入。
同一 Logic 进程的客户端有互斥锁。源码哈希、进程及其启动时间用于选择驻留桥，
应用和关键框架的版本/哈希不匹配时拒绝调用。

默认等待 120 秒，可用 `--wait-timeout` 调整。超时会标记请求取消；尚未开始的请求不会稍后执行。
已经进入原生导入/并轨的任务不会被强行中断。先查看任务响应，随后可运行：

```sh
python3 scripts/research/logic_bounce/run_headless_bounce.py --recover
```

恢复仅处理本工具保留的临时工程，并等待原宿主渲染器恢复，不关闭或丢弃用户工程。
如果 Logic 本身出现错误模态框，需要先处理该错误；工具不会自动确认任意对话框。
正常完成会核对原文档列表、路径、未保存标志、活动音频工程和已保存的渲染器选项。
这不等于对工程全部内部数据进行了逐字节快照比较。

临时工程使用原生 autosave 容器，成功清理后删除；小型优化桥按源码哈希保留在
`local/logic-bounce-runtime/`。没有独立工作树、应用副本或大型构建缓存。

## 生成研究输入

本轮复用 AC4 项目已有 Release CLI：

```sh
/Users/Sakuzy/code/rust/MacinDecode-AC4-Core/target/release/macinac4 export-adm-bwf \
  /Users/Sakuzy/code/rust/MacinDecode-AC4-Core/vectors/probe_axes_single_object/encoded/master_ac4_dme_l4_768K_3dof.m4a \
  --output /absolute/path/logic-axes-probe.wav \
  --object 2:1 --compatibility logic --fps 24
```

这是合成粉红噪声/元数据探针，不是恢复母版音频。生成 RF64、五位 ADM 时钟、11 路 24-bit PCM，
dbmd 段 7/9/10 校验通过；源工具报告的 trim/headphone/elevation 映射限制仍适用。

`make_semantic_probes.py` 可从小于 64 MiB 的合成 RF64 探针创建六份 PCM/dbmd 相同的语义夹具：

```sh
python3 scripts/research/logic_bounce/make_semantic_probes.py \
  /absolute/path/logic-axes-probe.wav /absolute/path/new-probe-directory
```

它在新目录生成 control、diffuse、Cartesian divergence、size 对照，不修改源文件。
此处需要实际 ADM 夹具，因为 Logic 不读取 MacinRender 的 semantic policy；
验证 MacinRender 自身时仍优先使用 `--semantic-policy`。
