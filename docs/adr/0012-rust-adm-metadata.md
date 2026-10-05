# ADR 0012：Rust ADM 元数据与 libadm 参考边界

> 状态：已接受；2026-10-05。平台验证见 Rust ADM 迁移记录。

生产 ADM XML 的解析、引用解析、场景投影、语义回写和输出元数据生成由 `mradm-adm` 完成。
本决议更新 ADR 0008 中暂不迁移 libadm 的阶段边界。libbw64、libear、C++ 场景所有权和公开 C ABI 保持原职责。

`mradm-adm` 禁止自身使用 unsafe，锁定 quick-xml 0.41.0，关闭可选 features。
标准 common definitions 数据随二进制嵌入，来源固定为 libadm 0.14.0，并保留许可证和 SHA-256。
无需在用户机器上安装 libadm 或查找 XML 资源。

`mradm-ffi` 聚合私有 `mradm_adm_*` 入口。输入是显式长度的 UTF-8 AXML、采样率及 CHNA UID 映射。
结果由 Rust 句柄拥有，通过只读记录数组、字符串索引和可选字段标记供 C++ 复制到 `AdmScene`；
视图不超过句柄生命周期，内存由分配侧释放，panic 和错误在边界转换为项目错误码。
这些记录不构成公开 ABI，也不向 GUI 暴露 Rust 内部类型。

回写保留原 XML，仅替换已支持且发生变化的字段。未知扩展、未修改的属性和文本继续保留，
无修改时 AXML 字节不变；WAVE 容器仍由 C++ 逐块复制，PCM、CHNA 和其他非 AXML 载荷不经解码或重编码。
同一共享节点的矛盾修改、结构不一致和需要重建 common-definition 引用链的编辑明确报错。
位置和 HOA pack gain/mute 回写仍遵循既有未实现边界。

默认构建、测试夹具和发行目标不获取或链接 libadm。`MR_ADM_BUILD_LIBADM_REFERENCE_TESTS` 默认关闭，
只在维护时编译冻结的 C++ 导入器和独立夹具生成器；启用它也不改变生产实现。
常规测试使用独立的轻量 XML 作者工具及固定 XML，不依靠 Rust 生成器为自身提供全部输入。

兼容性以场景结构、顺序、存在性、采样位置以及固定浮点容差验证。Release 音频对照使用
最大绝对误差 `2e-6` 门限，回写的原 PCM 载荷要求字节一致。本决议不承诺完整 libadm API、
新增 ADM 渲染语义或跨平台 PCM 位一致。
