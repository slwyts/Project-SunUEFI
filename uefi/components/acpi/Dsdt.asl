// SPDX-License-Identifier: BSD-2-Clause-Patent
DefinitionBlock ("", "DSDT", 2, "SUNUEF", "PIANO   ", 1)
{
    Scope (\_SB)
    {
        Device (CPU0) { Name (_HID, "ACPI0007") Name (_UID, 0) }
        Device (CPU1) { Name (_HID, "ACPI0007") Name (_UID, 1) }
        Device (CPU2) { Name (_HID, "ACPI0007") Name (_UID, 2) }
        Device (CPU3) { Name (_HID, "ACPI0007") Name (_UID, 3) }
        Device (CPU4) { Name (_HID, "ACPI0007") Name (_UID, 4) }
        Device (CPU5) { Name (_HID, "ACPI0007") Name (_UID, 5) }
        Device (CPU6) { Name (_HID, "ACPI0007") Name (_UID, 6) }
        Device (CPU7) { Name (_HID, "ACPI0007") Name (_UID, 7) }

        // The render engine and its GMU share one requester namespace. The
        // display controller, GPU clock controller and SMMU have separate MMIO.
        Device (GPU0)
        {
            Name (_HID, "PRP0001")
            Name (_UID, Zero)
            Name (_HRV, 0x44050001)
            Name (_DDN, "Qualcomm Adreno 830")
            Name (_CLS, Package () { 0x03, 0x02, Zero }) // 3D, not a VGA controller
            Name (_CCA, Zero)
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package () { Package () { "compatible", "qcom,adreno-44050001" } }
            })
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x03D00000, 0x00040000) // GPU
                Memory32Fixed (ReadWrite, 0x03D6C000, 0x00024000) // GMU TCM and HFI, up to GPUCC
                Memory32Fixed (ReadWrite, 0x03D40000, 0x00010000) // GMU always-on registers
                Memory32Fixed (ReadWrite, 0x03D50000, 0x00010000) // Resource state coordinator
                Memory32Fixed (ReadWrite, 0x03D9E000, 0x00002000) // CX miscellaneous registers
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 332 } // GPU
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 112 } // CX host
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 336 } // HFI
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 337 } // GMU
            })
        }

        Device (UFS0)
        {
            Name (_HID, "QCOM24A5")
            Name (_CID, "PCI\\CC_010901")
            Name (_CLS, Package () { 0x01, 0x09, 0x01 }) // Mass storage, UFS, UFSHCI
            Name (_UID, Zero)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                // UFS HCI; the adjacent ICE window is a separate resource.
                Memory32Fixed (ReadWrite, 0x01D84000, 0x00003000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 297 }
            })
            Device (DEV0)
            {
                Name (_ADR, 8)
                Name (_RMV, Zero)
            }
        }

        // One dual-role DWC3 controller. The two roles share one interrupt
        // and cannot be instantiated as two independent physical hosts.
        Device (URS0)
        {
            Name (_HID, "QCOM0D8B")
            Name (_CID, "PNP0CA1")
            Name (_UID, Zero)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0A600000, 0x0000D93C)
            })
            Device (USB0)
            {
                Name (_ADR, Zero)
                Name (_CRS, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 165 }
                })
            }
            Device (UFN0)
            {
                Name (_ADR, One)
                Name (_CRS, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 165 }
                })
            }
        }
    }
}
