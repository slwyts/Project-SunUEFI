# Android 模块

模块源码位于 `android/module/`，面向 Magisk / KernelSU 管理器。普通启动保留 Android，UEFI 使用独立的明确请求。原厂 Mi Recovery 已在target0验证；持久Linux偏好与`reboot recovery`组合仍待实机检查，新增的Recovery优先源码和证据见[重启请求说明](reboot-request.md)。[原生工具](android-boot-repack.md)已接通当前槽的在线身份检查、明确写入／读回、还原与持久请求接口，并验证真实 BOOT 文件无损还原及 CRC 改选。在线设备执行和同包装的启动／Recovery 证明尚未完成，因此仍不能生成可安装 ZIP。

先检查当前产品：

```sh
python3 tools/build_android_module.py --inspect
```

检查会验证产品 manifest、FD、BootShim 和 APPv1，并列出缺少的 selector、原生工具及设备证明。不会读取设备、下载旧 ROM 或写出 ZIP。后续具备真实输入时的入口：

```sh
python3 tools/build_android_module.py --product artifacts/product \
  --selector path/to/selector.bin --selector-manifest path/to/selector.json \
  --native-tool path/to/piano-boot-repack --native-manifest path/to/native.json \
  --output artifacts/android/SunUEFI-piano.zip
```

ZIP 只携带当前产品的 BootShim、FD、APP、早期 selector 和经测试的 ARM64 静态 ELF 工具，不携带原厂 BOOT/REC/内核。`selector.json` 记录 schema/interface 1、selector 大小、内存占用、metadata 偏移与 SHA、FD/APP SHA、BOOT v4、APP/wrapper ABI 1，以及 `entry_policy: explicit-request-only`、`request_bootarg: sunuefi.boot=uefi`、`persistent_uefi_request: false`。诊断包的持久 UEFI 标志不能作为日常模块入口。工具证明必须对应相同 payload、工具 SHA 与测试，另附 Android 旁路、持久请求处理与 CRC 改选和原厂 Recovery 保留的设备证据；三个标志 `device_passthrough_verified`、`request_handling_verified`、`standard_recovery_preserved` 缺一不可。

安装只允许正常启动完成的 piano Android、解锁状态和活动槽 `_a/_b`。原生 `probe` 读取当前 BOOT；ROM、bootimage 属性和实际 footer 指纹分别记录，不要求属性与 footer 版本相等。`repack` 写前重新核对身份与 source SHA，成功后 fsync 并完整读回。原生 `request --target android/uefi/linux/setup --preview` 只读；`--execute` 只为当前模块拥有的包装更新双页 NEXTv1，选择持续到下次明确改选，Android target0 清除 UEFI 路线。没有请求自动消费、misc/PMIC 写入或自动重启。接口在 [`native-interface.json`](../../android/module/native-interface.json)；在线读写、持久选择后的标准 Recovery 与同包装启动仍需实机证明。
`webroot/` 已提供状态、UEFI/Linux 两个按钮和只读目标预览。在线原生接口已实现；管理器 bridge 和设备证明尚未连接，按钮默认禁用；本地 JSON 只是包的信息，不是设备实时状态。页面没有内联 root 命令，只预留调用受限 `action.sh status/preview/request` 的适配接口；`action.sh` 无参数时仅显示状态。KernelSU 使用官方 [WebUI 目录与 API](https://kernelsu.org/guide/module-webui.html)；Magisk 可用模块 Action 入口，图形页面还需 [MMRL](https://mmrl.dev/) 等外部宿主，当前未做宿主兼容测试。

升级 OTA 后，应先正常启动新 Android，再重新安装模块。安装始终读取当时的活动 BOOT，不预写 OTA 的非活动槽；自动 OTA 适配还没完成。如果 OTA 保留了旧包装，工具应拒绝嵌套包装，由使用者先恢复当前 ROM 的原始 BOOT，再重新安装。

已有 `sunuefi_esp`、`sunuefi_root` 成对存在时，跳过分区步骤；只存在一个则停止。两者都没有时，音量上键轮换 No / 32 / 64 / 128 GiB root，音量下键选择，另外留 512 MiB ESP；超时选择 No。任何非零容量现在都会在 BOOT、F2FS、GPT 写入前停止。真正执行需要验证过的在线 F2FS 缩容工具、两个明文超级块检查、主备 GPT 核对，并分为缩容后重启、GPT 更新后再次重启两个阶段；没有 `dd` 或强制缩容退路。

卸载也交给原生 `restore`：从当前包装内保存的原内核、header/footer、布局及必要填充重建，验证与原镜像 SHA 一致后写回。模块不另存整个原厂分区；当前 ROM、槽位或镜像身份发生变化时必须拒绝，不能用旧 ROM 内容覆盖 OTA 新 BOOT。

安装格式使用管理器的 `customize.sh`，不支持 Recovery 安装；遵循 [Magisk 模块说明](https://topjohnwu.github.io/Magisk/guides.html) 与 [KernelSU 模块说明](https://kernelsu.org/guide/module.html)。本地检查：

```sh
python3 tests/unit/test_android_module.py
```

测试使用临时夹具验证篡改、路径越界、ELF 入口、打包门禁、音量选择和默认禁用的请求接口，不执行 Android 工具或访问设备。下一步由设备 owner 先做当前槽的只读 probe/status，再单独安排写入／读回、还原及同包装的 Android、持久请求和标准 Recovery；主机检查不能替代这一步。
