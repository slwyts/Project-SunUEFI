# Piano Linux 7.4-rc3 移植备份

这个分支保存 7.4 移植源码，供重装开发机后继续工作。它不改变
`config/release.json`：正式产品和软件源继续使用 Linux 7.3.3。

## 恢复源码

`piano-7.4.bundle` 保存三条 Linux 提交链及其原始作者信息：

| 分支 | 提交 | 用途 |
| --- | --- | --- |
| `codex/piano-linux-7.4-rc3` | `350a9d547b42` | 在平板用 systemd-boot 启动 Debian 并回退至 7.3.3 的早期移植 |
| `codex/piano-7.4` | `72fb58497c4e` | 已逐项重放核对的 133 枚补丁，以及后续源码差异的起点 |
| `codex/piano-7.4-pas` | `bd53c9a0e90b` | 复用主线 ADSP、CDSP PAS 描述；需要配合 `pas-device-tree.diff` |

先在 Linux 仓库取回官方 `v7.4-rc3` 标签并验签。它应指向
`aebb7274476403040d582c61478df8f4a7bf9cd3`。从 Linux 仓库运行以下
命令，将 `series_dir` 设为本目录的绝对路径：

```sh
git verify-tag v7.4-rc3
git bundle verify "$series_dir/piano-7.4.bundle"
git fetch "$series_dir/piano-7.4.bundle" \
  'refs/heads/codex/piano-7.4*:refs/heads/codex/piano-7.4*' \
  'refs/heads/codex/piano-linux-7.4-rc3:refs/heads/codex/piano-linux-7.4-rc3'
```

在从 `72fb58497c4e` 建立的独立内核工作树中，用
`git apply --index "$series_dir/rc3-hardware.diff"` 恢复后续驱动移植结果。
`git write-tree` 应输出 `0b3b5ac4d67b30de35a204f07ae57b658f6fc504`。
这份差异用于复原源码，不是要直接投上游的单枚补丁。后续按现行
`patches/linux/7.3/series.json` 的来源和职责拆分，修正折回对应补丁，
并同步未进入这份快照的新变化。

`pas-device-tree.diff` 和 `rust-toolchain.diff` 针对 SunUEFI 工程，
不应用到 Linux 源码。前者是 PAS 分支配套的设备树改动，后者保留 Rust
Binder 所需的工具链与原生打包适配。它们不参与此分支的正式构建。
