# PianoUEFI 产品启动策略与应用调度

`PianoBootPolicy.c` 是共享核心内的一份启动策略。它不创建第二个 USB
控制器实例，也不会因为 SimpleInit、Setup、Shell 或其他 EFI 应用返回而
停止后台 USB 服务。产品构建仍需由 Root 启动实际设备 owner，再初始化
本策略；本文件不是实机验收记录。

## 默认启动及 F12

初始化时发布 revision 1 的 `PianoProductRuntime`，GUID 为
`618c4e8d-29ab-4fdc-a413-602b463972a1`。接口固定为
`Pump / BootServicesAlive / RequestAction / GetPendingAction / AckAction`。
默认动作是从产品载荷注册表取得 SimpleInit，验证载荷，调用共同的
`LoadImage / StartImage` 路径，最后归还载荷 lease。

真实 `SimpleTextInputEx.RegisterKeyNotify` 注册 F12。通知回调只锁存
`SETUP` 动作与序列号，不调用 Boot Services，不启动应用，也不伪造按键。
SimpleInit 通过共同 runtime 客户端协作返回后，父调度器才在
`TPL_APPLICATION` 调用 Setup。Setup 或 Shell 返回后，若没有更新的动作，
调度器再进入 SimpleInit。Shell 也使用共同的 FV 加载器。

实际 USB worker 请求 Continue、Reboot、Boot 或 Fault 时，APP pump 只锁存
`RETURN_CORE=4`。这个动作不会被后续 F12/UI 动作覆盖；GUI 协作返回以后，
策略消耗对应序列并返回 `EFI_END_OF_FILE`，由共享核心检查真实 USB 动作并
统一退休所有 owner。策略本身不停止 USB、UFS，不重启，不嵌套启动应用。
动作常量的增加没有改变五个方法的 ABI、GUID 或 revision。

普通 Setup 或 SimpleInit 请求冷重启时使用 input-only
`REQUEST_REBOOT=5`。策略仅在真实子 UI 活跃时接受，将 pending 映射成
`RETURN_CORE=4`，并在 `RequestedCoreAction` 记录
`PianoUsbServiceActionReboot`。已经存在的 USB/core 返回不能被新的 UI
重启覆盖。GetPendingAction 的消费者仍然只接受动作 0..4；Root 在子应用
返回并清理后读取这个真实原因，交给统一 owner manager 退休和重启。

Shell 和 Setup 通过产品绑定的协作检查退出。APP 的 wait-event hook 在
`RETURN_CORE` 时返回 `EFI_ABORTED`；DisplayEngine 和 Shell 的实际输入循环
识别这个状态，通过各自正常的清理路径返回。它们不会等待用户按 ESC，也
不会通过 `ExitImage` 或伪造按键跳过清理。详见
[Setup/Shell 返回核心实现](piano-product-ui-return.md)。产品实机验收仍未完成。

Setup 的前置条件是实际 HII 数据库、字符串、字体、配置路由、FormBrowser2、
显示引擎及 VariableArch/VariableWriteArch 协议。VariableArch 等标记协议
允许成功返回 NULL 接口；策略不要求 `EmuVariableNvModeEnable=TRUE`，因此不会
排斥后续真实的持久变量后端。协议存在也不等于持久变量已验收。

## 后台服务和键盘热插拔

runtime `Pump` 仅在真实 APP TPL、Boot Services 存活且没有递归调用时，
调用同一个 `PianoUsbControllerServicePumpApp` 实例，并读取它的真实状态。
未启动的、retained 的或已失去 Boot Services 的 worker 不会被报告为 ready。
`BudgetUs` 是调用准入与工作切片预算，不代表能抢占一个阻塞的原生 HAL。

每个 APP slice 都重新查找已经登记的键盘句柄。协议卸载或替换之后不会
解引用旧的接口或通知 token；新接口重新登记。停止策略时也先重新查找，
只有接口身份和注销方法都仍相同时才注销通知。同一接口对象的注销方法
发生变化、协议返回异常或登记 token 所有权不明，会保留策略并拒绝调度。
策略最多登记 32 个键盘提供者，枚举结果最多接受 256 个句柄。

## 应用返回及 ExitBootServices

FV 加载器按 GUID 查找实际 PE32 section，并检查认证状态及长度；不把固定
内存地址或编译成功当作找到应用。LoadOptions 使用独立副本。
`StartImage` 返回之后重新查询 `LoadedImage`：如果 Mu Core 已自动卸载应用，
不会二次卸载；如果应用仍存在，只有协议、ImageBase、ImageSize 身份一致，
才恢复原来的选项并卸载。无法确认身份、卸载/释放/关闭事件返回 warning 或
error 时，保留相关资源，拒绝启动下一应用。

每个应用上下文均有 ExitBootServices fence。通知只写 CPU 状态；通知出现
之后如果控制流仍然返回，代码屏蔽 DAIF 并停止，不再查询协议、释放内存、
关闭事件或调用旧的 USB worker。普通 UI 应用加载器不承担 OS 退出前的
设备退休；真实 OS handoff 需要共享核心另行完成
`PrepareOsExit → 最终 memory map → ExitBootServices` 合同。

## 当前证据与待接线

`tests/PianoBootPolicyTest.c` 编译实际策略和 FV 加载器，使用真实 UEFI ABI
签名进行 55 个隔离场景测试。覆盖默认 SimpleInit、F12 顺序、UI 返回不关闭
USB、实际 worker 状态、热插拔且旧接口不可访问、Setup 的空标记协议、
畸形 FV 枚举、自动卸载与显式卸载、异常句柄身份、warning/error 的资源保留，
以及 EBS 后使 `gBS` 不可访问的返回路径。增加了真实 USB 四种 stop action 的
GUI 返回、进入 UI 前返回核心、通知 TPL 拒绝派发、动作优先级与重复 pump
序列稳定等测试。ASAN/UBSAN 和 ARM64 语法检查通过。
共同客户端的 CoreWaitForEvent/GUI pump 测试也通过。

仍需 Root 把这些源码、GUID、共同 runtime 客户端、实际产品载荷和持续 USB
worker 接入唯一产品构建并实机验证。键盘 F12 的产品级验收需要真实输入
驱动提供 SimpleTextInputEx；主机模拟回调不能证明官方键盘已经可用。
Shell/Setup 全程 USB 可发现、任意界面插拔后的 fastboot 命令、跨 UI 返回的
会话连续性及 OS 退出退休仍需同一产品镜像上的实机记录。
