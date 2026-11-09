# 原始 SSC 报告与向量解码

libssc 扩展复用已有 SUID 发现、QMI 传输及 open/close 流程，提供原始报告和已核对布局的向量解码。`piano-ssc-vector` 是限时诊断客户端，不作为服务自启动，也不创建 IIO 设备。

共享补丁由 `config/release.json` 的 `sensors.patches` 接入 `./build.sh sensors`，经标准 Debian quilt/Meson 流程构建。`0003-libssc-raw-vector-reports.patch` 提供报告接口及客户端，对应 Debian 版本为 `0.4.4-2+piano1+sunuefi2`。

## 限时读取原始报告

使用准确的 SSC 数据类型订阅，默认读取三秒；`--seconds` 接受 1–30 秒：

```sh
piano-ssc-vector --sensor cct_front --seconds 3
piano-ssc-vector --sensor ambient_light_back --seconds 3
piano-ssc-vector --sensor flicker --seconds 3
```

客户端使用现有 `SSCSensor` 的 open/close 流程，支持 continuous 和 on-change 订阅。其他 stream type 返回不支持错误。

`SSCSensor::raw-report(sensor, guint message_id, GBytes *payload)` 只转发与对象 SUID 匹配的原始报告。`GBytes` 在回调内借用，延长使用需调用 `g_bytes_ref()`。信号运行在现有 report dispatch context，GUI 应用需转交到自己的主循环。未订阅该信号时不复制载荷，原有强类型 measurement 信号和采样策略保持兼容。

输出包含原始 message ID、完整 payload hex，以及已支持布局的解码结果。未知布局保留原始输出，不推测分量的物理单位。

Ctrl-C 或期限到达后调用现有 close API，收到关闭回复才输出 `closed=true`。发现或 open 超时会取消，并等待最多五秒处理关闭；停止后晚到的发现结果不会继续 open。程序不会写入 registry 或运行校准。

## 离线向量解码

`ssc_sensor_decode_vector_report()` 返回由调用方持有的 `GVariant a{sv}`，当前解释前置色温、后置光线和防闪烁报告中已经核对的布局：

| 原始事件 ID | 返回字段 | 含义边界 |
| --- | --- | --- |
| 1025 | `values: ad`、`accuracy: i` | field1 全部 float32、field2 原始 int32 |
| 769 | `field1: i`、`field2: i`、`field3: i`、`values: ad` | 防闪烁报告的三个原始 int32、field4 全部 float32 |

float32 转为 double 保持数值；原始位模式仍在 `GBytes`。769 的三个整数只按字段号命名，不推断频率、精度或物理单位。未知 ID 返回 `G_IO_ERROR_NOT_SUPPORTED`，损坏或不完整的已支持布局返回 `G_IO_ERROR_INVALID_DATA`，原始载荷始终可以单独保存。

```sh
piano-ssc-vector --decode 1025 --payload-hex 0a04000000001003
```

手动编译仓库中的同一客户端时，使用应用上述补丁的开发库：

```sh
cc -O2 -Wall -Wextra linux/userspace/piano-ssc-vector.c \
  -o piano-ssc-vector $(pkg-config --cflags --libs libssc gio-unix-2.0)
```
