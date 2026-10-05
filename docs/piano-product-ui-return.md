# Setup / Shell 协作返回共享核心

本实现补齐产品界面收到 fastboot Continue、Reboot、Boot 后自动返回父核心的
路径，不再以等待用户 ESC 作为终态。它仍依赖 Root 启动真实持续 USB worker、
绑定产品 runtime 客户端，并由 Root 完成所有设备 owner 的统一退休。

`tools/prepare_product_ui.py apply` 在精确固定的 Mu_Basecore commit
`bb557081f80f4883ed832e34ab36bdca6ede1e10` 上添加协作检查。
SimpleInit 同时固定 `3d66a6e78d519dd050fbebde4db6c5ac933f9aa4`。
`verify` 检查实际构建源码；清单包含 28 个文件的 SHA。修改或重复的锚点会在
写入任何文件前被拒绝。产品构建必须同时应用 `prepare_product_pump.py`，并
把这两份清单纳入构建指纹。旧诊断构建仍绑定 `PianoProductPumpLibNull`，其中
返回核心检查恒为 FALSE；这些检查不会改变旧输入行为。

## 唤醒阻塞等待

Root 的真实 USB worker 在 APP slice 处理命令后锁存 `RETURN_CORE=4`。
共同客户端 `PianoProductReturnCoreRequested()` 只通过已存在的五方法
runtime 协议读取 pending action，必须为精确成功且动作是 RETURN_CORE。
它不 Ack，不执行新应用，不停止 USB/UFS。EBS fence 被触发时，客户端
屏蔽 DAIF 并停止，避免界面在返回 FALSE 后继续调用 Boot Services 清理。

CoreWaitForEvent 的 APP hook 在确认 RETURN_CORE 后返回 EFI_ABORTED，
因此等待按键的 Shell/Setup 可以醒来。CALLBACK/NOTIFY 不派发应用或设备工作。
有真实按键连续输入时，输入循环也提供 APP pump，避免仅靠无按键时的 wait
才服务 USB。这里不提供对阻塞原生 HAL 或任意 Shell 命令的强制抢占；退出在
下一个受控 APP/输入检查点进行，进行中的工作先正常返回。

## Setup 正常退出

DisplayEngine 的 UiWaitForEvent 用单独的 UIEventReturnCore 表示状态，不生成
键值。UiDisplayMenu 进入已有 CfExit，归还帮助文本；FormDisplay 继续释放
菜单对象。字符串/密码、数值、ordered-list、确认对话框、错误提示与 HII popup
等待同样可返回；字符串输入释放临时缓冲并恢复光标，ordered-list 使用已有的
取消恢复路径，popup 继续经过正常显示属性恢复和 selectable-option 释放。

浏览器在继续 ProcessUserInput 前检查返回请求，归还独立输入结果和 display
数据；原有 borrowed CurrentValue 缓冲不被重复释放。随后丢弃尚未提交的浏览器
数据，将 selection 设置为 UI_ACTION_EXIT，继续执行真实 FORM_CLOSE 回调与
HII package-notify 注销。这个路径不调用 ProcessAction 的系统 ExitHandler。

SendForm 继续执行 ExitDisplay、清空菜单历史、RestoreBrowserContext。
UiEntry 继续 FreeFrontPage，并在核心返回请求下跳过自己的 ResetReminder；
InitializeUserInterface 随后仍执行控制台模式恢复、字符串资源卸载和字体
HiiRemovePackages，正常返回父 FV 加载器。Root 才可以结束服务和决定重启或
内存启动，界面不会自行 reset。

若请求到达时已经进入重启提示，UefiLib CreatePopUp 的实际按键等待也能
返回；提示释放两份文字缓冲后返回，不调用自身的 ResetSystem。普通用户按
Enter 确认重启时，产品绑定也通过 `PianoProductRequestReboot()` 请求
`REQUEST_REBOOT=5`，然后正常返回。真实客户端拒绝将任何失败降级为原生
ResetSystem；Null 绑定保留原来的重启行为。

SimpleInit 重启菜单的实际路径是 bootmenu 的 `gui_run_and_exit(after_exit)`
→ `src/boot/reboot_uefi.c:run_boot_reboot`
→ `src/lib/reboot.c:adv_reboot`。产品绑定在 adv_reboot 的冷启动/重启请求上
使用同一个 runtime 请求；被接受后返回 0，boot dispatcher 归还数据并正常
返回。它不从 GUI 调用 owner manager 或直接 ResetSystem。Linux 分支和
旧诊断 Null 绑定不经过这个产品路径。

目前 UI 的产品请求只表达普通冷重启。warm、recovery、bootloader、EDL、
shutdown 或自定义 reboot data 尚未有相应 owner-manager 类型，产品暂时
返回 EOPNOTSUPP；不能把它们误当冷重启，也不能直接调用原生 ResetSystem。
这些目标仍需在统一退休之后逐个适配并实机验证。

## Shell 正常退出

FileInterfaceStdInRead 从被唤醒的等待返回 EFI_ABORTED，走原来的错误处理与
tab-completion 列表释放。DoShellPrompt 跳过新命令执行，RestoreBufferList 并
释放命令行；UefiMain 的提示循环观察核心请求后结束。

UefiMain 继续走 FreeResources：关闭 user-break timer、释放设备路径、撤销
ShellParameters 和 ShellEnvironment、清理临时缓冲与历史、移除 HII 字符串、
卸载 ConsoleLogger、清理环境变量列表。控制台暂停输出的等待也能中止，不
伪造 Ctrl-C、ESC 或修改输入缓存。共享 USB owner 保持由 Root 管理。

## 当前验证范围

`tests/test_product_ui.py` 执行实际补丁后的 UiWaitForEvent、WaitForKeyStroke、
DoShellPrompt、UiEntry 函数，以及 Shell 读行/完整 FreeResources、菜单 CfExit、
SetupBrowser FORM_CLOSE/notify 清理和输入取消的精确源码片段。主机提供
Boot Services 与设备模拟，覆盖 Continue/Reboot/Boot 的共同返回条件、无键唤醒、
临时输入归还、borrowed 缓冲避免二次释放，以及没有请求时原始按键路径。
ASAN/UBSAN 通过；清洁源应用、重复应用和拒绝损坏补丁三项测试通过。

实际修改的 18 个 C 源文件均使用现有 EDK2 AutoGen/include/ARM64 编译标志做
严格语法检查，通过。这些证据证明了实现和清理顺序；它们不证明平板上已经
完成 Setup/Shell 界面中的 fastboot 插拔、命令、自动返回、统一设备退休及
Android 恢复。最后仍需唯一产品镜像完成这些实机验收。

## 导航和 Shell reset

导航动作 1/2/3 使用相同协作退出路径，保留共享 USB/UFS。provider 对当前 ActiveAction 的重复请求成功无操作，父调度器在进入应用前 Ack，因此真实未消费导航是离开当前 UI 的请求。PianoProductUiReturnRequested() 查询实际 pending；CoreWait 仅在真实 APP pump 表示不同活跃界面的 yield 时唤醒，没有跨模块的猜测 UI 缓存。

Shell 实际 reset 命令在产品绑定下将 cold/default 和无数据的 -c 请求交给父核心；参数包正常释放，Shell 再清理退出。warm、shutdown、fwui 和自定义 reset data 目前返回 SHELL_UNSUPPORTED，在 fwui 写变量或任何原生 ResetSystem 前拒绝。产品请求失败返回 SHELL_DEVICE_ERROR，不降级原生 reset；Null 保留原行为。实际源码主机测试覆盖这些分支及参数包归还。

## Continue 的统一请求

产品Continue不是未配置的OS成功宣告。菜单文案「返回 Android（重启）」与现USB continue的已知冷重启策略一致，使用RequestContinue helper（输入动作6→输出pending4、真实reasonContinue1），经GUI正常清理、父核心和OwnerManager退休后执行。失败显示中文提示，旧diagnostic Null无该产品拦截。当前仍需要配置真正的OS boot backend才可把Continue升级为启动选定系统。
