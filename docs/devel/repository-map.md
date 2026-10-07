# 目录与编辑边界

构建主线是 canonical 源码与固定输入 → 准备平台 → 编译 → 身份校验 → 封装。Git 跟踪状态和“是否由工具生成”是两个维度，不能简单把所有 tracked 文件视为手写源码。

| 目录/文件 | 用途与编辑方式 |
| --- | --- |
| `uefi/core/` | ProductCore、USB/UFS、输入与共享协议的 canonical 源码；直接修改对应文件并验证实际使用路径 |
| `uefi/components/os-boot/`、`uefi/handoff/early-memory/` 等 | 按功能拆分的 canonical 源码；产品准备工具复制并校验编译副本 |
| `uefi/components/product-support/` | 产品 GOP、RAM 日志与 BDS 支持来源；不要用 GUI staging 副本替代 |
| `uefi/components/product-pump/`、`product-handoff/`、`nv/` | 产品库、协议和标准组件接入来源；与对应准备工具共同维护 |
| `uefi/platforms/pianoPkg/` | tracked 板级平台快照；`prepare_piano.py` 会根据采集和参考模板重写部分内容，修改前核对生成规则 |
| `uefi/platforms/pianoProbePkg/` | tracked 诊断平台快照，也是产品组装模板；`prepare_handoff_probe.py` 会从基础平台重新准备，注意覆盖范围 |
| `uefi/platforms/pianoGuiPkg/`、`pianoLinuxPkg/`、`pianoProductPkg/` | ignored 生成平台；修改生成工具、模板或 canonical 源码，不以手改副本作为最终修复 |
| `config/`、`linux/configs/`、`linux/dts/` | 产品合约、内核配置片段与 DT overlay；改动绑定所用构建角色和实际输出 |
| `linux/kernel-profiles.json`、`upstream/linux-piano` | RAM 内核角色锁与 Gitlink；完整/磁盘角色见 [内核角色](kernel-roles.md) |
| `tools/`、`tests/` | 准备、校验、封装及测试入口；其中部分工具会操作设备，按各工具的实际行为选择入口 |
| `upstream/` | 正式依赖由固定 Git submodule 登记；额外本地缓存仍忽略。改动通过 patch/overlay 重现 |
| `.venv/` | 本地 Python 环境，不入库 |
| `build/`、`artifacts/` | ignored 工作目录与产物；manifest/哈希是构建证据，不是硬件验收 |
| `private/` | ignored 采集、恢复和原始证据；公开仓库不依赖其被自动分发 |
| `docs/status.md`、其他 `docs/` | 当前状态与专题/历史证据；历史文档保留日期、test-id 和镜像身份 |

## 产品平台怎样产生

[`prepare_product.py`](../../tools/prepare_product.py) 从 `pianoProbePkg` 复制模板，叠加 `product-support`，再从 `uefi/core/` 与 `uefi/components/` 复制实际核心和分层源码。工具生成 ProductCore INF、DSC/FDF、SimpleInit 摘要和经核对的存储基线，最后复制到 `upstream/Mu-Silicium/Platforms/Xiaomi/pianoProductPkg` 并记录 prepared manifest。

动态组装本身不要求把生成物再次提交。修改组件清单或绑定时改生成规则；修改行为时改 canonical C/头文件；修改模板时同时检查重建规则。生成文件用于检查组装结果，其手工改动会在重新准备时丢失，也可能被身份校验拒绝。

`build_integrity.py` 会核对实际编译输入、canonical 来源和输出。不要通过删哈希、改成功标记或替换旧产物来绕过检查；重新准备、构建与封装的关系见 [构建说明](building.md)。

Linux 设备配置位于 `linux/bsp/`，桌面默认位于 `linux/desktops/`，发行版配方位于 `linux/rootfs/`。三者通过统一构建入口组合，不能把配置层视为包含所有驱动和 Mesa 的二进制包。

硬件服务的 unit、启动脚本和原生程序目前来自不同目录，具体来源及 rootfs 安装顺序见 [Linux 服务源码地图](linux-services.md)。旧 `bootprofiles/` 已迁移到 `uefi/` 与 `linux/`；本地残留的 BootShim 二进制和 Python 缓存不再是构建输入。
