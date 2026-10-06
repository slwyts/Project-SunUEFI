# 开发者编译与构建手册

欢迎加入 Project SunUEFI 的开发！本手册介绍如何在电脑上准备构建环境，并编译出唯一的统一产品镜像 **`PianoUEFI-product.img`**。

所有构建均在主机（PC）上完成，过程中**不会向任何连接的设备发起静默刷写**。

---

## 🛠️ 1. 主机开发环境准备

现有准备环境已成功构建产品；从全新克隆到完整构建的流程仍在整理。新贡献者可以直接运行下面的 portable 检查。

完整构建还需要固定版本的 Mu/SimpleInit 源码、`.venv/`、脚本使用的 `build/host-tools/usr` 工具链，以及从设备取得的 PE、DTB 和存储对照。具体材料见[本地输入](local-inputs.md)，来源见[第三方说明](../../THIRD_PARTY.md)。这些文件不会随克隆自动出现。

---

## 🧪 2. 快速自检（无需连接平板）

在开始全量固件编译前，可以先运行便携式主机自检，确保基础环境与校验算法正常：

```sh
python3 tools/check_host.py --group portable
```

该命令仅依赖标准 Python 环境，耗时数秒，检查产品配置、日志处理和构建状态管理。它不编译固件，也不检查完整工具链或硬件。

---

## 🏗️ 3. 在已准备环境中编译产品

当源码与依赖准备就绪后，直接执行一键编译脚本：

```sh
bash tools/build_product.sh
```

### 构建过程流水线：
1. **准备接入**：应用并核对共同服务、应用退出等源码修改。
2. **编译 UI 前端**：编译 ARM64 版 SimpleInit 图形应用与中文字体包，再组装 `pianoProductPkg`。
3. **编译 EDK2 核心**：调用 Clang 工具链与 BaseTools 编译生成 `PianoUEFI-product.fd`。
4. **封装产物**：将 BootShim 跳转头、固件 FD、DTB 设备树与 Android 启动头（boot header v3）合成为最终镜像。

### 产物输出位置：
* 固件镜像：**`artifacts/product/PianoUEFI-product.img`**
* 构建记录：`artifacts/product/manifest.json`；构建命令的输出可保存到 `build/logs/`。

### 校验构建完整性：
构建完成后，运行以下命令验证构建结果与合约一致性：

```sh
python3 tools/build_integrity.py validate --profile product
python3 tools/product_contract.py --build-manifest artifacts/product/manifest.json
```

---

## 🐧 4. Linux 内核与设备树编译

先从[内核角色](kernel-roles.md)选择基线，再按对应构建工具的 `--help` 准备参数。完整内核使用 `build_piano_full_kernel.py`，设备树使用 `build_piano_full_dtb.py`；二者仍依赖固定源码和已核对的板级输入。内核、config、DTB、initramfs 与模块要保持匹配，不能只替换一个 Image。

---

## 📖 深入探索
* 想了解代码组织和各目录的作用？查阅 [源码目录地图](repository-map.md)。
* 想了解各模块具体的测试命令？查阅 [测试与验证指南](testing.md)。
* 准备提交 Pull Request？请查阅 [贡献指南](../../CONTRIBUTING.md) 与 [AI 协作规范](../../AGENTS.md)。
