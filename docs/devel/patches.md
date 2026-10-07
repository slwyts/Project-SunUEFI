# 补丁登记与应用顺序

现有改动同时使用 Git patch、固定源码的严格转换和 canonical 文件复制。各机制服务于固定输入；本页登记目前的目标、顺序和校验入口，不把转换为标准 patch 作为先决条件。局部源码身份、重复/部分安装拒绝和构建 hash 应保留。

## 产品构建中的顺序

[`build_product.sh`](../../tools/build_product.sh) 先准备 `build/firmware-workspace/`，随后在该副本中依次 apply handoff、pump、UI、NV guard，再编译 SimpleInit、prepare 产品、构建固件和封装。上游子模块不应用这些修改。SimpleInit 准备会再次核对/安装 pump 和产品 UI；产品准备再次核对 handoff/pump/UI，并复制 canonical 来源。

| 顺序/机制 | 目标 | 校验与用途 |
| --- | --- | --- |
| 1 `prepare_product_handoff.py` | Mu Basecore DXE 的 DxeMain、Image、Page，以及 ProductExit 库/声明 | 原始文件摘要与严格转换；`verify` 检查已安装摘要；原生晚期 APP/EBS 身份和退出 hook |
| 2 `prepare_product_pump.py` | Mu DXE Event/库声明、Silicium 库绑定、SimpleInit GUI 和复制的 product-pump 文件 | 固定仓库 commit；严格 anchor、重复/部分安装拒绝；`verify` 检查实际文件字节 |
| 3 `prepare_product_ui.py` | Mu DisplayEngine、SetupBrowser、UiApp、CustomizedDisplayLib、UefiLib、Shell 及 SimpleInit 导航/退出 | 固定 Basecore/SimpleInit commit；严格转换和 `verify`；保留原模块清理流程 |
| 4 `prepare_nv_runtime_guard.py` | Basecore `MdeModulePkg/Universal/Variable/RuntimeDxe/Variable.c` | 固定 commit、完整 guard 检查与 `verify`；产品编译 flag 为 `PIANO_NV_BOOT_ONLY=1`，不表示磁盘 NV 已就绪 |
| 5 `prepare_simpleinit.py` | SimpleInit 配置、输入/截图/兼容源码与生成 DSC | 固定 commit；source manifest 记录字体、owned sources 与 DSC 摘要；`simpleinit_build_identity.py` 绑定实际输出 |
| 6 `prepare_product.py` 与分层准备工具 | 生成 ProductPkg 与 upstream 编译副本 | canonical 副本、早期内存/交接/provider 校验；prepared manifest 与后续 build integrity |
| 7 `build_stage0.sh` 的 Mu patch | Basecore，来自 pinned Mu `Resources/MuPatches` | 按 `Auth-Service.patch` → `Boot-Manager.patch` → `Timer.patch` → `Usb-Bus.patch`；正向 apply check 或已应用的反向 check，均不匹配则停止 |

完成对应准备后，可单独检查产品 hooks（只操作/检查主机源码；要求已有固定 checkout）：

```sh
python3 build/firmware-workspace/tools/prepare_product_handoff.py verify
python3 build/firmware-workspace/tools/prepare_product_pump.py verify
python3 build/firmware-workspace/tools/prepare_product_ui.py verify
python3 build/firmware-workspace/tools/prepare_nv_runtime_guard.py verify
```

## 其他补丁与维护方式

`tools/patches/piano-touch-view-observability.patch` 是独立触控观测补丁，不属于上述产品默认应用序列；内核 topic commits 与 DT/设备供应者的 `apply_piano_*.py` 工具属于对应 Linux 角色，按所选基线与专题记录使用。不要把所有工具目录的补丁自动应用到每个 profile。

新增/更新登记说明目标仓库与 base、目的、应用位置/顺序、verify 和相关测试、上游状态及替代/撤销条件。标准 Git patch 通常便于 diff 审查和移植；严格转换在固定版本下便于拒绝未知状态。更换形式时比较实际升级成本，并验证相同的成功和负例；不以自动兼容未知源码作为健壮性结论。

kernel 补丁是否已被上游吸收，需分别提供 ancestry、patch-id/fingerprint 等证据；缺对象保留 unknown。删除或替代补丁前保留可恢复来源，重新验证对应角色，见 [内核角色](kernel-roles.md) 和 [测试入口](testing.md)。
