#!/usr/bin/env python3
"""NT36532 image parser against the pinned touch firmware and the driver's rules"""
import platform
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'
INC = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
FIRMWARE = ROOT / "upstream/piano-firmware-current/firmware/novatek"
IMAGES = [FIRMWARE / "novatek_nt36532_piano_fw_csot.bin", FIRMWARE / "novatek_nt36532_piano_fw_boe.bin"]


def crc32(data, length):
    value = 0xFFFFFFFF
    for index in range((length + 4) // 4):
        value ^= struct.unpack_from('<I', data, index * 4)[0]
        for _ in range(32):
            value = ((value << 1) ^ 0x04C11DB7 if value & 0x80000000 else value << 1) & 0xFFFFFFFF
    return value


def partitions(data):
    """nvt_bin_header_parser for a hardware-CRC chip"""
    word = lambda offset: struct.unpack_from('<I', data, offset)[0]
    end, cascade = word(0), (data[0x20] >> 1) & 1
    info, pos = 0, 0x30
    while pos < (end // 2 if cascade else end):
        info, pos = info + 1, pos + 0x10
    info += cascade
    overlay = data[0x28] & 0x0F if data[0x28] & 0x10 else 0
    table, found = [], False
    for index in range(2 + overlay + info):
        if index < 2:
            binary, sram, size = word(index * 12), word(4 + index * 12), word(8 + index * 12)
            table.append((binary, sram, size, crc32(data[binary:], size)))
            continue
        if index < 2 + info:
            if not found:
                pos = 0x30 + 0x10 * (index - 2)
            elif cascade:
                pos = end - 0x10
        else:
            pos = table[1][0] + 0x10 * (index - 2 - info)
        sram, size, binary, crc = word(pos), word(pos + 4), word(pos + 8), word(pos + 12)
        if index < 2 + info and binary < end and size:
            found = True
        table.append((binary, sram, size, crc))
    return table


class TouchNvtTest(unittest.TestCase):
    def test_parser_matches_driver_rules(self):
        images = [image for image in IMAGES if image.is_file()]
        with tempfile.TemporaryDirectory(prefix="piano-touch-nvt-") as out:
            executable = str(Path(out) / "nvt")
            subprocess.run(["cc", "-std=gnu11", "-fshort-wchar", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            "-fsanitize=address,undefined", "-I", str(INC), "-I", str(INC / HOST_PROCESSOR_BIND),
                            "-I", str(ROOT / "uefi/components/touch-product"),
                            str(ROOT / "tests/native/PianoTouchNvtTest.c"),
                            str(ROOT / "uefi/components/touch-product/PianoTouchNvt.c"), "-o", executable], check=True)
            output = subprocess.run([executable, *map(str, images)], check=True, capture_output=True, text=True).stdout
        if not images:
            self.skipTest("piano-firmware checkout is not present")
        blocks = output.split("image ")[1:]
        self.assertEqual(len(blocks), len(images))
        for image, block in zip(images, blocks):
            lines = block.splitlines()
            self.assertIn("cascade=1", lines[0])
            parsed = [tuple(int(field.split('=')[1], 16) for field in line.split()[2:]) for line in lines[1:]]
            self.assertEqual(parsed, partitions(image.read_bytes()))
            self.assertEqual(len(parsed), 16)

    def test_product_embeds_both_panels(self):
        import sys
        sys.path.insert(0, str(ROOT / "tools"))
        import prepare_product
        if not all(image.is_file() for image in IMAGES):
            self.skipTest("piano-firmware checkout is not present")
        with tempfile.TemporaryDirectory(prefix="piano-touch-fw-") as out:
            record = prepare_product.prepare_touch_firmware(ROOT, Path(out))
            header = (Path(out) / "PianoTouchFirmware.h").read_text()
        for panel in ("csot", "boe"):
            self.assertIn(record[panel]["sha256"], header)
            self.assertIn("mPianoTouchFirmware" + panel.capitalize() + "[]", header)


if __name__ == "__main__":
    unittest.main()
