# Piano 实体摄像头隐私灯

该适配基于 `c8bf8df4d2ca3fe93886b94586c9c34141ccca2b`，通过内核补丁 `0016-piano-camera-privacy-led.patch` 与 `linux/dts/piano-camera-privacy-led.dtso` 接入默认唯一产品。Linux 下前后摄像头开流、停流与共享灯的驱动和 PWM 行为已经验证；实体发光尚未进行光学确认。

原厂 Android 的相机预览使 PM8550 LPG2 的 `green` 亮度从 0 变成 4，回到 Home、相机关闭后变回 0；满量程为 255。其间 LightsService 的八个灯颜色全部保持 0，实际 lights HAL 也只提供类型 0～7。这里直接使用已对应的 LED 硬件通道，不模拟 Android framework，也不增加后台守护进程。Linux 当前 LED 满量程同样为 255。

## 接线与正常生命周期

`v4l2-subdev.c` 为每个实际 `led_classdev` 保存一份注册引用数和活跃摄像头数。每个 sensor 保存一个活跃标志，重复获取或释放不会重复计数。第一份注册接管 sysfs/trigger，最后一份安全注销恢复 sysfs。新增 sensor 注册不会清掉另一颗正在使用的灯，一颗停止也不会关闭另一颗的指示灯。

`leds` 与 `led-names = "privacy"` 使用已有标准接口。`privacy-led-brightness = <4 255>` 是本项目提出的 common video binding 扩展，不是既有上游属性；它表示亮度比例并保留 LED 的真实满量程。当前 Linux 写 4；若 provider 满量程为 511，则换算为 8。共享消费者必须请求相同的数学比例，不同请求在注册时报错。

两颗 Piano sensor 在取得运行时电源引用之后、写 stream-on 之前同步获取指示灯。这样不会让前一次异步断电回调在启动过程中误释放新获取的引用。正常停止成功后释放引用；复位保持有效且 MCLK 关闭后，power-off 路径也能释放异常停止留下的引用。移除驱动时先完成这一步，再注销 LED 消费者。

## 失败处理

- LPG 的频率、PWM、输出控制、同步、SDAM/LUT、triLED 和 PBS 写入错误沿现有调用链返回到单色或多色同步亮度 setter；不再丢弃错误后返回 0。Piano sensor 收到点灯错误后不写 stream-on。返回成功仍代表硬件接口接受了编程，不等于做过光学测量。
- OV32D40 不再忽略实际 stream-off 写入错误。带 privacy LED 的 sensor 停止失败会保留错误与活跃状态，`.s_stream()` 包装层不能将它变成成功再灭灯。没有 privacy LED 的平台保留原行为。
- CAMSS 不因末端 sensor 停止错误中断剩余停止遍历和 pipeline/buffer 清理。部分启动失败只回退已成功启动的链路前缀，再清理 pipeline；失败节点由其自身处理电源引用。sensor 的实际错误仍逐项记录，不把软件 cleanup 当作 sensor 已停止。
- 两颗 sensor 用一个 `stream_pm_held` 标志追踪本次采集实际取得的唯一 PM 引用。正常停止、停止失败与启动失败各自只消费一次；停止失败后的快速重试在真实 reset/MCLK-off 之前返回 `-EAGAIN`，不会再次执行 CCI 或 `pm_runtime_put`。强制断电或移除仍持有引用时，通过原子交换消费该引用并使用 `pm_runtime_put_noidle`，不再次触发运行时断电。
- 异步 power-off 回调只记录已完成的 reset/MCLK-off，不抢 V4L2 state lock。下一次 enable 在已有流状态锁下清除旧的 enabled 状态，因此不会把停止失败时仍可能运行的 sensor 提前标为空闲，也不会因锁顺序发生死锁。
- 实际 stop 或 quiesce 已完成后，sensor 不再占用灯。即使 LED-off 写入失败，也会记录实际错误、释放已经安全停机的 owner，并允许最后一份注册释放 provider 引用；不会仅因灯的关闭错误永久扣住已停止摄像头的引用。实体灯可能暂时继续亮，错误日志不会把它报告成已经熄灭。

## 最小构建与剩余验证

五个修改对象均通过 ARM64 编译：`v4l2-subdev.o`、`ov32d40.o`、`s5kjn1.o`、`leds-qcom-lpg.o`、`camss-video.o`。DT overlay 通过 dtc 编译，并成功合并到现有完整 Piano DT；前后 sensor 的 privacy phandle 指向同一个 LPG2 节点。

2026-10-09，`7.2.9-piano-gnome-g5902d3af6f62` 实机前摄和后摄分别正常开流，亮度均为 `0 → 4 → 0`。两路同时工作时亮度保持 4，关闭前摄而后摄仍活动时保持 4，全部关闭后才变为 0。PWM 实际读回为启用、占空 13334/851667 ns，约等于原厂的 4/255；全部关闭后读回为禁用。测试帧均丢弃，没有保存照片或视频。这些结果证明控制器和驱动联动，不代替实体发光的光学测量；异常 I/O 路径没有在硬件上故意注入故障。

本适配处理 sensor 的停止/控制错误；独立的 CAMSS DMA 或硬复位故障仍由其原有底层驱动处理，不能把软件 cleanup 理解成所有 DMA 故障已经恢复。若其他 sensor 驱动在没有完成 stop/quiesce 的情况下直接注销，helper 仍保留不确定的活跃引用；Piano 两个驱动的移除路径已经明确先 quiesce。

源码与构建记录位于 `private/analysis/piano-camera-privacy-led-20261009/`，实机记录在 `private/provisioning/recovery-priority-20261008/privacy-exact-led-live-result.json` 和 `privacy-shared-live-result.json`。内核准备与产品打包均默认包含该适配。
