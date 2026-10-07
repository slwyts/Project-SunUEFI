# Piano 麦克风时钟

`linux/dts/piano-audio-dmic-clock.dtso` 在完整音频 overlay 之后应用，将 Linux 实际绑定的 `/soc/codec@7660000` 的 `qcom,dmic-sample-rate` 从 4800000 改为 2400000。主线驱动据此选择 divider 4。它只改变这一属性，不修改原厂 DTB，也不改变 DMIC1、DEC0、0 dB、48 kHz/S16 和两通道 PCM 配置。

依据是同机 Android 最终 DTB（2026-10-05，SHA256 `8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc`）及 [MiCode piano-w-oss 音频源码](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/tree/baeb7389997a6f6074dae31ec26565d6c286986e)。原厂 TX 四组 DMIC 配置均为 divider 4；VA 的 DT 配置为 divider 16，但 [VA hw_params](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/lpass-cdc/lpass-cdc-va-macro.c#L1414) 在采样率超过 16 kHz 时设置切换标志，[分频 getter](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/lpass-cdc/lpass-cdc-va-macro.c#L233) 返回 divider 4。

原厂最终 DTB 的 TX/VA core clock provider 都声明 19.2 MHz；[原厂时钟驱动](https://github.com/MiCode/vendor_qcom_opensource_audio-kernel/blob/baeb7389997a6f6074dae31ec26565d6c286986e/asoc/codecs/audio-ext-clk-up.c#L679) 将该频率传给 PRM。当前主线也请求 19.2 MHz，旧配置却选择 divider 2。这里对齐的是实际驱动分频选择，不能将原厂 DT 中的 16 直接推算为“实测 600 kHz”。

这仍是待实机对比的修正。原厂普通录音的活动 mixer/DSP 路由和寄存器尚未采集，公开源码与本机二进制的构建提交也未确认一致。下一次只做一次同距离的六秒采样：两秒安静、四秒正常说话，比较有效通道的语音电平、底噪和 SNR；未测得改善前不宣称麦克风质量已修好。
