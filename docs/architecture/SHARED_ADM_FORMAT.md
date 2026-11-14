# 通用 ADM/BWF 读写库接入

本仓库通过公开的 [MacinFormat-ADM-Core](https://github.com/SakuzyPeng/MacinFormat-ADM-Core)
复用 Rust 文件格式实现。
读取、投影和语义回写保持既有模型与数值行为，不重新设计 Snapshot/records。

- macin-adm 接管原 mradm-adm 的 Document、Snapshot、Patch、生成器及 common definitions 资源。
  消费方使用 Cargo 包别名 mradm-adm，保持 Rust 调用路径；原实现目录移除。
- macin-bwf 接管 RIFF/RF64/BW64、PCM、CHNA 与容器编辑。mradm-wav 转为 Rust 重导出入口，
  继续拥有本仓库的 CAF/FLAC 功能，不重复保存 BWF 实现。
- FFI、原生音频/Apple 适配、渲染场景与 DSP 仍属于本仓库。共享库不导出 C ABI，不新增 C 结构转换。
- cbindgen 按实际依赖包名读取共享记录；既有 repr(C) 布局与公开 adm_* 接口保持。
- 许可检查通过 Cargo metadata 定位锁定的 macin-adm 资源，不假设个人目录或 Cargo 缓存位置。

依赖提交由各 Cargo.toml 与 Cargo.lock 固定。正常构建使用 Git 依赖；本地开发可显式启用被忽略的
Cargo patch 文件，提交前恢复 Git lock。CI 与下游构建直接获取锁定的公开依赖，无需私有仓库凭据。

验收包括 Rust WAVE/Scene 测试、全 workspace Clippy、资源哈希与许可检查、Release 原生构建、
11 项文件 I/O/导出/CLI/C ABI 定向 CTest，以及 164 个公开导出和 FFI 结构/签名检查。
macin-adm/macin-bwf 的独立夹具在共享仓库继续执行，且共享库在 macOS arm64 与 Windows x64 原生通过。
