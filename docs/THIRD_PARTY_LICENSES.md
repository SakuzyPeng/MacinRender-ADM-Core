# 第三方许可证与发行边界

> 状态：当前结论  
> 日期：2026-05-23  
> 说明：本文用于工程发行决策，不构成法律意见。正式商业发行前仍建议做一次最终依赖 SBOM/notice 复核。

## 本项目许可证

MacinRender ADM Core 的项目源码采用 **MIT License**，以仓库根目录 `LICENSE` 为准。

该选择与当前默认构建依赖兼容。项目源码继续保持 MIT；二进制发行包需要附带本项目 MIT 许可证以及下表第三方 notice / license 文本。

## 默认发行策略

- 默认发行构建不得启用 GPL-only 或会引入强 copyleft 义务的可选模块。
- Release/多配置构建中，`libFLAC` 与 `libopus` 默认采用 vendored static provider；发行包仍需附带其许可证文本。
- macOS-only APAC 输出依赖系统 AudioToolbox/CoreFoundation framework；这些 Apple SDK/framework 不随本项目二进制再分发，但功能应标记为 macOS only。
- Linux 发行包以 Ubuntu 24.04 x86_64 为基线；`DEPENDENCIES.txt` 记录实际 `ldd` 清单，并在打包阶段拒绝缺失库、构建目录依赖和 `/usr/local` 依赖。
- 用户提供的 SOFA/HRTF 数据文件不属于本项目许可证覆盖范围；若发行示例数据，必须单独记录该数据集许可证。

## 当前依赖许可证清单

| 依赖 | 用途 | 许可证 | 默认发行可用性 | 备注 |
|---|---|---|---|---|
| `fmt` | 格式化与日志辅助 | MIT | 可用 | 若动态链接系统库，仍需记录版本与许可证 |
| `spdlog` | 日志库 | MIT | 可用 | 使用 external fmt |
| `CLI11` | CLI 参数解析 | BSD-3-Clause | 可用 | |
| `tl::expected` | `Result<T>` 基础类型 | CC0-1.0 | 可用 | header-only |
| `libbw64` | BW64/ADM BWF 读写 | Apache-2.0 | 可用 | |
| `libadm` | ADM XML 建模/解析 | Apache-2.0 | 可用 | 内含 RapidXML，Boost Software License 或 MIT |
| `libear` | BS.2127/EAR 增益计算 | Apache-2.0 | 可用 | 内部 vendored Eigen/xsimd/kissfft 需随 notice 记录 |
| Eigen（libear 内部） | 线性代数 | MPL-2.0 为主，含 BSD/LGPL 文件 | 需防线 | 建议编译 libear 时定义 `EIGEN_MPL2_ONLY`，避免误用 LGPL-only 模块 |
| xsimd（libear 内部） | SIMD 抽象 | BSD-3-Clause | 可用 | |
| kissfft（libear/SAF 内部） | FFT | BSD-3-Clause | 可用 | |
| `libebur128` | LUFS/True Peak 测量 | MIT | 可用 | |
| `dr_libs` (`dr_wav`, `dr_flac`) | WAV/FLAC 轻量读写 | Unlicense 或 MIT-0 可选 | 可用 | 本项目按 MIT-0/Unlicense 宽松使用 |
| `libFLAC` | FLAC 编码与 metadata | Xiph BSD-like | 可用 | 当前关闭 C++ libs/programs/docs/examples；源码包内 GPL/LGPL 文本不代表默认链接这些组件 |
| `libopus` | Opus MKA 编码 | BSD-like + IETF patent IPR notices | 可用 | 发行文档需保留专利 IPR 链接说明 |
| Spatial Audio Framework (SAF) core | VBAP/MDAP/HRTF/FFT 等 DSP | ISC + permissive third-party code | 可用 | 仅在禁用 GPL 模块的配置下 |
| SAF `saf_sofa_reader` | 用户 SOFA HRIR 读取 | ISC | 可用 | 默认可启用 |
| SAF examples `spreader` (fork) | saf_spreader 双耳扩散渲染（实验性） | ISC，版权 2021 Leo McCormack | 可用 | fork 位于 `src/adm_render_binaural/spreader_mr.*`；最小改动：增加 `spreader_init_from_hrtf_grid()` 接口、修复 SOFA Q 未赋值 bug；不引入额外依赖 |
| libmysofa（SAF SOFA reader 内部） | SOFA 解析 | BSD-3-Clause | 可用 | |
| zlib（SAF SOFA reader 内部） | 压缩支持 | zlib License | 可用 | |
| Apple AudioToolbox/CoreFoundation/Accelerate | APAC、CAF layout、macOS 加速 | Apple 平台 SDK | macOS only | 不进入跨平台发行物依赖集合 |

## 禁止或需单独审批的选项

以下选项不得进入默认发行构建：

| 选项 / 依赖 | 风险 | 当前策略 |
|---|---|---|
| `SAF_ENABLE_TRACKER_MODULE=ON` | SAF tracker 模块为 GPLv2 | 默认强制 OFF |
| `SAF_ENABLE_HADES_MODULE=ON` | SAF HADES 模块为 GPLv2 | 默认强制 OFF |
| `SAF_USE_FFTW=ON` | FFTW 为 GPL/商业双许可，默认分发会复杂化 | 默认强制 OFF |
| `SAF_ENABLE_NETCDF=ON` | 依赖链和大文件 reader 发行审计复杂 | 默认强制 OFF；SOFA 走 libmysofa/zlib |
| `SAF_USE_INTEL_IPP=ON` | 专有 SDK/redistribution 条款需单独审计 | 默认强制 OFF |
| FFmpeg/libavformat provider | LGPL/GPL 配置差异大 | 不进入默认核心；若未来实现需独立 ADR |

## 发行包 notice 要求

发布二进制时至少包含：

1. `LICENSE`：本项目 MIT 许可证。
2. `THIRD_PARTY_NOTICES.md` 或 `licenses/` 目录：包含上表所有实际链接/打包依赖的许可证文本。
3. 构建配置摘要：记录 `MR_ADM_FLAC_PROVIDER`、`MR_ADM_OPUS_PROVIDER`、`MR_ADM_ENABLE_SOFA`、SAF 可选模块开关，以及是否启用 APAC。
4. 若包含示例 SOFA/HRTF/ADM 音频素材，素材许可证必须与代码许可证分开声明。

当前首版 release package 会把本文复制为 `THIRD_PARTY_NOTICES.md`。它是工程审计用 notice 摘要；
正式公开发行前仍建议拆分为完整逐项许可证文本 bundle。

## 推荐后续防线

- 在 CMake 中对 GPL 风险 SAF 开关做显式 fatal guard，防止本地 cache 覆盖。
- 给 libear 编译定义 `EIGEN_MPL2_ONLY`，将 Eigen LGPL-only 代码路径变为构建期错误。
- 增加 release checklist：生成 `otool -L` / `ldd` 依赖清单，并与本文件逐项核对。
