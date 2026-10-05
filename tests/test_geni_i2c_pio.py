#!/usr/bin/env python3
"""Compile actual default-off/on PIO source against real Mu types, host only."""
from pathlib import Path
import subprocess
import hashlib
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"

class GeniPioTest(unittest.TestCase):
    def test_pinned_reference_facts(self):
        sys.path.insert(0, str(ROOT / "tools"))
        import audit_pogo_i2c as native
        data = (ROOT / "upstream/Mu-Silicium/Binaries/piano/Bringup/I2C/I2C.efi").read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), native.NATIVE_SHA)
        sections = native.pe_sections(data)
        # Actual ARM64 FW_REV read/mask and the native loader call, not an ABI cast.
        for rva, instruction in ((0x7478, 0xb940692a), (0x747c, 0x12181d4b),
                                 (0x749c, 0x53087d49), (0x73c4, 0x97ffff1b)):
            self.assertEqual(struct.unpack("<I", native.rva_bytes(data, sections, rva, 4, True))[0], instruction)
        facts = native.dt_facts((ROOT / "private/captures/2026-10-03-piano/live.dtb").read_bytes())
        self.assertEqual((facts["se_base"], facts["se_bytes"], facts["slave"]), (0xa98000, 0x4000, 0x4c))
        self.assertFalse(facts["native_gpio_clock_supply_contract_verified"])
        pins = {
            "drivers/i2c/busses/i2c-qcom-geni.c": "f96dbf7ac597ad05adfe747fbff71a6e391abbd8c6074b9011458dc6497185a1",
            "include/linux/soc/qcom/geni-se.h": "eb36836c91becdd8494a968cdaa76a2c697b4b84d201c529b29b17b2bb84198b",
            "drivers/soc/qcom/qcom-geni-se.c": "249348128bcc6d6431bc4c7e37098c3c9b3f7035d6c608634200f87861c1699e",
        }
        for name, expected in pins.items():
            self.assertEqual(hashlib.sha256((ROOT / "kernels/linux-piano" / name).read_bytes()).hexdigest(), expected)

    def test_actual_source(self):
        with tempfile.TemporaryDirectory(prefix="piano-geni-pio-") as out:
            for enabled in (0, 1):
                executable = str(Path(out) / f"pio-{enabled}")
                subprocess.run(["cc", "-std=gnu11", "-fshort-wchar", "-g", "-O1",
                    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    f"-DPIANO_GENI_I2C_PIO_EXPERIMENT={enabled}",
                    "-I", str(INC), "-I", str(INC / "X64"),
                    str(ROOT / "tests/PianoGeniI2cPioTest.c"), "-o", executable], check=True)
                subprocess.run([executable], check=True)
            subprocess.run([str(ROOT / "build/host-tools/usr/bin/clang"),
                "--target=aarch64-windows-msvc", "-fshort-wchar", "-ffreestanding",
                "-fsyntax-only", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-DPIANO_GENI_I2C_PIO_EXPERIMENT=1", "-I", str(INC), "-I", str(INC / "AArch64"),
                str(ROOT / "bootprofiles/uefi-app/PianoGeniI2cPio.c")], check=True)

if __name__ == "__main__":
    unittest.main()
