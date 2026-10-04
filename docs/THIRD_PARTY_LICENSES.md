# 第三方许可证与发行边界

> 状态：当前结论  
> 日期：2026-05-23  
> 说明：本文用于工程发行决策，不构成法律意见。正式商业发行前仍建议做一次最终依赖 SBOM/notice 复核。

## 本项目许可证

麦渲峰 ADM Core（MacinRender ADM Core）的项目源码采用 **MIT License**，以仓库根目录 `LICENSE` 为准。

该选择与当前默认构建依赖兼容。项目源码继续保持 MIT；二进制发行包需要附带本项目 MIT 许可证以及下表第三方 notice / license 文本。

## 默认发行策略

- 默认发行构建不得启用 GPL-only 或会引入强 copyleft 义务的可选模块。
- Release/多配置构建中，`libFLAC` 与 `libopus` 默认采用 vendored static provider；发行包仍需附带其许可证文本。
- IAMF 编码通过预构建的官方 AOM iamf-tools bridge 接入；启用 `MR_ADM_ENABLE_IAMF=ON` 的发行包必须附带 bridge 及其依赖许可证。
- macOS-only APAC 输出依赖系统 AudioToolbox/CoreFoundation framework；这些 Apple SDK/framework 不随本项目二进制再分发，但功能应标记为 macOS only。
- Linux CLI 发行包采用 AppImage/standalone 形式；非核心运行时库应打包进 AppImage，`DEPENDENCIES.txt` 记录 `ldd` 清单和 AppDir 内 bundled libraries，并在打包阶段拒绝缺失库、构建目录依赖和 `/usr/local` 依赖。宿主内核、动态加载器、glibc 基线和其他核心平台设施仍由系统提供。
- 用户提供的 SOFA/HRTF 数据文件不属于本项目许可证覆盖范围；若发行示例数据，必须单独记录该数据集许可证。

## 当前依赖许可证清单

> 下表与机器可读 `third_party/sbom.cyclonedx.json`、license 原文 bundle `third_party/licenses/`
> 同源于唯一事实源 `third_party/manifest.json`，由 `scripts/licenses/generate.py` 生成，
> 由 `scripts/quality/check-licenses.sh` 在 CI 强制三者一致。新增/升级依赖只改 manifest。

<!-- BEGIN GENERATED DEPS -->
<!-- 本块由 scripts/licenses/generate.py 从 third_party/manifest.json 生成，请勿手改。 -->

| 依赖 | 用途 | 许可证 (SPDX) | 默认发行可用性 | 备注 |
|---|---|---|---|---|
| `CLI11` @ v2.5.0 | CLI 参数解析 | BSD-3-Clause | 可用 |  |
| `nlohmann_json` @ v3.12.0 | JSON 解析/序列化（语义策略、SBOM 内部） | MIT | 可用 |  |
| `fmt` @ 11.2.0 | 格式化与日志辅助 | MIT | 可用 | 若动态链接系统库，仍需记录版本与许可证 |
| `spdlog` @ v1.15.3 | 日志库 | MIT | 可用 | 使用 external fmt |
| `tl-expected` @ v1.1.0 | Result<T> 基础类型 | CC0-1.0 | 可用 | header-only；FetchContent 目标目录名为 expected-src |
| `libebur128` @ v1.2.6 | LUFS/True Peak 测量 | MIT | 可用 |  |
| `dr_libs` @ 47a4f08e777faddf59a8955c4ea84f69f41020d5 | WAV/FLAC 轻量读写（dr_wav / dr_flac） | Unlicense OR MIT-0 | 可用 | header-only；本项目按 MIT-0/Unlicense 宽松使用 |
| `miniaudio` @ 0.11.21 | 实时音频设备输出（监听引擎） | Unlicense OR MIT-0 | 可用 | header-only；实现仅在 src/adm_realtime/miniaudio_device.cpp；Linux dlopen ALSA/PulseAudio，macOS 链接 CoreAudio/AudioToolbox |
| `libsamplerate` @ 0.2.2 | 实时 Scene 空间渲染后的输出采样率转换 | BSD-2-Clause | 可用 | 固定 SRC_SINC_MEDIUM_QUALITY；仅作为 PRIVATE 实现依赖 |
| `libFLAC` @ 1.5.0 | FLAC 编码与 metadata | BSD-3-Clause | 可用 | 默认仅链接 libFLAC（Xiph BSD-like）；源码包内 COPYING.GPL/LGPL/FDL 覆盖未链接的 C++ libs/programs/docs，默认发行不涉及 |
| `libbw64` @ 0.10.0 | BW64/ADM BWF 读写 | Apache-2.0 | 可用 |  |
| `libadm` @ 0.14.0 | ADM XML 建模/解析 | Apache-2.0 | 可用 | 内含 RapidXML（Boost Software License 或 MIT） |
| `libear` @ 2db69f8fcea0bc5db8a78e14a9c2ae6ed4283c15 | BS.2127/EAR 增益计算 | Apache-2.0 | 可用 | 内嵌 vendored Eigen/xsimd/kissfft，单列于下 |
| `Spatial_Audio_Framework` @ v1.3.4 | 历史 DSP 数值参考（测试专用） | ISC | opt-in（默认关闭） | 仅用于显式启用的历史 SAF 参考测试；生产渲染和 SOFA 读取已由 Rust 实现。 |
| `libopus` @ v1.5.2 | Opus MKA 编码 | BSD-3-Clause | 可用 | 发行文档需保留专利 IPR 链接说明（LICENSE_PLEASE_READ.txt） |
| `Eigen` @ bundled-in-libear@2db69f8f | 线性代数（libear 内嵌子模块） | MPL-2.0 | 可用 | libear 以 EIGEN_MPL2_ONLY 编译，避免 LGPL-only 模块；源码树含 COPYING.GPL/LGPL/BSD 等其他文件不代表默认使用 |
| `xsimd` @ bundled-in-libear@2db69f8f | SIMD 抽象（libear 内嵌子模块） | BSD-3-Clause | 可用 |  |
| `kissfft` @ bundled-in-libear@2db69f8f | FFT（libear/SAF 内嵌子模块） | BSD-3-Clause | 可用 |  |
| `libmysofa` @ bundled-in-SAF@v1.3.4 | SOFA 解析（SAF saf_sofa_reader 内嵌） | BSD-3-Clause | opt-in（默认关闭） | 仅用于显式启用的历史 SAF 参考测试；生产渲染和 SOFA 读取已由 Rust 实现。 |
| `zlib` @ bundled-in-SAF@v1.3.4 | 压缩支持（SAF SOFA reader 内嵌） | Zlib | opt-in（默认关闭） | 仅用于显式启用的历史 SAF 参考测试；生产渲染和 SOFA 读取已由 Rust 实现。 |
| `spreader_mr` @ fork-of-SAF-examples@v1.3.4 | saf_spreader 双耳扩散渲染（实验性 fork） | ISC | opt-in（默认关闭） | 仅用于显式启用的历史 SAF 参考测试；生产渲染和 SOFA 读取已由 Rust 实现。 |
| `aom-iamf-tools-bridge` @ external-sdk | IAMF raw OBU 编码（opt-in，MR_ADM_ENABLE_IAMF） | BSD-3-Clause-Clear AND AOM-PL-1.0 | opt-in（默认关闭） | 预构建 SDK 形式接入；启用时发行包须随实际 SDK 记录其内部依赖许可证。bundle NOTICE.txt 为手写声明，实际包须补全文本 |
| `apple-system-frameworks` @ platform-sdk | APAC/CAF layout/Accelerate（AudioToolbox/CoreFoundation/Accelerate） | LicenseRef-Apple-SDK | 平台 SDK（不再分发） | macOS-only；Apple 平台 framework 不随本项目二进制再分发。bundle NOTICE.txt 为手写声明 |
| `Corrosion` @ v0.6.1 | CMake/Cargo 构建集成 | MIT | 可用 | 仅构建时使用。 |
| `saf-derived-rust-dsp` @ v1.3.4-subset | Rust afSTFT、去相关/OM 算法及 KEMAR/滤波器/HOA 数据来源 | ISC AND MIT | 可用 | 保留原作者及许可证；不链接 SAF。资源哈希由 check_cargo.py 校验。 |
| `rust-adler2-2.0.1` @ 2.0.1 | Rust DSP 的锁定依赖 | 0BSD OR MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-approx-0.5.1` @ 0.5.1 | Rust DSP 的锁定依赖 | Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-arrayvec-0.7.8` @ 0.7.8 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-autocfg-1.5.1` @ 1.5.1 | Rust DSP 的锁定依赖 | Apache-2.0 OR MIT | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-bitflags-2.13.2` @ 2.13.2 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-bytemuck-1.25.2` @ 1.25.2 | Rust DSP 的锁定依赖 | Zlib OR Apache-2.0 OR MIT | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-glam-0.30.10` @ 0.30.10 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-glam-0.31.1` @ 0.31.1 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-glam-0.32.1` @ 0.32.1 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-glam-0.33.12` @ 0.33.12 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-log-0.4.34` @ 0.4.34 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-matrixmultiply-0.3.11` @ 0.3.11 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-memchr-2.8.3` @ 2.8.3 | Rust DSP 的锁定依赖 | Unlicense OR MIT | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-miniz_oxide-0.8.9` @ 0.8.9 | Rust DSP 的锁定依赖 | MIT OR Zlib OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-nalgebra-0.35.0` @ 0.35.0 | Rust DSP 的锁定依赖 | Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-num-bigint-0.4.8` @ 0.4.8 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-num-complex-0.4.6` @ 0.4.6 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-num-integer-0.1.47` @ 0.1.47 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-num-rational-0.4.2` @ 0.4.2 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-num-traits-0.2.19` @ 0.2.19 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-primal-check-0.3.4` @ 0.3.4 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-proc-macro2-1.0.107` @ 1.0.107 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-quote-1.0.47` @ 1.0.47 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-rawpointer-0.2.1` @ 0.2.1 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-realfft-3.5.0` @ 3.5.0 | Rust DSP 的锁定依赖 | MIT | 可用 | 上游发布包在 README 声明 MIT；随包保留声明、作者归属和标准 MIT 许可文本。 |
| `rust-rustfft-6.4.1` @ 6.4.1 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-safe_arch-1.2.0` @ 1.2.0 | Rust DSP 的锁定依赖 | Zlib OR Apache-2.0 OR MIT | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-simba-0.10.2` @ 0.10.2 | Rust DSP 的锁定依赖 | Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-sofar-0.3.0` @ 0.3.0 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 0.3.0 最小本地补丁公开 RawHrtf；关闭 DSP/重采样 features；保留上游 NOTICE。 |
| `rust-strength_reduce-0.2.4` @ 0.2.4 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-syn-3.0.6` @ 3.0.6 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-thiserror-2.0.21` @ 2.0.21 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-thiserror-impl-2.0.21` @ 2.0.21 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-transpose-0.2.3` @ 0.2.3 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-typenum-1.20.1` @ 1.20.1 | Rust DSP 的锁定依赖 | MIT OR Apache-2.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-unicode-ident-1.0.26` @ 1.0.26 | Rust DSP 的锁定依赖 | (MIT OR Apache-2.0) AND Unicode-3.0 | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-wide-1.7.1` @ 1.7.1 | Rust DSP 的锁定依赖 | Zlib OR Apache-2.0 OR MIT | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |
| `rust-winnow-0.7.15` @ 0.7.15 | Rust DSP 的锁定依赖 | MIT | 可用 | 包含各平台、feature 及构建工具的锁定依赖；实际编入范围由 Cargo 决定。 |

<!-- END GENERATED DEPS -->

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

所有 `scripts/release/package*` 打包脚本均自动随包写入以下内容：

1. `LICENSE`：本项目 MIT 许可证。
2. `THIRD_PARTY_NOTICES.md`：本文（含上方生成的依赖摘要表）。
3. `licenses/`：逐项许可证**原文** bundle（`third_party/licenses/` 复制而来，含 `INDEX.md`）。
4. `sbom.cyclonedx.json`：机器可读 CycloneDX SBOM（`third_party/sbom.cyclonedx.json` 复制而来）。
5. `BUILD_INFO.txt`：构建配置摘要（version/commit/平台/cmake_options）。
6. `DEPENDENCIES.txt`：运行时动态库清单（`otool -L` / `ldd` / `dumpbin`）。

macOS GUI 发行包还会把 `LICENSE`、`THIRD_PARTY_NOTICES.md`、`licenses/` 与 SBOM 再复制一份到
`.app/Contents/Resources/Legal/`，避免用户只移动 `.app` 时丢失许可文本。

若包含示例 SOFA/HRTF/ADM 音频素材，素材许可证必须与代码许可证分开声明。

> 维护方式：依赖清单的唯一事实源是 `third_party/manifest.json`。新增/升级依赖后运行
> `python3 scripts/licenses/generate.py` 重生成 SBOM 与本文依赖表，并用
> `python3 scripts/licenses/sync_license_texts.py --refresh --build-dir <全量构建>` 刷新原文 bundle。
> CI 由 `scripts/quality/check-licenses.sh` 强制三者一致，不再依赖人工核对。

## 推荐后续防线

- 在 CMake 中对 GPL 风险 SAF 开关做显式 fatal guard，防止本地 cache 覆盖。
- 给 libear 编译定义 `EIGEN_MPL2_ONLY`，将 Eigen LGPL-only 代码路径变为构建期错误。
- ~~增加 release checklist：生成 `otool -L` / `ldd` 依赖清单并逐项核对~~ —— 已由
  `scripts/quality/check-licenses.sh` 的 manifest↔CMake 一致性校验 + bundle 漂移校验自动化替代。
