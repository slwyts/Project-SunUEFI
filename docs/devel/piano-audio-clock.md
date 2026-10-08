# Piano 麦克风时钟

`linux/dts/piano-audio-dmic-clock.dtso` 在完整音频 overlay 之后应用，将 Linux 实际绑定的 `/soc/codec@7660000` 的 `qcom,dmic-sample-rate` 从 4800000 改为 2400000。主线驱动据此选择 divider 4。它只改变这一属性，不修改原厂 DTB，也不改变 DMIC1、DEC0、0 dB、48 kHz/S16 和两通道 PCM 配置。

依据是同机 Android 最终 DTB（2026-10-05，SHA256 `8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc`）及 [MiCode piano-w-oss 音频源码](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/tree/baeb7389997a6f6074dae31ec26565d6c286986e)。原厂 TX 四组 DMIC 配置均为 divider 4；VA 的 DT 配置为 divider 16，但 [VA hw_params](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/lpass-cdc/lpass-cdc-va-macro.c#L1414) 在采样率超过 16 kHz 时设置切换标志，[分频 getter](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/lpass-cdc/lpass-cdc-va-macro.c#L233) 返回 divider 4。

原厂最终 DTB 的 TX/VA core clock provider 都声明 19.2 MHz；[原厂时钟驱动](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/audio-ext-clk-up.c#L679) 将该频率传给 PRM。当前主线也请求 19.2 MHz，旧配置却选择 divider 2。这里对齐的是实际驱动分频选择，不能将原厂 DT 中的 16 直接推算为“实测 600 kHz”。

g086 实机已读回该属性为2400000；这确认配置已应用，不代表已测量物理 DMIC clock 或验证语音质量。公开源码与本机二进制的构建提交尚未确认一致；原厂普通录音的活动 mixer/DSP 路由和寄存器也未采集。未测得改善前不宣称麦克风质量已修好。

## 当前麦克风配置与受控比较

2026-10-08 使用电脑的实际扬声器播放已知500/1000/2000Hz声源，自动在PCM开始后播放；仅保存逐秒电平、频谱和削顶计数，不保存麦克风音频。DMIC2比DMIC1的响应更强，与原厂VA路由一致。DMIC2、Volume98（+14dB）的稳定段中，三频信号相对后段环境背景约为18/27/21dB，没有削顶；声源距离与声压未标定，不能把它当完整语音质量验证。

开流首段仍有约41个满幅样本，之后没有；此前短采样的高RMS包含启动冲击，不能直接当持续底噪。当前UCM已选DMIC2/DEC0和Volume98，保留驱动要求的两通道PCM及MONO/AUX0，不添加虚拟入口。完整release和多发行版BSP使用同一份tracked HiFi.conf；原厂TX路径与LinuxVA后端仍有差异，启动冲击、正常语音与DSP处理继续排查。实际统计在private/analysis/pc-speaker-mic-20261008/RESULT.json。

以下保留此前DMIC1/0dB与时钟、原厂路由的定位记录。

## 原厂路由与此前配置

2026-10-08 的原厂空闲采集位于 `private/captures/2026-10-08-factory-hardware/`。`tinymix-idle.txt` 的声卡是 `sun-mtp-snd-card`，TX/VA 的 CAP 全部关闭、DMIC mux 全为 ZERO，VA Volume 为 84；这些是空闲状态，不是录音活动路由。同目录提取的 `extracted/vendor/etc/audio/sku_sun/mixer_paths_sun_mtp.xml` 和 `resourcemanager_sun_mtp.xml` 提供以下静态配置：

| 路径 | 输入与增益 | 后端 |
| --- | --- | --- |
| `va-mic` / `va-mic-mono` / `va-mic-mono-lpi` | VA DEC0 → DMIC2；初始 VA Volume=84 | `CODEC_DMA-LPAIF_VA-TX-0` |
| `va-mic-dmic` / `va-mic-dmic-lpi` | VA DEC0 → DMIC2，DEC1 → DMIC3；各初始 Volume=84 | VA |
| `handset-mic` / `voice-rec-mic` | TX DEC2 → DMIC2，Volume=98 | `CODEC_DMA-LPAIF_RXTX-TX-3` |
| `speaker-mic` | TX DEC2 → DMIC0，Volume=85 | TX |
| `handset-mic-unprocessed` | TX DEC2 → DMIC1，Volume=85 | TX |

原厂 VA 的 `MSM_DMIC` 与主线的 `VA_DMIC` 都是 DEC mux 的值 0。[原厂 DMIC 枚举](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/lpass-cdc/lpass-cdc-va-macro.c#L1614) 与本地 `upstream/linux-piano/sound/soc/codecs/lpass-va-macro.c` 同为 ZERO、DMIC0…DMIC7，并在 CFG0 的 shift 4 选择输入；DMIC2/3 可以直接对应，不存在编号偏移。

此前 UCM 只开 VA DEC0/DMIC1，Volume=84。主线 Volume 的每格为 1 dB，84 对应 0 dB；98 对应 +14 dB，但原厂98属于 TX handset 路径，不能据此直接给当前 VA 加14 dB。增益增加也会放大底噪。当前 WirePlumber 将两通道 PCM 标为 `[ MONO AUX0 ]`，第二个通道没有路由是既有单麦设计，默认全零本身不说明缺少双麦。

2026-10-08 g086 的真实 PipeWire 默认源为既有 `HiFi__Mic__source`，PCM 是
`hw:Pro,2`；空闲时 suspended，EnumFormat 为 S16LE/48 kHz/两通道 MONO、AUX0，
硬件未开流时 Format 为空。volume、softVolumes、channelVolumes 都是1.0，
dither 关闭，没有观察到额外软件 boost。实际 q6apm capture frontend 的
`channels_min=2`，不能仅把 UCM CaptureChannels 改成1；VA codec 本身支持mono
不等于整个 PCM 支持mono。

保持 gain84 和同一 PCM/格式，各做了一次两秒环境幅度统计，只在内存处理并输出
峰值/RMS，没有保存音频。DMIC1 的通道0 RMS为−32.33 dBFS，DMIC2为−32.18 dBFS，
两者 peak=32767，通道1均全零。DMIC2 在统计前后 mux 读回均为enum3；结束已恢复
DMIC1 enum2、gain84。环境未确认静音且含满幅峰值，0.14 dB差异不能证明底噪或
SNR改善，也没有验证正常说话响应。DMIC2返回非零 PCM，仍是下一次受控声源
比较的优先候选；本轮未改UCM、默认源或加入虚拟入口。统计与参数记录在本地
`private/analysis/g086-mic-20261008/RESULT.json`。

原厂最终 DTB 的 `cdc_dmic01_pinctrl` / `cdc_dmic23_pinctrl` 选 LPI gpio6/7、gpio8/9 的 p81 active states，drive-strength=4；上游音频 overlay 对这四个引脚使用8。原厂节点中的 `qcom,tlmm-pins=<171 172>` / `<174>` 由 [msm-cdc-pinctrl](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/msm-cdc-pinctrl.c#L317) 保存为唤醒引脚，[唤醒操作](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/msm-cdc-pinctrl.c#L197) 调用 `msm_gpio_mpm_wake_set`；active 操作仅切 LPI pinctrl。这些 TLMM 属性不是麦克风供电开关，不应按供电猜测新增 GPIO 输出。

## 下一次受控声源比较

先安排明确的测试窗口和声源，不自行开始正常说话采样。保持 DEC0、gain84、
48 kHz/S16 和两通道真实 PCM，只输出分阶段峰值/RMS，不保存 PCM 或音频文件。
在确认环境静音及同一距离的受控声源后，才比较 DMIC1 与 DMIC2 的语音幅度和
本底；本轮两秒环境数据不能代替它。

若用直接 ALSA 读取，先核对 capture PCM 是否空闲、既有 HiFi/Mic UCM 已启用。
现有 UCM EnableSequence 会写回DMIC1，所以不能改mux后重新启用Mic。只切实际
VA mux，保持其他参数，结束恢复DMIC1和gain84：

```sh
amixer -c 0 cset "name='VA DMIC MUX0'" DMIC2
amixer -c 0 cget "name='VA DMIC MUX0'"
# 在已确认的窗口用level-only工具读取hw:0,2，输出峰值/RMS；不写音频文件。
amixer -c 0 cset "name='VA DMIC MUX0'" DMIC1
```

只有 DMIC2 的受控声源响应确实正常并优于当前输入，才将路由写回 UCM 与 BSP。
TX98属于不同后端及处理链，不能先把当前VA加14dB当作改善。

后续原厂动态采集应在用户明确打开原厂录音应用、选定 AudioSource 的同一窗口
进行，只读取路由/格式/效果元数据，不自行开启录音或收集音频：

```sh
tinymix -D 0
cat /proc/asound/cards /proc/asound/pcm
for p in /proc/asound/card*/pcm*c/sub*/hw_params; do echo "$p"; cat "$p"; done
dumpsys media.audio_flinger
dumpsys media.audio_policy
```

完整tinymix确认最终TX/VA、DMIC和gain，活动hw_params确认rate/format/channels；
AudioFlinger/AudioPolicy用于关联录音source、后端、AGC/NS/AEC效果。XML中有某条
路由不等于应用实际启用了它。
