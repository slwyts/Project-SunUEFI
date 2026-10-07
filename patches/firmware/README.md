# 固件上游补丁

三份标准 Git patch 固定于 `series.json` 指定的上游提交，记录 SHA256。它们保存 Mu Basecore、Mu Silicium 库绑定和 SimpleInit 的现有接入改动。产品构建先应用这些补丁，再由原准备工具复制本项目组件并核对安装后的实际源码。

`apply_firmware_patches.py` 只接受完整可应用或完整已应用的状态；未知修改会报冲突，不自动重置工作区。三份补丁已在各自干净的基线工作树上应用，并核对修改后文件与现有编译来源一致。

原厂 PE 不在这些补丁中，位于 `vendor/piano`；Linux 内核补丁另见 `patches/linux`。生成平台仍由正式模板和组件组装，不将手改生成目录作为维护方式。
