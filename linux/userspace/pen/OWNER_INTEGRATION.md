# 在唯一触控进程中接入笔

`piano-pen-owner.c` 由现有 `piano-touch-view` 的同一 poll 循环调用。
它不打开 THP FIFO，只读取匹配 Bluetooth VID `0x22`、PID `0x5081` 的
hidraw。实际 15 字节 Report5 按读取时的 `CLOCK_BOOTTIME` 时间进入 core；
NTP1 使用内核的同源时间，去除 257 字节传输头后把原始 type29 交给
`piano-pen-frame` 的双校验和 core。断开、流 epoch 和进程退出会释放输入状态。

默认不启用笔。实机候选用法是在现有 `input` 命令及手指参数后追加：

```sh
--pen-ini ACTUAL.ini --pen-pressure-max-age-ms 100 --pen-input
```

多支同型号设备同时存在时，使用 `--pen-hidraw /dev/hidrawN` 选定实际笔。
`--pen-json` 可输出已解码的原生 portrait 数值；未知字段为 null。
owner 不负责蓝牙连接、原厂初始化或修改显示／扫描状态；这些由现有 touch 服务中的 BlueZ 伴随进程处理。

笔通过独立的标准 tablet-tool uinput 设备上报。landscape 坐标为
`X=portraitY`、`Y=213599-portraitX`；倾角为
`tiltX=portraitTiltY`、`tiltY=-portraitTiltX`。
实际面板 239×163 mm 对应 X/Y 分辨率 1339/1310 native units/mm。
`BTN_TOUCH` 是每个 SYN 帧的第一项。真实 Report2 的 `02 6e`／`02 00`
作为 `BTN_STYLUS` 按下／松开，在真实工具进入范围时才上报；释放工具也
释放该按钮。HID 的 P81C 专用映射同时去掉重复的 F19 键盘输出。
未知报告不会生成笔身滑动或字符快捷键。

有效坐标表示进入范围；新鲜的真实压力大于零表示接触，真实零压力表示
悬停。缺失或过期压力会按显式时间策略释放接触，不补造零压力或未知倾角。
有效原始帧但 solver 不再给坐标时退出范围。100 ms 是本轮使用的调用者策略，
并非原厂超时参数。无有效帧超过同一期限也释放工具状态。数值或校验失败
仅计数，不生成替代坐标。

Type29 只有一个手指矩阵 quarter，不能直接送入 type3 的连续手指矩阵解码。
当前接入跳过这条路径，并在有效 type29 到来时释放旧的手指跟踪状态。
同时使用笔和手指所需的完整 quarter 桥接仍需实现。

构建时将 `piano-touch-view` 派生副本应用
`tools/patches/piano-touch-view-pen-owner.patch`，顺序在 observability 和
raw-capture 补丁之后。C 源码包括 touch-view、owner、frame；C++20 源码包括
core、decoder。最后使用同一 ARM64 GNU sysroot 的 C++ linker 静态链接，
不要使用主机 x86 的默认 ld。现有 Makefile 的 archive 已包含 owner C 对象；
touch-view 对象可与该 archive 及 `-lm` 通过 `clang++`／交叉 g++ 链接。
