# 房间预设

房间数值组件通过 Git LFS 分发，JSON 清单、制作定义和校验文件由普通 Git 管理。
macOS、Windows 和 Linux 使用同一份资源。个人 SOFA 由用户独立选择，运行时原生加载；
这些预设不包含个人 HRIR，也不需要用户运行 Python 或转换 SOFA。

## 获取和使用

安装 [Git LFS](https://git-lfs.com/) 后，在已克隆的仓库根目录运行：

```sh
git lfs install --local
git lfs pull --include="resources/rooms/**" --exclude=""
```

`git lfs pull` 下载真实数值组件。只取得 LFS 指针文件时，房间加载的大小/哈希检查会失败。
使用包含原生房间 SOFA 导入的构建（`c03af5f` 或后续版本）：

```sh
build/release/mradm room-check \
  resources/rooms/reference-7x6x3.2-corrected-v1/room.json --sofa /path/personal.sofa

build/release/mradm render -i input.wav -o output.wav --renderer room-binaural \
  --room resources/rooms/reference-7x6x3.2-corrected-v1/room.json --sofa /path/personal.sofa
```

`room-check` 检查资源完整性和准备结果。声学适用范围以对应报告为准。
切换房间会继续使用当前个人 SOFA；更换个人 SOFA 时重建会话。

## 现有预设

| 预设 | 房间尺寸 | 固定监听点 | 资源大小 |
|---|---|---|---|
| [校正参考监听室 v1](reference-7x6x3.2-corrected-v1/room.json) | 7 × 6 × 3.2 m | (3.4, 3.0, 1.2) m | 6,647,188 字节，约 6.65 MB |

资源大小包含清单、制作定义和五个数值组件，不含校验文件。各文件 SHA-256 见包内
[`SHA256SUMS`](reference-7x6x3.2-corrected-v1/SHA256SUMS)，组件形状、类型和哈希也记录在 `room.json`。
这些组件及清单与已验收的校正包逐字节相同；清单 SHA-256 为
`40495a7c441c77d4b430e7b97278a42ee53786707ecb585a91fafe6ce596bdec`。

为保持原始清单和验证身份，v1 清单保留 `default_hrir: "hrir.json"` 回退名称。
本目录不提供该文件，使用时显式传入 `--sofa`，程序直接准备所选 SOFA。

- [声学验证报告](../../docs/architecture/ROOM_CORRECTED_ACOUSTIC_VALIDATION.json)：既定监听点、
  28 个参考声源位置及报告所述距离校准条件下通过；不代表任意位置、耳机或设备的结果。
- [校正来源与三平台对照](../../docs/architecture/ROOM_ACOUSTIC_CLOSEOUT.json)：保留原冻结参考，
  校正资源使用独立 ID；报告中的验收压缩包包含私人 HRIR，本目录只分发房间资源。
- [制作方法](../../scripts/room/README.md)：复用模板生成其他房间，另行完成声学验收。

现有清单中的 `validation_scope` 是生成时的完整性说明，后续声学结果由上述报告记录。
房间资源沿用仓库的 [MIT 许可证](../../LICENSE)。

## 维护

新增预设使用新的目录和资源 ID，保留既有版本。`.f32`、`.f64` 和 `.u32` 组件由
根目录 `.gitattributes` 自动纳入 Git LFS；清单和 `SHA256SUMS` 固定 LF 换行，保持三平台哈希一致。
提交前检查清单内的组件大小与 SHA-256、执行 `room-check --sofa`，并运行 `git lfs fsck`。
正常 `git push` 的 LFS 钩子上传对应数值组件。声学报告和资源校验分别维护。
