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
