# Piano 麦克风时钟

`linux/dts/piano-audio-dmic-clock.dtso` 在完整音频 overlay 之后应用，将 Linux 实际绑定的 `/soc/codec@7660000` 的 `qcom,dmic-sample-rate` 从 4800000 改为 2400000。主线驱动据此选择 divider 4。它只改变这一属性，不修改原厂 DTB，也不改变 DMIC1、DEC0、0 dB、48 kHz/S16 和两通道 PCM 配置。

依据是同机 Android 最终 DTB（2026-10-05，SHA256 `8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc`）及 [MiCode piano-w-oss 音频源码](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/tree/baeb7389997a6f6074dae31ec26565d6c286986e)。原厂 TX 四组 DMIC 配置均为 divider 4；VA 的 DT 配置为 divider 16，但 [VA hw_params](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/lpass-cdc/lpass-cdc-va-macro.c#L1414) 在采样率超过 16 kHz 时设置切换标志，[分频 getter](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/lpass-cdc/lpass-cdc-va-macro.c#L233) 返回 divider 4。

原厂最终 DTB 的 TX/VA core clock provider 都声明 19.2 MHz；[原厂时钟驱动](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/audio-ext-clk-up.c#L679) 将该频率传给 PRM。当前主线也请求 19.2 MHz，旧配置却选择 divider 2。这里对齐的是实际驱动分频选择，不能将原厂 DT 中的 16 直接推算为“实测 600 kHz”。

这仍是待实机对比的修正。公开源码与本机二进制的构建提交尚未确认一致；原厂普通录音的活动 mixer/DSP 路由和寄存器也未采集。未测得改善前不宣称麦克风质量已修好。

## 原厂路由与当前配置

2026-10-08 的原厂空闲采集位于 `private/captures/2026-10-08-factory-hardware/`。`tinymix-idle.txt` 的声卡是 `sun-mtp-snd-card`，TX/VA 的 CAP 全部关闭、DMIC mux 全为 ZERO，VA Volume 为 84；这些是空闲状态，不是录音活动路由。同目录提取的 `extracted/vendor/etc/audio/sku_sun/mixer_paths_sun_mtp.xml` 和 `resourcemanager_sun_mtp.xml` 提供以下静态配置：

| 路径 | 输入与增益 | 后端 |
| --- | --- | --- |
| `va-mic` / `va-mic-mono` / `va-mic-mono-lpi` | VA DEC0 → DMIC2；初始 VA Volume=84 | `CODEC_DMA-LPAIF_VA-TX-0` |
| `va-mic-dmic` / `va-mic-dmic-lpi` | VA DEC0 → DMIC2，DEC1 → DMIC3；各初始 Volume=84 | VA |
| `handset-mic` / `voice-rec-mic` | TX DEC2 → DMIC2，Volume=98 | `CODEC_DMA-LPAIF_RXTX-TX-3` |
| `speaker-mic` | TX DEC2 → DMIC0，Volume=85 | TX |
| `handset-mic-unprocessed` | TX DEC2 → DMIC1，Volume=85 | TX |

原厂 VA 的 `MSM_DMIC` 与主线的 `VA_DMIC` 都是 DEC mux 的值 0。[原厂 DMIC 枚举](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/lpass-cdc/lpass-cdc-va-macro.c#L1614) 与本地 `upstream/linux-piano/sound/soc/codecs/lpass-va-macro.c` 同为 ZERO、DMIC0…DMIC7，并在 CFG0 的 shift 4 选择输入；DMIC2/3 可以直接对应，不存在编号偏移。

当前 UCM 只开 VA DEC0/DMIC1，Volume=84。主线 Volume 的每格为 1 dB，84 对应 0 dB；98 对应 +14 dB，但原厂98属于 TX handset 路径，不能据此直接给当前 VA 加14 dB。增益增加也会放大底噪。当前 WirePlumber 将两通道 PCM 标为 `[ MONO AUX0 ]`，第二个通道没有路由是既有单麦设计，默认全零本身不说明缺少双麦。

原厂最终 DTB 的 `cdc_dmic01_pinctrl` / `cdc_dmic23_pinctrl` 选 LPI gpio6/7、gpio8/9 的 p81 active states，drive-strength=4；上游音频 overlay 对这四个引脚使用8。原厂节点中的 `qcom,tlmm-pins=<171 172>` / `<174>` 由 [msm-cdc-pinctrl](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/msm-cdc-pinctrl.c#L317) 保存为唤醒引脚，[唤醒操作](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/msm-cdc-pinctrl.c#L197) 调用 `msm_gpio_mpm_wake_set`；active 操作仅切 LPI pinctrl。这些 TLMM 属性不是麦克风供电开关，不应按供电猜测新增 GPIO 输出。

## 下一次实机比较

先启动包含 2400000 overlay 的 Linux，确认运行中的 `/soc/codec@7660000/qcom,dmic-sample-rate` 已是2400000，记录当前 mixer 与内核日志。保持 DEC0、Volume=84、48 kHz/S16 和两通道 PCM。用已有录音方式采一份 DMIC1 基线：同一距离，两秒安静、四秒正常说话。

然后只做一次真实的 DMIC2 对照。若用直接 ALSA 录音，先让桌面音频服务释放 capture PCM，启用既有 HiFi/Mic UCM，再将 mux 改为 DMIC2；不要在改 mux 后重新启用 Mic，否则现有 UCM 会写回 DMIC1。声卡0、capture PCM2 是当前配置，操作前按实际声卡核对：

```sh
amixer -c 0 cset "name='VA DMIC MUX0'" DMIC2
amixer -c 0 cget "name='VA DMIC MUX0'"
arecord -D hw:0,2 -r 48000 -f S16_LE -c 2 -d 6 /tmp/piano-dmic2.wav
```

仍按两秒安静、四秒说话采样，只比较有效通道的语音 RMS/峰值、底噪和 SNR。若 DMIC2 仍全零，保留该录音和活动 mixer/日志，再核对实际 pinctrl、时钟及 TX/VA 路径，不以加增益代替输入通路定位。比较结束恢复原 mux 和桌面音频服务；只有 DMIC2 实测正常并优于当前输入后，才将路由写回 UCM 与 BSP patch。

后续原厂采集需要在录音应用正在录音时保存完整 `tinymix`、活动 PCM 的 `hw_params`、应用及 AudioSource，确认最终选择 TX/VA、物理麦克风和增益。文件中存在的路由不代表应用实际启用了它。
