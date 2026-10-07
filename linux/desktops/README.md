# 桌面配置

[catalog.json](catalog.json) 与发行版基础系统独立。GNOME/KDE 的候选组合执行时检查目标仓库包；DDE 只开放显式 Deepin 基础输入，不给 Debian/Ubuntu 添加 DDE PPA 或混源。

默认请求 200%，可用 assembler 的 `--scale-percent` 覆盖。GNOME 使用整数 toolkit 默认，KDE 使用 Qt 默认；这是轻量初始设置，不能替代未知屏幕的 Wayland 每输出布局验收。DDE 设置接口未确认，200%只记录为首次配置目标，不伪造已应用状态。

GNOME 使用 Shell 自带屏幕键盘，不安装 Caribou。Plasma 只在目标仓库能安装 Maliit、对应 desktop entry 存在时配置，且依赖 Wayland。DDE 键盘未确认，不创建虚假服务。旋转依赖真实传感器；安装 iio-sensor-proxy 不等于设备已支持自动旋转。

来源：[GNOME 屏幕键盘](https://help.gnome.org/gnome-help/keyboard-osk.html)、[Ubuntu noble Plasma 包](https://packages.ubuntu.com/search?keywords=plasma&searchon=names&suite=noble)、[Ubuntu noble Maliit](https://packages.ubuntu.com/noble/maliit-keyboard)、[Debian trixie Maliit](https://packages.debian.org/trixie/arm64/maliit-keyboard)、[Arch Linux ARM GNOME](https://archlinuxarm.org/packages/aarch64/gnome-shell)、[Arch Linux ARM Plasma](https://archlinuxarm.org/packages/aarch64/plasma-desktop)。
