// SPDX-License-Identifier: BSD-2-Clause AND BSD-2-Clause-Patent
// Based on pakala/DSDT.aml from sunflower2333/mu_wide_platforms; see pakala/LICENSE.
// Board resources and the GPU namespace follow the Piano DT and IORT.
DefinitionBlock ("", "DSDT", 2, "SUNUEF", "PIANO   ", 1)
{
    Scope (_SB)
    {
        Name (PSUB, "MTP08750")
        Name (SOID, Zero)
        Name (STOR, One)
        Name (SIDS, "SM8750")
        Name (SIDV, Zero)
        Name (SVMJ, Zero)
        Name (SVMI, Zero)
        Name (SDFE, Zero)
        Name (SFES, "")
        Name (SIDM, Zero)
        Name (SUFS, 0xFFFFFFFF)
        Name (PUS3, One)
        Name (SUS3, 0xFFFFFFFF)
        Name (SIDT, Zero)
        Name (SOSN, Zero)
        Name (PLST, Zero)
        Name (EMUL, Zero)
        Name (SJTG, Zero)
        // No RFS/TCM or SoC-info buffer is allocated to Windows by this firmware.
        Name (RMTB, Zero)
        Name (RMTX, Zero)
        Name (RFMB, Zero)
        Name (RFMS, Zero)
        Name (RFAB, Zero)
        Name (RFAS, Zero)
        Name (TCMA, Zero)
        Name (TCML, Zero)
        Name (SOSI, Zero)
        Name (PRP0, Zero)
        Name (PRP1, Zero)
        Name (PRP2, Zero)
        Name (PRP3, Zero)
        Name (PRP4, Zero)
        Name (PRP5, Zero)
        Name (PRP6, Zero)
        Name (DPP0, Zero)
        Name (CCST, Zero)
        Name (HSFL, Zero)
        Name (QUFN, Zero)
        Name (PEPI, Zero)
        Device (TLMM)
        {
            Name (_HID, "QCOMFFEB")
            Name (_UID, Zero)
            Name (_CCA, One)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0F000000, 0x00202000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 240 }
            })

            Name (GAVL, Zero)
            OperationRegion (LREG, GeneralPurposeIO, Zero, One)
            Field (LREG, ByteAcc, NoLock, Preserve)
            {
                Connection (GpioIo (Shared, PullDefault, , , IoRestrictionInputOnly,
                                    "\\_SB.TLMM", 0, ResourceConsumer) { 80 }),
                LCOV, 1
            }

            Method (_REG, 2, NotSerialized)
            {
                If (Arg0 == 0x08) // GeneralPurposeIO
                {
                    GAVL = Arg1
                    If (Arg1) { Notify (\_SB.LID0, 0x80) }
                }
            }

            Name (_AEI, ResourceTemplate ()
            {
                GpioInt (Edge, ActiveBoth, Exclusive, PullDefault, 3000,
                         "\\_SB.TLMM", 0, ResourceConsumer) { 80 }
            })

            Method (_E50, 0, NotSerialized)
            {
                Notify (\_SB.LID0, 0x80)
            }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Name (_DEP, Package () { \_SB.TLMM })

            Method (_LID, 0, NotSerialized)
            {
                // Do not report a closed cover while its GPIO cannot be read.
                If (\_SB.TLMM.GAVL) { Return (\_SB.TLMM.LCOV) }
                Return (One)
            }
        }

        Device (GCC0)
        {
            Name (_HID, "PRP0001")
            Name (_UID, "GCC")
            Name (_DDN, "Qualcomm SM8750 global clock controller")
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package () { Package () { "compatible", "qcom,sm8750-gcc" } }
            })
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x00100000, 0x001F4200)
            })
        }

        // The display and render engines do not own the GPU clock controller.
        Device (GCLK)
        {
            Name (_HID, "PRP0001")
            Name (_UID, "GPUCC")
            Name (_DDN, "Qualcomm GPU clock controller")
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package () { Package () { "compatible", "qcom,sm8750-gpucc" } }
            })
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x03D90000, 0x00009800)
            })
        }

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
            Name (_CLS, Package () { 1, 9, 1 })
            Alias (PSUB, _SUB)
            Alias (^EMUL, EMUL)
            Name (_UID, Zero)
            Name (_CCA, One)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x01D84000, 0x00003000) // UFSHCI
                Memory32Fixed (ReadWrite, 0x01D80000, 0x00002000) // QMP PHY
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x00000129 }
            })
            Device (DEV0)
            {
                Method (_ADR, 0, NotSerialized) { Return (0x08) }
                Method (_RMV, 0, NotSerialized) { Return (Zero) }
            }
        }
        Device (SYSM)
        {
            Name (NCNT, ResourceTemplate () { Register (SystemMemory, 0, 0, 0x0, 0) })
            // A control method must return a valid package even without a PEP.
            // Keep the reference descriptions but disable each parent state.
            Method (PLPI, 1, NotSerialized)
            {
                If (PEPI) { Return (Arg0) }
                CopyObject (Arg0, Local0)
                For (Local1 = 3, Local1 < SizeOf (Local0), Local1++)
                {
                    Local2 = DerefOf (Local0 [Local1])
                    Local2 [2] = Zero
                    Local0 [Local1] = Local2
                }
                Return (Local0)
            }
            Name (CLPI, Package (0x05)
            {
                Zero,
                Zero,
                0x02,
                Package (0x0A)
                {
                    Zero,
                    Zero,
                    One,
                    Zero,
                    Zero,
                    Zero,
                    ResourceTemplate () { Register (FFixedHW, 32, 0, 0xFFFFFFFF, 3) },
                    NCNT,
                    NCNT,
                    "NCC.C1"
                },
                Package (0x0A)
                {
                    0x0258,
                    0x01F4,
                    One,
                    Zero,
                    Zero,
                    0x02,
                    ResourceTemplate () { Register (FFixedHW, 32, 0, 0x4, 3) },
                    NCNT,
                    NCNT,
                    "NCC.C4"
                }
            })

            Name (_HID, "ACPI0010")
            Name (_UID, 0x00100000)
            Method (_LPI, 0, NotSerialized)
            {
                Return (PLPI (Package (0x06)
                {
                    Zero,
                    0x02000000,
                    0x03,
                    Package (0x0A)
                    {
                        0x1D4C, 0x01F4, One, Zero, Zero, Zero, 0x0100, NCNT, NCNT, "platform.SS1"
                    },
                    Package (0x0A)
                    {
                        0x1F40, 0x0BB8, One, Zero, Zero, Zero, 0x0200, NCNT, NCNT, "platform.SS2"
                    },
                    Package (0x0A)
                    {
                        0x2328, 0x1388, One, 0x20, Zero, Zero, 0xC300, NCNT, NCNT, "platform.DRIPS"
                    }
                }))
            }
            Device (CLS0)
            {
                Name (_HID, "ACPI0010")
                Name (_UID, 0x10)
                Method (_STA, 0, NotSerialized) { Return (0x0F) }
                Method (_LPI, 0, NotSerialized)
                {
                    Return (PLPI (Package (0x05)
                    {
                        Zero, 0x01000000, 0x02,
                        Package (0x0A)
                        {
                            0x09C4, 0x01F4, One, Zero, Zero, Zero, 0x40, NCNT, NCNT, "Cluster0.CL4"
                        },
                        Package (0x0A)
                        {
                            0x1B58, 0x0FA0, One, Zero, Zero, 0x03, 0x50, NCNT, NCNT, "Cluster0.CL5"
                        }
                    }))
                }
                Device (CPU0)
                {
                    Name (_HID, "ACPI0007")
                    Name (_UID, Zero)
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Alias (^^CLPI, _LPI)
                }
                Device (CPU1)
                {
                    Name (_HID, "ACPI0007")
                    Name (_UID, One)
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Alias (^^CLPI, _LPI)
                }
                Device (CPU2)
                {
                    Name (_HID, "ACPI0007")
                    Name (_UID, 0x02)
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Alias (^^CLPI, _LPI)
                }
                Device (CPU3)
                {
                    Name (_HID, "ACPI0007")
                    Name (_UID, 0x03)
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Alias (^^CLPI, _LPI)
                }
                Device (CPU4)
                {
                    Name (_HID, "ACPI0007")
                    Name (_UID, 0x04)
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Alias (^^CLPI, _LPI)
                }
                Device (CPU5)
                {
                    Name (_HID, "ACPI0007")
                    Name (_UID, 0x05)
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Alias (^^CLPI, _LPI)
                }
            }
            Device (CLS1)
            {
                Name (_HID, "ACPI0010")
                Name (_UID, 0x11)
                Method (_STA, 0, NotSerialized) { Return (0x0F) }
                Method (_LPI, 0, NotSerialized)
                {
                    Return (PLPI (Package (0x05)
                    {
                        Zero, 0x01000000, 0x02,
                        Package (0x0A)
                        {
                            0x09C4, 0x01F4, One, Zero, Zero, Zero, 0x40, NCNT, NCNT, "Cluster1.CL4"
                        },
                        Package (0x0A)
                        {
                            0x1B58, 0x0FA0, One, Zero, Zero, 0x03, 0x50, NCNT, NCNT, "Cluster1.CL5"
                        }
                    }))
                }
                Device (CPU0)
                {
                    Name (_HID, "ACPI0007")
                    Name (_UID, 0x06)
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Alias (^^CLPI, _LPI)
                }
                Device (CPU1)
                {
                    Name (_HID, "ACPI0007")
                    Name (_UID, 0x07)
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Alias (^^CLPI, _LPI)
                }
            }
        }
        Device (URS0)
        {
            Name (PLOC, Package (0x01)
            {
                ToPLD (
                    PLD_Revision           = 0x2,
                    PLD_IgnoreColor        = 0x1,
                    PLD_Red                = 0x0,
                    PLD_Green              = 0x0,
                    PLD_Blue               = 0x0,
                    PLD_Width              = 0x0,
                    PLD_Height             = 0x0,
                    PLD_UserVisible        = 0x1,
                    PLD_Dock               = 0x0,
                    PLD_Lid                = 0x0,
                    PLD_Panel              = "BACK",
                    PLD_VerticalPosition   = "CENTER",
                    PLD_HorizontalPosition = "LEFT",
                    PLD_Shape              = "VERTICALRECTANGLE",
                    PLD_GroupOrientation   = 0x0,
                    PLD_GroupToken         = 0x0,
                    PLD_GroupPosition      = 0x0,
                    PLD_Bay                = 0x0,
                    PLD_Ejectable          = 0x0,
                    PLD_EjectRequired      = 0x0,
                    PLD_CabinetNumber      = 0x0,
                    PLD_CardCageNumber     = 0x0,
                    PLD_Reference          = 0x0,
                    PLD_Rotation           = 0x0,
                    PLD_Order              = 0x0,
                    PLD_VerticalOffset     = 0xFFFF,
                    PLD_HorizontalOffset   = 0xFFFF)
            })

            Name (PCAP, Package (0x04)
            {
                One,
                0x09,
                Zero,
                Zero
            })

            Name (_HID, "QCOM0D8B")
            Name (_CID, Package (0x02)
            {
                "PNP0CA1",
                "QCOMFFE1"
            })
            Alias (PSUB, _SUB)
            Name (_UID, Zero)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0A600000, 0x00100000)
            })
            Device (USB0)
            {
                Name (_ADR, Zero)
                Name (_S0W, 0x03)
                Name (_CRS, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 0x000000A5 }
                    Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake) { 0x000000A2 }
                    Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake) { 0x0000031A }
                    Interrupt (ResourceConsumer, Edge, ActiveHigh, SharedAndWake) { 0x00000317 }
                    Interrupt (ResourceConsumer, Edge, ActiveHigh, SharedAndWake) { 0x00000318 }
                })
                Device (RHUB)
                {
                    Name (_ADR, Zero)
                    Device (PRT1)
                    {
                        Name (_ADR, One)
                        Alias (^^^PCAP, _UPC)
                        Alias (^^^PLOC, _PLD)
                    }
                }
                Method (_STA, 0, NotSerialized) { Return (0x0F) }
                Method (DPM0, 1, NotSerialized)
                {
                    DPP0 = Arg0
                }
                Method (CCVL, 0, NotSerialized) { Return (CCST) }
                Method (HSEN, 0, NotSerialized) { Return (HSFL) }
                Method (_DSM, 4, Serialized)
                {
                    Switch (ToBuffer (Arg0))
                    {
                        Case (ToUUID ("ce2ee385-00e6-48cb-9f05-2edb927c4899"))
                        {
                            Switch (ToInteger (Arg2))
                            {
                                Case (Zero)
                                {
                                    Switch (ToInteger (Arg1))
                                    {
                                        Case (Zero)
                                        {
                                            Return (Buffer (One) { 0x1D })
                                        }
                                        Default
                                        {
                                            Return (Buffer (One) { 0x01 })
                                        }
                                    }
                                }
                                Case (0x02)
                                {
                                    Return (Zero)
                                }
                                Case (0x03)
                                {
                                    Return (Zero)
                                }
                                Case (0x04)
                                {
                                    Return (0x02)
                                }
                                Default
                                {
                                    Return (Buffer (One) { 0x00 })
                                }
                            }
                        }
                        Default
                        {
                            Return (Buffer (One) { 0x00 })
                        }
                    }
                }
                Method (PHYC, 0, NotSerialized)
                {
                    Return (Package () {})
                }
            }
            Device (UFN0)
            {
                Name (_ADR, One)
                Name (_S0W, 0x03)
                Device (RHUB)
                {
                    Name (_ADR, Zero)
                    Device (PRT1)
                    {
                        Name (_ADR, One)
                        Alias (^^^PCAP, _UPC)
                        Alias (^^^PLOC, _PLD)
                    }
                }
                Name (_CRS, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 0x000000A5 }
                    Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake) { 0x000000A2 }
                })
                Method (CCVL, 0, NotSerialized) { Return (CCST) }
                Method (_DSM, 4, Serialized)
                {
                    Switch (ToBuffer (Arg0))
                    {
                        Case (ToUUID ("fe56cfeb-49d5-4378-a8a2-2978dbe54ad2"))
                        {
                            Switch (ToInteger (Arg2))
                            {
                                Case (Zero)
                                {
                                    Switch (ToInteger (Arg1))
                                    {
                                        Case (Zero)
                                        {
                                            Return (Buffer (One) { 0x03 })
                                        }
                                        Default
                                        {
                                            Return (Buffer (One) { 0x01 })
                                        }
                                    }
                                }
                                Case (One)
                                {
                                    Return (0x20)
                                }
                                Default
                                {
                                    Return (Buffer (One) { 0x00 })
                                }
                            }
                        }
                        Case (ToUUID ("18de299f-9476-4fc9-b43b-8aeb713ed751"))
                        {
                            Switch (ToInteger (Arg2))
                            {
                                Case (Zero)
                                {
                                    Switch (ToInteger (Arg1))
                                    {
                                        Case (Zero)
                                        {
                                            Return (Buffer (One) { 0x03 })
                                        }
                                        Default
                                        {
                                            Return (Buffer (One) { 0x01 })
                                        }
                                    }
                                }
                                Case (One)
                                {
                                    Return (0x39)
                                }
                                Default
                                {
                                    Return (Buffer (One) { 0x00 })
                                }
                            }
                        }
                        Default
                        {
                            Return (Buffer (One) { 0x00 })
                        }
                    }
                }
                Method (PHYC, 0, NotSerialized)
                {
                    Return (Package () {})
                }
            }
        }
    }
}
