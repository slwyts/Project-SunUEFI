# 许可证范围说明

本仓库采用已有的文件级许可

## 已有声明

| 声明 | 已核对的代表文件 | 范围说明 |
| --- | --- | --- |
| BSD-2-Clause-Patent | [PianoProductCore.c](bootprofiles/uefi-app/PianoProductCore.c)、[PianoEarlyMemory.c](bootprofiles/early-memory/PianoEarlyMemory.c) | 适用各文件已有声明，不覆盖全部 UEFI 代码。 |
| GPL-2.0-only | [PianoKeys.c](bootprofiles/uefi-app/PianoKeys.c)、[PianoPmicMetadata.c](bootprofiles/uefi-app/PianoPmicMetadata.c) | 保留已有标识和参考来源。 |
| GPL-2.0-or-later | [PianoTouchProbe.c](bootprofiles/uefi-app/PianoTouchProbe.c) | 保留已有标识；与 GPL-2.0-only 分别记录。 |
| LGPL-3.0-or-later | [simple-init runtime](bootprofiles/product-pump/simple-init/src/gui/piano_product_runtime.c) 及其头文件 | 适用这些文件，不覆盖整个固件或所有工具。 |
| BSD-3-Clause | [Linux DT overlays](configs/linux/dtb/piano-linux-owned-dma.dtso) 等带相同标识的文件 | 以文件自身声明为准。 |
| 原文件内的版权及分发条款 | [AOSP Font.h](platforms/pianoProbePkg/Library/RamLogSerialPortLib/Font.h) | 原声明保留在文件头，不由本说明替换。 |

上表列代表文件；其他文件按各自 SPDX、版权和来源声明处理。许可正文可按 ID 查阅 [SPDX License List](https://spdx.org/licenses/) 第三方与二进制材料说明见 [THIRD_PARTY.md](THIRD_PARTY.md)。
