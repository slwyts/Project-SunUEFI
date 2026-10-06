# 历史与实验记录

本目录和下列实验文档保存推导过程、故障与对照，不能直接作为当前安装教程。[当前状态](../status.md)优先说明哪些结论已被后续结果替代。

- [公开整理前 README 快照](readme-before-public-layout.md)：保留原始研发叙述，已有过时/矛盾结论，明确标为历史。
- 显示：第 [92](../piano-product-test92.md)、[93](../piano-product-test93.md)、[94](../piano-product-test94.md)、[95](../piano-product-test95.md)、[98](../piano-product-test98.md)、[99](../piano-product-test99.md)、[100](../piano-product-test100.md)、[101](../piano-product-test101.md)、[102](../piano-product-test102.md)、[103](../piano-product-test103.md)、[104](../piano-product-test104.md)、[105](../piano-product-test105.md)次记录及[回归窗口](../piano-display-regression-window.md)。
- 存储 / USB：[专用区域审查](../ufs-dedicated-area-audit.md)、[受控写入](../ufs-controlled-write-transport.md)、[第 91 次联合退出](../ufs-usb-retirement-test-91.md)。
- Linux：[EFI 交接审查](../linux-efi-handoff-audit.md)、[RAM 诊断入口](../ram-boot-probe-profile.md)、[基线构建与 RAM smoke](../linux-reproducible-builds.md)。

本轮没有批量移动技术文档和测试文件。它们仍被源码、测试和其他文档引用；后续迁移需核对用途与链接，必要时保留兼容入口。旧候选、私有记录和恢复输入没有被删除。

实验身份通常为源码 commit、build ID、镜像/config/DTB SHA256 和 test ID。`artifacts/tests/` 为本地封存产物，`private/analysis/` 为本地原始证据；路径存在于文档不表示它们随公开仓库交付。主机成功和实机成功分别记载。
