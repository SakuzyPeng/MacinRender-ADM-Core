# Rust ADM 元数据迁移

生产 ADM 元数据已由 `mradm-adm` 接管：AXML 解析、common definitions、引用链解析、
Programme / Content / Object、Objects / DirectSpeakers / HOA 场景投影、语义回写及输出 AXML/CHNA。
接口和兼容性决议见 [ADR 0012](../adr/0012-rust-adm-metadata.md)。

## 所有权与行为

C++ 继续读取和重写 WAVE 容器、持有 `AdmScene`、执行语义策略和渲染编排。
Rust 在准备阶段持有 XML 文档和临时投影，C++ 通过私有类型化视图复制需要的数据。
渲染循环不解析 XML，也不持有这些临时句柄。

导入保留现有元素和轨道顺序、CHNA 大小写及尾部填充匹配、显式默认值与缺失字段的区别、
源增益单位、对象层级时间、纳秒转换后最近采样点取整、headLocked 覆盖关系及 HOA 元数据优先级。
不支持的 Matrix/Binaural 输入渲染继续产生既有警告。

回写支持对象 gain/mute、Objects 块 gain/diffuse/extent/divergence/channelLock/jumpPosition/
interpolationLength/headLocked、DirectSpeakers 块 gain/headLocked，以及 HOA 块 headLocked。
未知 XML 扩展在回写时保留，但不会因此成为新的渲染能力。引用同一源节点的修改必须一致；
修改 common definitions 块需要重新分配定义和更新 CHNA，当前明确返回 unsupported。
位置及 HOA pack gain/mute 的修改继续按既有接口约定忽略。

## 验证方法

- 默认 Debug CTest 覆盖导入、导出、容器、语义策略、C ABI 及各渲染器。
- Rust 专项覆盖字段存在性、分数时间、标准定义、命名空间、未知 XML、无操作字节保真、
  自闭合节点、可选属性删除、共享编辑冲突及错误路径；FFI 覆盖空参数、UTF-8 错误、panic 和释放。
- 可选 `mr_adm_libadm_reference_tests` 对比冻结导入器与 Rust 的完整场景 JSON 和源元数据，
  并由 libadm 解析 Rust 生成的双耳、HOA3、7.1.4、9.1.6 输出。
- `scripts/consistency/compare-adm.py` 对比两个 Release CLI 的 31 个音频场景，覆盖 EAR、VBAP、
  Triple Balance、双耳、HOA、时间窗口和语义策略。直接 extent 用策略禁用 diffuse，
  同时检查语义报告；源音频文件不修改。旧二进制用 SHA-256 标识，归档时的源码 HEAD 单独记录。

最终平台结果和限制记录在 [验收数据](evidence/rust-adm/validation.json)。本轮只交付本地分支，
未运行远端 Linux CI，也不推断跨平台 PCM 位相等。

## 2026-10-05 验收结果

| 验证 | 结果 |
|---|---|
| macOS arm64 Debug | 全套 63/63；最终修正后定向 9/9 |
| Windows x64 规范 Release | 全套 62/62；最终修正后定向 9/9 |
| Rust | 最终 workspace 128 项；其中 ADM 11 项、私有 FFI 22 项 |
| libadm 0.14.0 对照 | 10 组场景及源元数据一致；Rust 输出由旧库成功解析 |
| 同平台 Release 音频 | 31/31 逐位一致，最大样本差 0；包含两组直接 extent 语义报告验证 |
| 依赖与导出 | 两端默认依赖图无 libadm；无 `mradm_adm_*` 私有符号导出 |
| 质量 | rustfmt、Clippy `-D warnings`、变更 C++ 质量门及许可证检查通过 |

测试复用了现有构建目录。两台机器均保留已有 live VBAP 工作；本分支提交不包含这些改动。
全套测试后补充的空视图保护和参数范围校验均经过最终定向测试。角度、diffuse、divergence、
channelLock 等参数沿用旧库已有的取值范围，避免异常有限角度进入渲染阶段。

代码验收提交为 `7e3cfe44211c4f4088119cee4a0d6d98505f979e`。预迁移 Release CLI 在任务开始时归档，
其实际身份以音频记录中的二进制 SHA-256 为准，不将归档时 HEAD 当作重新构建的证明。
C++ 质量门仍显示既有长函数等告警；没有将其描述为零告警。参考探针的单次耗时只作观察，
不作为性能提升结论。
