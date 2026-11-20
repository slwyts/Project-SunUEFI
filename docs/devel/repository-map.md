# 仓库地图与编辑边界

构建分五步：取固定版本的源码和输入，准备平台，编译，校验产物，打包。仓库里有一部分文件由脚本生成，下表标出了哪些可以直接改，哪些改动会在下次构建时被覆盖。

## 目录用途

| 目录/文件 | 来源 | 用途与修改方式 |
| --- | --- | --- |
| `uefi/core/` | 手写源码 | ProductCore、USB/UFS、输入与共享协议的源码，直接修改 |
| `uefi/components/` | 手写源码 | 按功能拆分的组件（`os-boot`、`linux-loader`、`product-handoff` 等），直接修改 |
| `uefi/handoff/` | 手写源码 | 前置选择器、BootShim 和冷启动早期内存代码，直接修改 |
| `uefi/platforms/pianoPkg/` | 手写模板 | 基础平台描述与组件引用，修改前核对生成工具 |
| `uefi/platforms/pianoProbePkg/` | 手写模板 | 诊断平台模板，也是产品组装的基线；ACPI 表在 `AcpiTables/` |
| `uefi/platforms/pianoProductPkg/` | 脚本生成 | 产品构建的临时平台，不提交，不要手工修改里面的文件 |
| `config/` | 手写配置 | 内核版本、固件清单、发行版依赖等配置，直接修改 |
| `linux/configs/`、`linux/dts/` | 手写配置 | 内核配置片段与设备树 overlay，直接修改 |
| `linux/bsp/`、`linux/rootfs/`、`linux/desktops/` | 手写配方 | 多发行版支持、根系统装配与桌面环境配置，直接修改 |
| `tools/` | 手写脚本 | 构建、打包、检查脚本，直接修改 |
| `tests/` | 手写测试 | 主机端 Python 和原生 C 测试，直接修改 |
| `upstream/` | 子模块 | 固定提交的上游源码，不要直接在里面修改，改动通过 `patches/` 表达 |
| `vendor/piano/` | 输入文件 | 板级 DTB、xbl-config DTB 和原厂 PE，由 `tools/piano_vendor_inputs.py` 校验 |
| `build/firmware-workspace/` | 脚本生成 | 产品固件的临时构建副本，不提交 |
| `build/`、`artifacts/` | 脚本生成 | 构建产物与中间文件，不提交 |
| `docs/` | 手写文档 | 用户和开发者文档，直接修改 |

## 产品固件怎样产生

`./build.sh uefi` 运行以下五步：

1. `tools/firmware_workspace.py` 在 `build/firmware-workspace/` 建立独立副本，不污染 `upstream/` 中的子模块。
2. `tools/apply_firmware_patches.py` 在副本里应用 `patches/firmware/series.json` 中的补丁。
3. `tools/prepare_product_handoff.py`、`prepare_product_pump.py`、`prepare_product_ui.py`、`prepare_nv_runtime_guard.py` 和 `build_simpleinit.sh` 安装运行时钩子和菜单。
4. `tools/prepare_product.py` 以 `pianoProbePkg` 为基线，组装 ProductCore INF、DSC/FDF 和 `uefi/components/` 中的组件，写入副本中的 `pianoProductPkg`。
5. `tools/build_stage0.sh product` 编译固件，`tools/build_integrity.py` 核对输入与输出，`tools/package_product.py` 完成最终打包。

修改功能时改 `uefi/core/` 和 `uefi/components/` 中的源文件；修改打包或依赖时改对应工具。
