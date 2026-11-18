# 运行检查

本地通过 `tools/check_host.py` 运行主机端检查，不需要连接平板。涉及写入和硬件交互的改动，还需要在实际平板上验证。

## 主机检查

日常改动只需 Python 3，不触碰设备：

```sh
python3 tools/check_host.py --group portable
```

这条命令检查产品配置、日志大小上限、用模拟 ADB 收集 RAM 日志的流程、构建产物是否过期、DMA 日志、USB 诊断读取，以及测试脚本本身。输出里每个测试文件单独显示结果和跳过数。`portable` 组出现跳过按失败处理。

需要查看范围或运行完整 Python 单元测试时：

```sh
# 只查看测试文件列表，不运行
python3 tools/check_host.py --group portable --list
python3 tools/check_host.py --group python-all --list

# 运行 tests/unit/ 下的全部 Python 测试
python3 tools/check_host.py --group python-all
```

`python-all` 运行 `tests/unit/test_*.py` 中的全部测试，默认每文件 120 秒超时。部分测试依赖上游头文件、C 编译器或特定输入文件，条件不足时会跳过或报错，输出里会说明原因。

`tests/native/` 下的原生 C 和 Shell 测试（例如 DMA、UFS、USB 状态机测试）不属于上述 Python 发现范围，修改对应原生模块时需按脚本注释单独运行，例如 `bash tests/native/test_usb_fastboot.sh`。

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

## CI 检查

GitHub Actions 的 **Build products** 在 `main` 上有相关源码变动时自动运行，覆盖 `uefi`、`linux` 以及四个发行版。CI 使用与本地相同的 `./build.sh`、固定子模块和补丁配置，不连接物理设备。

选择目标前会运行补丁系列检查；它检查共享引用、摘要、重复变更，以及已登记和本次修改系列的产品目标、上游状态与删除条件。单独运行 `./build.sh check --pattern test_check_patch_series.py`，规则见[补丁管理](patches.md#软件包补丁与产品目标)。
