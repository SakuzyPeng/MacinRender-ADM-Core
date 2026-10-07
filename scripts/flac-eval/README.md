# FLAC 解码器选型实验

独立于生产 Cargo workspace/CMake 的探测工具。结论与迁移门槛见
[替换准备方案](../../docs/architecture/RUST_FLAC_DECODER_PLAN.md)。

本机 macOS/Linux 复现需要 Rust 1.98.0、Python 3、C++20 编译器、FLAC CLI，
以及现有 Release 构建中的 vendored `libFLAC.a`。默认源目录是仓库的 `.fc-cache`；
不同布局可用 `--dr-libs`、`--flac-source`、`--build-dir` 指定。

```sh
python3 scripts/flac-eval/run.py
```

报告默认写入 `local/flac-eval/selection.json`。Cargo 使用生产 Corrosion 路径对应的共享 target
目录；不创建另一套 CMake 缓存。运行时夹具和 C++ 探针放入报告目录下的临时目录，完成后删除。
Windows/MSVC 版 runner 尚未实现，不能以此工具代替规范 Windows 产品验证。

`reference.cpp` 用 libFLAC 写夹具，用当前锁定 dr_flac 读取；Rust 探针分别读取 Symphonia/Claxon。
整数样本由 Python 独立生成，正确答案为左对齐 i32 及 `sample / 2^(bits-1)` 的 f32 位模式。
解码器输出每样本 8 字节：LE i32 + LE f32，用后删除，仅保存摘要。

这是行为探测，不是通过/失败测试门禁：候选解码器失败或输出错误也会写入报告，正常结束 runner
只表示探测完成。`pcm_exact` 必须和 `status`、退出码、帧数、gaps、MD5 一起阅读；期望输出为空
而 seek 报错时也可能 `pcm_exact=true`。不要把该字段单独当成迁移通过。

实验锁定 Symphonia 0.6.1（MPL-2.0，`flac,ogg`，无默认 features）和 Claxon 0.4.3（Apache-2.0）；
它们不参与生产构建或发布。实验源码随本项目采用 MIT；依赖仍采用各自许可证。
