# 运行检查

本地通过 `./build.sh check` 运行主机端检查，不需要连接平板。涉及写入和硬件交互的改动，还需要在实际平板上验证。

## 主机检查

日常改动只需 Python 3，不触碰设备：

```sh
./build.sh check
```

这条命令检查产品配置、日志大小上限、用模拟 ADB 收集 RAM 日志的流程、构建产物是否过期、DMA 日志、USB 诊断读取，以及测试脚本本身。输出里每个测试文件单独显示结果和跳过数。`portable` 组出现跳过按失败处理。

需要查看范围或运行完整 Python 单元测试时：

```sh
# 只查看测试文件列表，不运行
./build.sh check --list
./build.sh check --group python-all --list

# 运行 tests/unit/ 下的全部 Python 测试
./build.sh check --group python-all
```

`python-all` 运行 `tests/unit/test_*.py` 中的全部测试，默认每文件 120 秒超时。部分测试依赖上游头文件、C 编译器或特定输入文件，条件不足时会跳过或报错，输出里会说明原因。加 `--no-skips` 后，有测试跳过或指定的 `--pattern` 没匹配到文件，都按失败处理。

`./build.sh check --group ci --list` 列出 CI 运行的主机检查，包括无需私有样本的原生 Shell 测试。
需要本地固件、工具链或采集样本的其他检查按测试脚本的说明单独运行。

## 设备验证

声称某个功能在设备上可用，需要说明：

* 运行的镜像版本或提交号；
* 启动方式（合体 BOOT、`fastboot boot` 临时启动、或从已安装系统启动）；
* 实际现象，例如屏幕显示了什么、按键和触屏是否有响应、能否正常回到 Android。

主机检查只能证明解析和模拟逻辑正确，编译通过或协议已注册不代表硬件功能可用。

### 标准 rootfs 的硬件验收

验收“换发行版仍可用”时，使用匹配内核、设备树和标准固件目录，安装该发行版
的 libinput、BlueZ、libfprint／fprintd、libcamera 和 V4L2 工具。不要先安装项目
专用输入或相机服务再把结果算作标准组件支持。验证下载与安装流程时，使用完整
产品镜像首次启动；在运行中的旧系统临时执行新程序，只能证明该程序对应的检查。

| 链路 | 检查与操作 | 必须满足的结果 |
| --- | --- | --- |
| 触控笔 | `libinput list-devices`、`libinput debug-events`；两种笔绘画、悬停、倾角、按钮、手掌、磁吸、120／144 Hz、挂起恢复及固件重载 | 一个物理 tablet-tool，坐标与笔尖一致，抬笔／断连无残留压力；没有 THP／hidraw 用户态解码或 uinput。当前 BlueZ 初始化、弱悬停及 144 Hz 的缺项见[笔实现](hardware/pen-protocol.md#内核输入与许可边界) |
| 指纹 | 先核对传感器控制模型、固件许可和录入授权，再通过发行版 fprintd 检查录入、多指、验证、枚举、删除、取消及超时 | 使用已接受的设备控制模型和标准 TEE 接口，不依赖 UIO；不得在禁止 RPMB 写入时运行可能获取 HAT 的录入／验证。当前边界见[指纹实现](hardware/led-ir-fingerprint.md#侧指纹) |
| 相机 | `cam -l`、`v4l2-compliance`；前后摄逐模式拍照、持续录像，测量尺寸、帧间隔、掉帧和音画同步；检查自动／手动曝光、ISO、白平衡、后摄对焦、闪光和恢复 | libcamera 直接配置 TFE，IPA 使用硬件统计；记录实际曝光和增益的生效帧，ISO 与标定数据一致，不把模式枚举当成完整录像结果。标准路径不依赖 loopback、控制 socket 或软件 ISP。缺项见[相机接口](hardware/camera-controls.md#内核接口与-libcamera) |

记录实际软件版本、物理操作、设备节点和结果。只出现设备、驱动 probe 成功或
`v4l2-compliance` 的非开流检查通过，都不能替代上述功能验收。

相机迁移要检查全新安装和旧版升级。全新安装不包含 `piano-camerad`、
`v4l2loopback` 或项目控制 socket；旧版升级后卸载这些组件，再通过
发行版的 libcamera 和 PipeWire 用普通应用打开两颗相机。预览、拍照、
带声音录像、自动和手动控件均应可用，停流重开和睡眠恢复后重复检查。

## CI 检查

GitHub Actions 的 **Host checks** 在发往 `main` 的 PR 和 `main` 更新时运行
`./build.sh check --group ci`。它用固定的公开源码检查启动回退、Fastboot、NVRAM 掉电恢复、
运行时变量、整机重启、相机运行和原生打包。检查不使用发布密钥，选中的测试
如果跳过也会报错。只改测试不会触发产品构建。

**Build products** 在 `main` 上有相关产品源码变动时自动运行，覆盖 `uefi`、`linux`
以及四个发行版。构建前用 CI 主机的 `kernel-install` 在临时目录检查启动项更新和失败后的旧项保留，
再用断网的构建容器检查 APT／pacman 签名发布，以及 pacman 升级、卸载时保留管理员配置。
这两组测试都不使用发布密钥，有测试跳过也会阻止构建。两个工作流都不连接物理设备。
工作流启动后，构建选择器检查改名的新旧路径，文件移出产品目录也按移除构建输入处理。

选择目标前会运行补丁系列检查；它核对全部系列的产品目标、补丁来源、删除条件、文件摘要和重复引用，不需要完整 Git 历史。单独运行 `./build.sh check --pattern test_check_patch_series.py`，规则见[补丁管理](patches.md#软件包补丁与产品目标)。
