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

默认产品构建在 `build/firmware-workspace/` 的本地副本中应用补丁、生成平台和编译。`upstream/` 保留固定的上游源码，成品仍输出到主仓库的 `artifacts/`。修改本地适配时编辑 `uefi/`、`patches/` 或 `tools/`；不要修改构建副本。

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


## 新的统一构建入口（接入中）

上游已登记为固定版本的 submodule。首次从主仓库克隆后运行 `./build.sh sources`，只取得编译所需的子模块，不递归下载上游的测试和 fuzz 数据。`./build.sh check` 运行公开主机检查。

`./build.sh uefi` 使用 `vendor/piano` 的必要板级输入和 `patches/firmware`；`./build.sh linux` 从公开基线与八份补丁准备源码，再编译 LABEL 根策略的内核。ESP/root 打包与多发行版适配正在接入，不能仅凭命令存在当作所有发行版已经构建成功。

默认 Linux release 目标为 `7.2.9`：先保留 `debian-piano` 配套的 Piano 内核基线与八份本地补丁，再合入固定的官方 `v7.2.9` stable 提交。源码和记录位于 `build/kernel-worktrees/release-7.2.9/`、`build/release-7.2.9/source-manifest.json`；旧 Kernel 69 工作树和构建记录保留。只准备源码可运行 `python3 tools/prepare_release_kernel.py`，历史目标仍可用 `--target kernel69`。准备完成不代表内核已编译或平板已升级。

CPU 型号由 `linux/dts/piano-cpu-model.dtso` 给八个 CPU 节点补充标准 `model` 属性，内核的小补丁将其输出为 `/proc/cpuinfo` 的 `model name`，供 GNOME 等通用程序读取。这里只补充处理器名称；MIDR、核心拓扑、时钟和系统板型号都保持原值。`vendor/piano-linux/board.dtb` 是采集来源，应用 overlay 后的 DTB另行生成。

容器基础镜像固定在 `config/build-container.json`，定义见 `containers/Dockerfile`。实际 APT 包版本仍需记录，不宣称完整位级复现。构建在电脑/CI 中进行，安装器是独立入口。
