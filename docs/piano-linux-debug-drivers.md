# Piano Linux userspace debug driver audit

Audit date: 2026-10-05. This is a read-only audit of the **actual Stable
userspace-debug build**, installed `modules.dep`/`modules.builtin`, pinned source
and captured board DT. No tablet module was loaded, no MMIO was read, and no
kernel/config/firmware/DT input was changed. The result is a distribution
preparation list, **not a USB or UFS hardware pass**.

## Current blocking difference

The gadget function side already exists in the Image. The first problem is the
board binding and provider chain, rather than missing ACM/NCM modules. The
existing display-only DT composition preserves vendor USB/UFS nodes; those
nodes have not been converted and accepted for these mainline drivers.

| Layer | Captured vendor DT | Pinned mainline board/driver | Practical gap |
| --- | --- | --- | --- |
| Core suppliers | `qcom,sun-gcc`, `qcom,sun-tcsrcc`, `qcom,sun-tlmm`, vendor `sun-*` ICC and RPMh clock nodes | GCC `qcom,sm8750-gcc`, TCSR `qcom,sm8750-tcsr`, TLMM `qcom,sm8750-tlmm`, mainline SM8750 ICC/RPMh bindings | These selected drivers do not match the captured vendor names. Provider resources, clock/reset/ICC IDs and consumer references must be converted together; a string-only rename is not evidence that the ABI matches. |
| USB parent | `qcom,dwc-usb3-msm`, nested `snps,dwc3` | `qcom,sm8750-dwc3`, `qcom,snps-dwc3`, single flattened controller | The vendor parent has no match in either built-in QCOM glue driver. Changing only the compatible string is insufficient: clocks, interrupt names, power domains, PHY graph and resource ownership differ. |
| HS PHY | `qcom,usb-m31-eusb2-phy`, legacy `usb-phy`/`usb-repeater` | `qcom,sm8750-m31-eusb2-phy`, generic `phys`/`phy-names` | Vendor compatible is unmatched; mainline wants ref clock/reset, `vdd`/`vdda12` and a generic repeater reference. |
| SS/DP PHY | `qcom,usb-ssphy-qmp-dp-combo` | `qcom,sm8750-qmp-usb3-dp-phy` | Vendor compatible is unmatched; named clocks/resets/GDSC, regulator supplies, orientation/mode graph and register extent differ. |
| PMIC repeater | `qcom,pmic-eusb2-repeater` | PMIH0108 node uses `qcom,pm8550b-eusb2-repeater` | Vendor compatible is unmatched. Actual rail/tuning conversion must be reviewed, not replaced with an unverified MTP default. |
| Type-C services | `qcom,qti-pmic-glink`, `qcom,sun-adsp-pas` | Both have explicit matches in this pinned fork | Matching is not service availability. ADSP firmware, GLINK endpoint, QRTR/PDR and UCSI connector/role graph must work. Vendor child `qcom,ucsi-glink` is not the auxiliary-driver match used by mainline UCSI. |
| Type-C mux | Captured WCD node `qcom,wcd939x-i2c` is disabled; a vendor FSA4480 node is enabled | Board skeleton selects WCD9395/9390 USBSS and marks GPIO/rail assumptions | Do not unconditionally enable/load WCD and FSA together. Resolve the actual active mux and conversion first. |
| DMA/SMMU | `qcom,qsmmu-v500`, vendor atomic/fastmap/reserved IOVA properties | `qcom,sm8750-smmu-500`, `qcom,smmu-500`, `arm,mmu-500` | The vendor SMMU compatible has no match in the selected mainline implementation. USB SID `0x40` and UFS SID `0x60` agree, but DMA ownership, provider binding and reserved mappings still need a validated handoff. |
| UFS | `qcom,ufshc` plus vendor PHY/MCQ/ICE resources | Same generic HC fallback; `qcom,sm8750-qmp-ufs-phy` has an explicit PHY match | HC compatible alone does not validate PHY, supplies, resets, OPPs or SMMU. The board skeleton still labels rail/GPIO choices as inherited/unknown. Crypto remains disabled in this build. |

Evidence: captured `private/analysis/live.dts`; pinned
[`sm8750.dtsi`](https://github.com/blu-sharky/linux-piano/blob/7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8/arch/arm64/boot/dts/qcom/sm8750.dtsi)
and
[`Piano board DTS`](https://github.com/blu-sharky/linux-piano/blob/7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts),
plus the corresponding `dwc3-qcom*.c`, `phy-qcom-m31-eusb2.c`,
`phy-qcom-qmp-combo.c`, `phy-qcom-eusb2-repeater.c`, `arm-smmu-qcom.c`,
`pmic_glink.c`, `ucsi_glink.c` and `qcom_q6v5_pas.c` match tables. See
`docs/piano-dtb-integration.md`: the current composed DT scope is display wiring,
not a USB/provider conversion.

## Actual compiled states

Source commit is `7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8`; release is
`7.2.6-00060-g7a33c60fd6ed`. Config SHA256 is
`7ba0e2bb4d8ec634fa4e78f33b391193808e9412e4ac94c2ffe883d20fde200a`.
Use only its matching module tree from
`artifacts/kernels/stable/userspace-debug/modules/lib/modules/<release>/`.
Do not use Android vendor modules or Next modules with this Image.

| Function/provider | Actual `.config` state | Installed result |
| --- | --- | --- |
| DWC3/QCOM/dual-role, xHCI, gadget, role-switch | `y` | `dwc3`, `dwc3-qcom`, `dwc3-qcom-legacy` and `udc-core` in `modules.builtin` |
| Configfs ACM/NCM | `USB_CONFIGFS`, `USB_F_ACM`, `USB_U_SERIAL`, `USB_F_NCM`, `USB_U_ETHER`, `CONFIGFS_FS=y` | `libcomposite`, `usb_f_acm`, `u_serial`, `usb_f_ncm`, `u_ether` built-in; no gadget `.ko` is required |
| USB PHY/repeater | `PHY_QCOM_M31_EUSB`, `PHY_QCOM_QMP_COMBO`, `PHY_QCOM_EUSB2_REPEATER=y` | Actual built-in paths include `phy-qcom-m31-eusb2`, `phy-qcom-qmp-combo`, `phy-qcom-eusb2-repeater` |
| GCC/TCSR/RPMh/ICC | `SM_GCC_8750`, `SM_TCSRCC_8750`, `QCOM_CLK_RPMH`, `QCOM_RPMH`, `QCOM_RPMHPD`, `INTERCONNECT_QCOM_SM8750=y` | GCC/TCSRCC, RPMh clocks/domains/regulators and `qnoc-sm8750` built-in |
| SPMI/SMEM/SCM/IPCC/SMP2P | Relevant providers `y` | Built-in; hardware/DT readiness still unknown |
| TLMM, GENI I2C, SMMU | `PINCTRL_SM8750`, `I2C_QCOM_GENI`, `ARM_SMMU`, `ARM_SMMU_QCOM=m` | `pinctrl-sm8750.ko`, `i2c-qcom-geni.ko`, `arm_smmu.ko`; QCOM implementation is part of `arm_smmu`, not a separate module |
| ADSP/GLINK/QRTR/PDR | `QCOM_Q6V5_PAS`, `RPMSG_QCOM_GLINK_SMEM`, `QRTR`, `QRTR_SMD`, `QCOM_PD_MAPPER`, `QCOM_PMIC_GLINK=m` | Runtime services/modules required for the selected normal PMIC role path |
| Type-C/UCSI/mux | Type-C core `y`; `TYPEC_UCSI`, `UCSI_PMIC_GLINK`, `TYPEC_MUX_WCD939X_USBSS=m` | `typec_ucsi.ko`, `ucsi_glink.ko`, conditional mux/altmode modules |
| UFS/readable disks | `SCSI_UFSHCD`, `SCSI_UFSHCD_PLATFORM`, `SCSI_UFS_QCOM`, `PHY_QCOM_QMP_UFS`, `BLK_DEV_SD=m` | Separate later-stage module group; `SCSI_UFS_CRYPTO=n` |

`cdc-acm.ko` and `cdc_ncm.ko` are **host-side** drivers. Loading them on the
tablet does not create `/dev/ttyGS0` or a configfs NCM gadget. They are useful on
the connected Linux PC (using that PC's own modules), or when the tablet later
acts as a USB host. `modprobe libcomposite` may be a harmless built-in lookup
with a complete module database, but this build needs no loadable libcomposite
object.

## Minimal staged module groups

The machine-readable `artifacts/linux-driver-audit/stable-userspace-debug.json`
contains exact installed paths, dependency-ordered closures, module hashes and
`modules.dep` hash. These lists are packaging inputs; **they are not an autoload
script**. `modules.dep` encodes symbol dependencies, not DT supplier readiness or
firmware/PDR/USB-role ordering. The two mainline DMA suppliers and 16-module PMIC
role closure form the core candidate list; the optional mux/SS branch has an
eight-module closure with shared dependencies. The five-module UFS closure is a
separate later stage. All 32 distinct listed module files were checksum-checked
against the build manifest.

1. Establish the explicit board DT, reserved-memory/SMEM/SCM/SPMI/clock/RPMh
   providers and the actual UEFI-to-Linux DMA retirement. Supply
   `pinctrl-sm8750` and `arm_smmu` for the verified mainline topology. Since DWC3
   is built-in, a default enabled node may already attempt probe; inspect deferred
   probe rather than assuming userspace controls all DMA driver startup.
2. For normal PMIC-controlled data role, package `qrtr-smd`, `qcom_pd_mapper`,
   `qcom_q6v5_pas` and `ucsi_glink` plus their dependency closures. Dependencies
   include `qrtr`, `qmi_helpers`, `qcom_pdr_msg`, `qcom_pil_info`, `qcom_sysmon`,
   `qcom_q6v5`, `qcom_glink_smem`, `qcom_common`, `mdt_loader`, `typec_ucsi`,
   `pdr_interface` and `pmic_glink`. The selected kernel PD mapper supports
   `qcom,sm8750` and stock `qcom,sun`/`qcom,sunp`; don't run a competing userspace
   mapper simultaneously. Require real remoteproc running, endpoint
   `PMIC_RTR_ADSP_APPS`, charger PD service and UCSI connection/data-role state.
   The selected SM8750 PAS resource has `auto_boot=true`; loading its driver can
   start the ADSP, so firmware, reservations and handoff are prerequisites rather
   than checks deferred until after an unrestricted module load.
3. For the validated mux/SS orientation topology only, add
   `pmic_glink_altmode` (and `aux-hpd-bridge`) plus the actual mux. The WCD branch
   requires `i2c-qcom-geni` and `wcd939x-usbss`; its GPIO, supply and active board
   status are presently unresolved. The artifact marks this branch conditional.
   ACM/NCM first acceptance can be at HS; SS/DP is a separate result.
4. Require a real `/sys/class/udc/` entry and device data role. Compose one RAM
   configfs gadget with `acm.<instance>` and `ncm.<instance>`, then bind it to that
   UDC. Bind after setting descriptors/functions/configuration; do not assume
   creating function directories alone enumerates a device. This ordering follows
   the [Linux configfs gadget documentation](https://docs.kernel.org/usb/gadget_configfs.html).
   Validate PC descriptors, serial bidirectional bytes, NCM link and IP traffic;
   only then enable a getty/SSH service. No mass-storage function is enabled.
5. UFS comes after USB logs and handoff proof. Package `phy-qcom-qmp-ufs`,
   `ufs-qcom` (with `ufshcd-pltfrm`, `ufshcd-core`) and `sd_mod`. First check
   enumeration/capacity and explicit read-only access. Merely loading the normal
   UFS driver does **not** impose a read-only media policy; mounting, filesystem
   recovery, discard or writes need their own approved scope. No UFS module is
   required for RAM gadget debug.

## Firmware and observable gates

DWC3, these PHYs, GCC, SMMU and configfs ACM/NCM do not request separate firmware
blobs in their audited source path. **Normal Type-C service relies on ADSP**, so
the transport can still be blocked by firmware or remoteproc even with every
USB symbol enabled.

The board skeleton requests `qcom/sm8750/adsp.mbn` and
`qcom/sm8750/adsp_dtb.mbn`; the captured vendor node requests `adsp.mdt` and
`adsp_dtb.mdt`. The compiled PAS driver's defaults are `.mdt`, but the actual
`firmware-name` in the accepted final DT overrides them. Choose one exact final
DT naming scheme and stage authenticated, device-compatible ELF headers and all
segments required by those headers (or complete combined images). The MDT loader
requests split `bNN` segments when data is outside the header file; do not copy
only two small header files and call firmware complete. No matching ADSP payload
was staged or authenticated by this audit. CDSP, modem, GPU and Wi-Fi firmware
are not part of the minimum USB debug objective.

Record separate milestones: matching `uname -r` and module provenance → actual
provider binds/deferred-probe reasons → real ADSP/GLINK/PDR/UCSI state → real UDC
and device role → PC enumeration → bidirectional ACM → NCM link/IP → optional
SS/orientation → later UFS read. Keep source/config availability distinct from
these observations. Read-only diagnostics can use dmesg, provider/remoteproc/UDC/
Type-C sysfs and `devices_deferred`; they do not require `/dev/mem` or blind
SMMU-bypass writes. No such live validation was performed here.
