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

## CI 检查

GitHub Actions 的 **Build products** 在 `main` 上有相关源码变动时自动运行，覆盖 `uefi`、`linux` 以及四个发行版。CI 使用与本地相同的 `./build.sh`、固定子模块和补丁配置，不连接物理设备。

选择目标前会运行补丁系列检查；它检查共享引用、摘要、重复变更以及已登记系列的产品目标。单独运行 `./build.sh check --pattern test_check_patch_series.py`，规则见[补丁管理](patches.md#软件包补丁与产品目标)。
