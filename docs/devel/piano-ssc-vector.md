# 原始 SSC 向量接口

这份扩展准备在正常libssc中提供额外传感器事件，复用已有SUID发现、QMI传输及open/close流程。[源码补丁](../../patches/libssc/0002-raw-vector-reports.patch)应用在libssc0.4.4及property-types修正之后；[最小客户端](../../linux/userspace/piano-ssc-vector.c)只用公开libssc API。没有新增传感器daemon、IIO假设备或桌面策略。

当前`+sunuefi1`软件包只有属性类型修正；新向量补丁尚未加入默认消费者，后续正常包需要使用`+sunuefi2`并重新生成GIR，不用相同版本替换不同内容。此次host共享库和客户端已编译，原始捕获已离线解码，ARM64包、GIR安装和新接口实机订阅仍待正常打包验证。

## 应用接口

现有`SSCSensor`新增`raw-report(sensor, guint message_id, GBytes *payload)`信号。只转发与该对象实际SUID匹配的报告，仅有未阻塞订阅者时才复制客户端即将释放的GArray。GBytes保持原始不可变protobuf载荷；回调内借用，需要延长使用就调用`g_bytes_ref()`。信号同步运行在现有SSC客户端的report dispatch context，GUI应用需要按自身主循环要求转交处理。原有强类型measurement信号和采样策略不变。

公开函数`ssc_sensor_decode_vector_report(message_id, payload, &error)`返回`GVariant a{sv}`，调用方拥有返回引用。它只解释已核字段的wire类型，保留全部分量，不套用光线接口的“只取第0分量”规则：

| 原始事件ID | 返回字段 | 边界 |
| --- | --- | --- |
| 1025 | `values: ad`、`accuracy: i` | protobuf field1的全部float32及field2原始int32 |
| 769 | `field1: i`、`field2: i`、`field3: i`、`values: ad` | 三个原始int32及field4全部float32 |

float32转换为double保持数值，原始浮点位模式仍完整保存在GBytes。769的三个整数仅用字段号命名，不能把field3改名accuracy或把field1当频率。API要求完整已观察布局的scalar字段，不能据此推断所有vendor protobuf版本的required/optional声明；新布局应另核原厂解析器后扩展。

未知事件ID返回`G_IO_ERROR_NOT_SUPPORTED`，缺已支持布局的必需scalar、损坏packed float或空/超过64KiB载荷返回`G_IO_ERROR_INVALID_DATA`。原始payload仍由调用方持有，可以另存或交给后续decoder。重复向量没有人为补值；空向量仍为空向量，不表示已有测量。接口不添加lux、Kelvin、Hz、校准、姿态变换或固件时间戳。

## 一次性客户端

在安装带新API的标准开发包后正常编译：

```sh
cc -O2 -Wall -Wextra linux/userspace/piano-ssc-vector.c \
  -o piano-ssc-vector $(pkg-config --cflags --libs libssc gio-unix-2.0)
```

离线解码只读取参数，不打开QMI：

```sh
./piano-ssc-vector --decode 1025 --payload-hex 0a04000000001003
```

真实订阅由主任务另行执行；准确data type来自实际SUID发现，不扫描或造名称：

```sh
./piano-ssc-vector --sensor cct_front --seconds 3
./piano-ssc-vector --sensor ambient_light_back --seconds 3
./piano-ssc-vector --sensor flicker --seconds 3
```

客户端输出原始message ID、解出的GVariant字段和完整payload hex；配置等未知事件保持原始输出并注明不能解码。Ctrl-C或期限到达走现有close API，只有收到真实close完成回复才输出`closed=true`。发现/open超时先取消并保留最多5秒等待回调；停止后晚到的发现结果不会启动open，而是立即正常close。发现未返回或close仍超时会明确报错，不宣称已正常关闭。它是短时应用，不作为自启动服务。

## 本次依据与范围

源版本来自固定Debian`libssc_0.4.4-2.dsc`和原始tarball：DSC SHA256为`3f79e8cad936a647f2c7773c5fb2fa4fdd3a96d893f70279aa3e7a9b55023fef`，orig tar为`716d6bd6b34d2d753060c6b54c9a87e34fae75b724c763bf9ef487efa3621587`。DSC所列上游为[Codeberg libssc](https://codeberg.org/DylanVanAssche/libssc)。本机实际原厂`sensors.qsh.so`、`libsnsapi.so`和a8 SSC捕获用于限定数据类型与wire布局，原厂ELF没有在Linux执行。

本机`a8-ssc-vendor-payloads.jsonl`中的33条支持布局已由实际新共享库、protobuf-c和客户端离线解码：cct_front1条1025返回`[0]`、accuracy3；后置光线1条1025返回`[0,2,0,50,1024,1024]`、accuracy3；flicker31条包含769和1025。769的field1实际为493，message ID仍是769。这里只确认字段和值，不为分量补物理含义。缺accuracy、截断、packed float长度3字节和未知768的最小错误检查均明确拒绝。

离线结果与source/ELF/capture哈希记录在`private/analysis/piano-ssc-vector-20261008/`，不随公共镜像分发捕获数据。尚未运行新API实机订阅，也未因此宣称后置自动亮度、色温校准或防闪烁桌面策略完成。
