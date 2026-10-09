# 小米焦点触控笔 Pro 原厂固件

`p81c/0.0.32/` 保存从 Android 原厂 OTA 缓存取得的完整 ZIP 和解包后的 IMG。两份文件已经核对一致；来源、版本和校验值记录在 `manifest.json`，校验文件为 `SHA256SUMS`。

这是笔本体的固件，与平板上的 Novatek NT36532 触控控制器固件不同。本项目不会在 Linux 启动时自动刷写它。固件原始字节由厂商提供，项目没有给它另行授予开源许可。

容器与程序分析见 [固件分析](../../docs/devel/p81c-firmware.md)，原厂升级和日常震动协议见 [P81C 协议](../../docs/devel/p81c-ota-protocol.md)。
