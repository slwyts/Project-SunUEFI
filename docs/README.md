# 文档导航

[项目首页](../README.md)提供目标和简短状态；[项目状态](status.md)是当前功能、候选身份与限制的统一入口。技术文档中的局部“尚未验证”仅适用于其描述的版本/路径，历史记录不能替代当前状态。

## 先选一个入口

| 你要做什么 | 从这里开始 |
| --- | --- |
| 了解能否使用、怎样临时启动和返回 Android | [开始使用](user/getting-started.md) |
| 修改代码、运行检查或构建 | [构建与检查](devel/building.md) |

其余页面按需查阅，不需要顺序读完。

<details>
<summary>使用参考</summary>

- [当前功能与候选身份](status.md)
- [Fastboot 命令](user/fastboot.md)
- [已知问题](user/known-issues.md)
- [恢复细节](user/recovery.md)

</details>

<details>
<summary>维护参考</summary>

- [仓库地图](devel/repository-map.md)、[本地输入](devel/local-inputs.md)、[内核角色](devel/kernel-roles.md)
- [补丁登记](devel/patches.md)、[测试范围](devel/testing.md)、[发布要求](devel/release.md)
- [公开构建链](devel/public-build.md)、[BSP配置包](../linux/bsp/README.md)、[发行版装配](../linux/rootfs/README.md)、[桌面配置](../linux/desktops/README.md)
- [贡献指南](../CONTRIBUTING.md)、[来源](../THIRD_PARTY.md)、[许可](../LICENSE.md)

</details>

<details>
<summary>架构专题</summary>


这些现有文档保留原路径，便于源码和历史链接继续使用。它们描述各自契约和验证范围，不是普通用户安装指南。

| 主题 | 阅读入口 |
| --- | --- |
| 产品构建与共同核心 | [唯一产品构建](piano-product-build.md)、[启动策略](piano-product-boot-policy.md)、[共同 pump](piano-product-pump.md) |
| 应用 / OS 生命周期 | [产品 owners](piano-product-owners.md)、[OS 控制](piano-product-os-controller.md)、[退出契约](piano-product-os-exit-contract.md) |
| 内存与 DMA / SMMU | [内存契约](piano-platform-memory-contract.md)、[完整 DDR 缺口](piano-full-ddr-producer-gaps.md)、[DMA 基础](dma-smmu-milestone.md) |
| 显示 | [回归窗口](piano-display-regression-window.md)、[GOP 发布](piano-gop-framebuffer-publishing.md)、[映射契约](piano-display-mapping-contract.md) |
| USB | [驻留服务](piano-usb-persistent-service.md)、[验收工具](piano-product-acceptance.md)、[日志快照](piano-usb-log-snapshot.md) |
| UFS | [BlockIO / USB 进展](blockio-usb-progress.md)、[受控写入实验](ufs-controlled-write-transport.md)、[退出](ufs-ebs-lifecycle.md) |
| Linux | [构建历史与基线](linux-reproducible-builds.md)、[完整集成基线](piano-full-linux-candidate.md)、[用户态准备](piano-ram-hardware-prepare.md)、[EFI 会话](piano-linux-efi-session.md) |

</details>

## 历史与证据

[历史索引](archive/README.md)解释实验编号和旧结论的适用范围。原始设备记录保留本地；公开状态使用摘要、哈希与验证方法，避免依赖读者持有私有日志。
