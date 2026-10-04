# Rust ebur128 适配性评估

仅供评估的独立小工具，不加入生产 Cargo workspace、CMake 默认构建或发行依赖。
调用同仓库已经构建的 **Release libebur128 1.2.6**，逐次比较 Rust 0.1.10 和 C 的结果。
两者读取相同的 float32 样本。Cargo 编译产物复用项目现有目录，不建立新工作区或依赖副本。

从仓库根目录执行（参考库目录按实际构建位置填写）：

```sh
python3 tests/tools/ebur128_eval/run.py \
  --reference-lib-dir .fc-cache/libebur128-build
```

若参考库在 `build/release/_deps/libebur128-build`，改用该路径；必须确认其为 Release。
Windows 指向包含 `ebur128.lib` 的原有 Release 目录。`--cargo` 可选择既有 SDK 的 Cargo，
`--target-dir` 可覆盖共享产物目录，尤其适用于路径大小写不同的 Windows checkout。
运行器在仓库 `rust/` 下启动 Cargo，沿用仓库固定工具链，不安装额外工具链。

运行器依次执行 Debug correctness、Release numerical verification 和 Release benchmark，
将 JSONL 明细与汇总写入忽略的 `out/ebur128-eval-*`。已有完整结果可用 `--reuse-results`
只重新汇总。数值报告和性能结论使用 Release 数据。

覆盖范围：

- 49 组 C/Rust 对照：44.1/48/88.2/96/176.4/192 kHz，1–64 声道，实际模式组合，
  静音、低电平门限、噪声、脉冲、跨采样峰值、400 ms / 3 s 窗口边界、HOA 7.1.4 和 LFE。
- 独立 1 kHz / -23 LUFS 电平检查，跨采样峰值检查，分块、重置和错误输入检查。
- 600 秒音频的尽速模拟监听；分配计数器只记录 Rust 分配，不拦截 C malloc，不等同于 RSS。
- 本项目未使用的 `set_max_history()` 独立探针。该探针如实报告库的限制，
  不把它混入现用接口的通过数量；0.1.10 的动态缩短历史缺陷记录于评估文档。
- 5 秒 PCM 的五轮交替性能测量，处理和查询计时，创建/销毁与数据生成不计入。

对照阈值是 0.001 LU / 0.01 dBTP，不要求位相等。低于 -300 LUFS 的滤波残留差异单列，
不纳入有效电平误差最大值；该阈值远低于 R128 的 -70 LUFS 绝对门限。
初始默认声道映射与生产调用相同；额外覆盖显式 HOA 7.1.4 映射。
默认映射不会自动识别任意多声道扬声器布局。

本轮未下载或重跑完整 EBU 官方音频测试集，不把这些检查称为 EBU 合规认证。
`precision-true-peak` 可作为后续实验选项，本轮结果使用关闭该 feature 的默认实现。

见 [评估结论](../../../docs/architecture/RUST_EBUR128_EVALUATION.md)。
