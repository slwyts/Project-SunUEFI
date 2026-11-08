# Piano 显示校准

两份数据分别匹配原厂 BOE 和 CSOT 面板，来源文件、ROM 版本和 SHA256 记录在
`manifest.json`。它们不含单机身份或用户数据。

驱动通过 IGCv5 将标准 PQ 或 Gamma 2.2 信号转换为线性光，通过 Gamut v4.3
完成 BT.2020 到 P3-D65 及原厂面板校准，再通过 GCv2 输出面板编码。
GC 保留原厂 `native_hdr` 的 1024 个主采样点和 256 个暗部点；三维 LUT
由原厂 17³ 校准表与标准线性 BT.2020 → P3-D65 变换组合。
文件中的峰值和黑位是原厂配置值，不是逐台光度测量。

可从同版本原厂 QDCM 文件重新生成：

```sh
./build.sh display-calibration \
  --boe-source /path/to/qdcm_calib_data_xiaomi_p81_42_02_0a_vid_mode_dualdsi_dsc_panel.json \
  --csot-source /path/to/qdcm_calib_data_xiaomi_p81_35_02_0b_vid_mode_dualdsi_dsc_panel.json \
  --rom OS4.0.4.0.XPYCNXM --output build/display-calibration
```

格式为 32 字节小端头、两组 1025 项均匀 Degamma LUT、三路打包 GC 表，以及
红色索引变化最快的 17³ RGB LUT。头部包含格式版本、面板 ID、毫尼特亮度和总长度。
空配置或校验失败不影响普通 SDR 显示；驱动不会开放 HDR。

原厂校准来自 Qualcomm／小米的专有配置，其权利不因重新打包而改变。
项目生成器的源码许可不覆盖原厂校准数据。
