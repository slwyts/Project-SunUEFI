# Linux 补丁系列

[发布配置](../../config/release.json)选择产品内核和补丁系列。当前系列的
[`series.json`](7.3/series.json)记录公共源码基线、补丁顺序、提交及父链、
每份补丁的 SHA256 和应用后的源码树。

## 许可

本目录内由 Project SunUEFI 贡献者授权的原创补丁内容，包括对已有内核代码的
原创改动，均可依照 [GNU 通用公共许可证第 2 版或任何更新版本（GPL-2.0-or-later）](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)
使用、修改和再分发，许可全文见 [COPYING](COPYING)。这项许可适用于整个补丁系列中的上述贡献，不取决于补丁的分类前缀。

从 Linux、邮件列表或其他仓库引入的代码仍遵循其原有许可，保留版权和署名。
本声明不改变补丁应用后内核文件的 SPDX 标识，也不将 GPL-2.0-only 或带例外的
第三方代码重新授权。投稿时按 [Linux 内核的许可规则](https://docs.kernel.org/process/license-rules.html)
填写源文件标识，并保留作者及其 `Signed-off-by`。

## 维护与构建

按 Linux 主线习惯拆分补丁，每一项只做一个可独立审查的变更。入库时按
[内核补丁维护要求](AGENTS.md)导出邮件补丁。
修正尚未进入上游的新增代码时，修改引入它的补丁，再重放之后的补丁并更新清单；
修复上游已有的问题则另作独立补丁。迁移内核版本时，删掉已由上游替代的补丁，
同一个修复不按发行版复制。

在[构建环境](../../docs/devel/building.md)中运行 `./build.sh linux`。构建入口
从固定的公开来源准备基线，在 `build/kernel-worktrees/` 中按顺序应用补丁并核对
每步源码树。不需要在远端取原补丁的提交对象。应用记录写入
`build/release/source-manifest.json`，其中记录供后续构建使用的实际提交。
重跑会核对已有工作树；应用失败会保留该工作树，供排查。
