/** @file
  Piano processor namespace, with processor IDs matching the MADT.

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
        }

        Device (C001)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 1)
        }

        Device (C002)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 2)
        }

        Device (C003)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 3)
        }

        Device (C004)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 4)
        }

        Device (C005)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 5)
        }

        Device (C006)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 6)
        }

        Device (C007)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 7)
        }
    }
}
