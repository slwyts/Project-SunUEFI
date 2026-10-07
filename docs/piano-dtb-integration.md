# Piano 显示 DTB 拓扑候选

当前产物只用于离线拓扑审核，`hardware_verified=false`、`enable_allowed=false`。Panel 和两个新 KTZ8866 节点保持 disabled；stock host/controller/provider 没有被伪装成主线接口。它不是完整主线 board DTS，也没有进行实机启动或显示测试。

## 来源和范围

基础是同机 `private/captures/2026-10-03-piano/live.dtb`，1,110,810 bytes，SHA256 `a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7`。该树已包含原厂 boot overlays，因此不再应用 stock dtbo entry0。它还含完整 vendor USB/touch/UFS/audio/video 等节点；本轮保留这些数据，未重新认证其主线兼容性。

显示源码在独立 `/home/slwyts/linux-piano-dtb` worktree 的 `topic/piano-dtb`，从 panel topic `33ca14a5b6673d562606433112c90b97903a8466` 派生；canonical commit 为 `1c2655ef4a94912fae39a14b943ba3d10b6847ce`。唯一新增 kernel 文件是116行 `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-display-topology.dtso`，不含大份 stock 反编译源码。

参考审核锁定到 `debian-piano e4004f5d5f08f79433b61a892c8b4f783c4c2e37` 的 display/video/touch-v2/usb-nopd9 文件，逐文件 URL/SHA 见 `linux/dts/display-review.json`。只取显示 wiring 信息；不会编译其 include 链、rootfs、UFS、GPU、音频、视频 codec、触摸或 USB 转换。参考 display overlay 的 synthetic supplies、rpmhpd hold consumer、第二个 apps SMMU 和 userspace devmem/bypass 修复均未移植。

## 精确映射

| 项目 | 本候选的映射及证据 |
| --- | --- |
| Panel 变体 | 捕获 `/chosen/bootargs` 的 selector 为 `p81_35_02_0b_dualdsi_dsc_vid`，即 CSOT；生成仍要求显式 `--variant csot/boe`，不沿用参考设备的 BOE 报告 |
| 双 DSI | 现有 `/soc/qcom,mdss_dsi_ctrl0@ae94000` 与 `ctrl1@ae96000` 的 output endpoint，分别互指 panel 的 port0/port1；尚未创建 DPU→DSI input graph |
| Panel | ctrl0 下新 `panel@0`，对应已移植 `xiaomi,piano-csot-nt36532` / `novatek,nt36532` binding；reset GPIO98，供电 refs 按 vddio/avdd/avee 命名 |
| vddio | 原有 `/soc/rsc@16500000/drv@2/rpmh-regulator-ldob12/regulator-pm-humu-l12`，phandle `0x3e`，捕获 min/max 均1,900,000µV；未复制参考1.8V stand-in |
| avdd / avee | 原有 `/soc/display_gpio_regulator_vsp` / `vsn`，phandle `0x794` / `0x795`，捕获电压幅值均5,800,000µV，GPIO117/118；未新增或修改 provider/电压/enable GPIO |
| primary KTZ | hub SE3 `/soc/qcom,qupv3_i2c_geni_se@9c0000/i2c@98c000/backlight@11`，GPIO2 HWEN，6 sinks，引用 VSP/VSN 与 secondary |
| secondary KTZ | QUP2 SE8 `/soc/qcom,qupv3_2_geni_se@8c0000/i2c@880000/backlight@11`，`kinetic,secondary`，无独立供应/背光设备 |

新 panel/KTZ 使用当前 panel topic 的 bindings；reset 的 active-low 描述来自固定显示参考的主线接口转换，不是本轮电气测量。GPIO refs 仍指向 stock `/soc/pinctrl@f000000`，其 `qcom,sun-tlmm` 尚不兼容主线，不能仅凭 phandle 可解析就启用。

14个新节点包括 panel、两路互指 endpoints、ports 和两个背光节点。对已存在硬件节点仅有4项受审查变更：DSI0 增加 panel bus 的 address/size cells，两个旧 `ktz8866@11` 改为 disabled，避免同地址旧/新驱动同时使用。供电、内存、chosen、DMA、clock/ICC、USB/UFS 等属性保持原字节值。

## 可复现组合和检查

```bash
python3 tools/compose_piano_dtb.py \
  --base private/captures/2026-10-03-piano/live.dtb \
  --kernel-tree /home/slwyts/linux-piano-dtb \
  --variant csot \
  --schema build/kernel-topics/piano-panel/Documentation/devicetree/bindings/processed-schema.json \
  --dt-validate build/kernel-topics/piano-panel-schema/bin/dt-validate
```

工具校验 base/overlay/GPIO header SHA 与 canonical Git blob；只给 base `__symbols__` 添加4个已核对路径 alias，不新建供电 provider。它检查 FDT header/block/reserve bounds、重复 phandle、节点/alias 冲突、受允许的属性变更、GPIO cells、单 phandle refs 和 endpoint 双向关系，再运行真实 `cpp → dtc -@ → fdtoverlay → dtc round-trip`。重放已应用候选、未知节点改动、输入漂移或启用新 compatible 节点会拒绝。

输出：`artifacts/dtb/display-topology/piano-display-candidate.dtb`，1,112,428 bytes，SHA256 `b8524c3ab2ebb759fd4988fa0c116f4948adff89ac7811cdb724b9bb99888898`，以及 `dtb-manifest.json`。

结构检查未引入新 dtc warning；stock baseline 与候选各有471行已有 warning，不能把它们隐藏成全树有效。对 NT36532/KTZ8866 的 scoped schema check 没有新诊断；两个 inherited `bias-disable: size (4) error for type flag` 在 baseline/candidate 均存在，已写入 manifest，未豁免。未请求 schema check 时工具明确记录 `unknown`。

## 尚未兼容的接口

- **MDSS/DPU/DSI/PHY**：stock vendor compatibles、reg/IRQ/clock 布局和全局 panel 描述与当前 mainline host 接口不同。只改 compatible 不够；缺 DPU input graph、PHY 和完整资源关系。
- **GPIO/regulator**：stock TLMM 和 L12B 的 vendor RPMh 层级尚需转换。原有供应路径存在不等于驱动可取得并安全控制这些 rails。
- **SMMU**：显示 SID `0x800` / mask `2`、继承的 hypervisor routes 与 UEFI owner 交接须单独验证；未建立新 context 或 bypass 脚本。
- **Clock/power/interconnect**：GCC/dispcc/RPMh/NoC 的 provider 和 votes 尚未转换；没有 fake fixed clock 或阻止 sync_state 的假 consumer。
- **背光 I²C**：hub SE3 与 QUP2 SE8 保留原厂 bus 兼容，mainline GENI clock/pinctrl/DMA 依赖仍缺。

后续应按这些真实 host/controller 接口逐层形成独立补丁和数据审核，完成 owner/stream/电源验证后才讨论启用。stable71 / next73 的 RAM闭环证据属于原先显式 stock FDT 的启动路径，不验证本候选的 native display。
