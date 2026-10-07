# 运行检查

普通改动先从这一条开始，只需 Python 3，既不连接平板，也不需要原厂采集或外部源码：

```sh
python3 tools/check_host.py --group portable
```

它显式运行产品契约、日志大小边界、RAM 日志收集的模拟 ADB 响应、构建新鲜度、DMA 日志、USB 诊断读取模型及 runner 自身测试。每个文件显示结果和跳过数量；portable 中出现跳过也视为失败。GitHub CI 使用同一个入口。

## 需要更多检查时

```sh
# 查看范围，不运行测试。
python3 tools/check_host.py --group portable --list
python3 tools/check_host.py --group python-all --list

# 已准备开发环境后，发现两个位置中的 Python 测试。
python3 tools/check_host.py --group python-all
```

`python-all` 包含 `tests/unit/test_*.py` 和 `tools/test_*.py`，逐文件运行，默认每文件 120 秒超时。部分测试依赖上游头文件、C 编译器、准备树或本地采集；缺少条件可能跳过或失败，输出会保留原因。不要把此组的跳过当成该功能已验证。

原有 `tools/test_*.sh`、独立 C/汇编夹具不会被 Python discovery 自动执行。修改某个原生模块时，还要运行该模块文档指定的入口；例如 DMA、UFS、USB、输入生命周期的 shell 组需要 Mu 头文件与编译器。请先查看脚本的工具路径和输入要求，按[构建准备](building.md)配置环境，别把所有测试盲目塞进公开 CI。

## 怎样报告结果

PR 写明运行的组或具体命令、通过/失败/跳过及原因。主机检查证明解析、契约或模拟行为；设备结论另记录源码/镜像身份、启动路径、实际屏幕/输入/数据结果与恢复情况。编译、模拟 DMA 或协议存在都不能替代真实硬件传输。

当前 CI 只覆盖 portable 组，不编译完整产品、不提供私有输入、不运行设备实验。发布前还需[发布要求](release.md)中的构建与设备验收。
