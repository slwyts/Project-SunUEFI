/** @file
  DSDT for Piano (SM8750).

  Describes the eight processors, the UFS host controller and the xHCI
  controller. Register windows and interrupts come from
  vendor/piano/dtb/piano.dtb. UFS uses the QCOM24A5 hardware ID from
  Qualcomm's reference ACPI; the xHCI controller uses the Microsoft
  compatible ID PNP0D10, the documented standard for a plain xHCI device.
  No _PR0/_PR3, _DSD or GPIO resources: the firmware leaves these devices
  clocked and powered when it hands off to Windows.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

DefinitionBlock ("Dsdt.aml", "DSDT", 2, "SUNUEF", "PIANO", 1)
{
    Scope (\_SB)
    {
        Device (C000)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0)
            Name (_STA, 0x0F)
        }

        Device (C001)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 1)
            Name (_STA, 0x0F)
        }

        Device (C002)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 2)
            Name (_STA, 0x0F)
        }

        Device (C003)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 3)
            Name (_STA, 0x0F)
        }

        Device (C004)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 4)
            Name (_STA, 0x0F)
        }

        Device (C005)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 5)
            Name (_STA, 0x0F)
        }

        Device (C006)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 6)
            Name (_STA, 0x0F)
        }

        Device (C007)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 7)
            Name (_STA, 0x0F)
        }

        Device (UFS0)
        {
            Name (_HID, "QCOM24A5")
            Name (_UID, 0)
            Name (_CCA, 1)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x01D84000, 0x0001C000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 297 }
            })
        }

        Device (XHC0)
        {
            Name (_HID, "PNP0D10")
            Name (_UID, 0)
            Name (_CCA, 1)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0A600000, 0x00100000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 165 }
            })
        }
    }
}
