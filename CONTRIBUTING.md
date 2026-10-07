# 贡献指南

感谢关注 Project SunUEFI！本项目旨在为小米平板 8 Pro (`piano`) 构建一套标准且好用的 UEFI 固件，并推进主线 Linux 与 Windows on ARM 的硬件适配。

---

## 如何参与

* **汇报问题**：通过 GitHub Issues 反馈 Bug 或测试结果，请附上测试所用的镜像哈希、操作步骤与具体现象（若可用，提供 `fastboot -s SunUEFI-piano oem ramlog` 导出的运行日志）。
* **提交代码**：
  1. 优先拆分为独立、清晰的小 Pull Request，并在提交信息中说明改动原因与验证方式。
  2. 固件核心与生命周期代码位于 `uefi/core/` 和 `uefi/components/`；Linux 服务与适配来源见 [Linux 服务源码地图](docs/devel/linux-services.md)。完整目录说明见 [代码地图](docs/devel/repository-map.md)。
  3. 修改代码后，请在本地通过编译与完整性自检（参考[开发者手册](docs/devel/building.md)）。
* **AI 协助开发**：若使用 AI 编程助手（如 Claude、Gemini 等）进行代码编写或文档改进，请遵循仓库根目录的 [AGENTS.md](AGENTS.md) 协作规范。

---

## 协作守则

1. **唯一产品原则**：所有新增功能与修复均应向唯一的 `PianoUEFI-product.img` 收敛，避免引入割裂的独立测试产物。
2. **安全第一**：涉及 UFS 读写、内存映射与 ExitBootServices 交接的代码，必须保留严格的安全边界与退出释放逻辑，确保原厂分区安全。
3. **实事求是**：明确区分已在硬件上实测的功能与仅在理论/模拟环境下通过的代码，不把待验证目标写成已完成。
