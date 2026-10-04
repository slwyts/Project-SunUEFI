"""Exercise package input gates and the v2 binary contract in temporary dirs."""

import contextlib
import hashlib
import importlib.util
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from test_build_kernel import make_efi_image


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("test_kernel_payload_packager", ROOT / "tools/package_kernel_payload.py")
packager = importlib.util.module_from_spec(SPEC)
with patch.object(sys, "path", [str(ROOT / "tools"), *sys.path]):
    SPEC.loader.exec_module(packager)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def check_v2_contract(test, payload):
    """Independent fixed-offset contract checks, not the packer's format string."""
    test.assertGreaterEqual(len(payload), 144)
    test.assertEqual(b"SUNUEFI-LINUXv2\0", payload[:16])
    test.assertEqual(2, int.from_bytes(payload[16:20], "little"))
    test.assertEqual(144, int.from_bytes(payload[20:24], "little"))
    sizes = [int.from_bytes(payload[start:start + 8], "little") for start in (24, 32, 104)]
    test.assertTrue(all(sizes))
    test.assertEqual(len(payload), 144 + sum(sizes))
    cursor = 144
    parts = []
    for size, hash_offset in zip(sizes, (40, 72, 112)):
        test.assertLessEqual(cursor + size, len(payload))
        part = payload[cursor:cursor + size]
        test.assertEqual(hashlib.sha256(part).digest(), payload[hash_offset:hash_offset + 32])
        parts.append(part)
        cursor += size
    return parts


class PayloadPackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "tools").mkdir()
        self.source = self.root / "artifacts/kernels/stable/ram"
        self.source.mkdir(parents=True)
        self.pin = "a" * 40
        self.kernel = make_efi_image()
        self.initrd = b"synthetic initramfs fixture"
        self.config = b"CONFIG_EFI=y\n# CONFIG_BLOCK is not set\n"
        (self.source / "Image").write_bytes(self.kernel)
        (self.source / "initramfs.cpio.gz").write_bytes(self.initrd)
        (self.source / "config").write_bytes(self.config)
        self.metadata = {"profile": "stable", "mode": "ram", "source_commit": self.pin,
                         "status": "HOST_BUILT_NOT_HARDWARE_VERIFIED", "source_dirty": False,
                         "image": {"sha256": sha(self.kernel)}, "config_sha256": sha(self.config)}
        self.init_meta = {"profile": "stable", "mode": "ram", "source_commit": self.pin,
                          "kernel_image": {"sha256": sha(self.kernel)},
                          "initramfs": {"sha256": sha(self.initrd)}}
        (self.root / "kernel-profiles.json").write_text(json.dumps({"profiles": {"stable": {"commit": self.pin}}}))
        self.dtb = self.root / "explicit-board.dtb"
        structure = struct.pack(">I", 1) + b"\0" * 4 + struct.pack(">II", 2, 9)
        self.dtb.write_bytes(struct.pack(">10I", 0xd00dfeed, 72, 56, 72, 40, 17, 16, 0, 0, 16)
                             + bytes(16) + structure)

    def invoke(self):
        (self.source / "manifest.json").write_text(json.dumps(self.metadata))
        (self.source / "initramfs-manifest.json").write_text(json.dumps(self.init_meta))
        with patch.object(packager, "__file__", str(self.root / "tools/package_kernel_payload.py")), \
             patch.object(sys, "argv", ["package_kernel_payload.py", "--profile", "stable", "--dtb", str(self.dtb)]), \
             contextlib.redirect_stdout(io.StringIO()):
            packager.main()

    def output(self):
        return self.root / "artifacts/linux-ram/linux-payload.bin"

    def test_v2_layout_offsets_hashes_and_explicit_dtb_round_trip(self):
        self.invoke()
        payload = self.output().read_bytes()
        self.assertEqual([self.kernel, self.initrd, self.dtb.read_bytes()], check_v2_contract(self, payload))
        result = json.loads(self.output().with_name("payload-manifest.json").read_text())
        self.assertEqual(sha(payload), result["payload_sha256"])
        self.assertEqual(len(payload), result["payload_bytes"])
        self.assertEqual(str(self.dtb.resolve()), result["dtb_source"])
        self.assertFalse(result["dtb_board_topology_verified"])
        self.assertFalse(result["runtime_dtb_from_abl"])
        self.assertFalse(result["storage_drivers_allowed"])
        self.assertFalse(result["hardware_verified"])

    def test_post_package_truncation_length_and_hash_corruption_violate_contract(self):
        self.invoke()
        original = self.output().read_bytes()
        cases = [original[:143], original[:-1], original + b"unexpected trailing data"]
        for offset in (24, 32, 104):
            data = bytearray(original)
            data[offset:offset + 8] = (0xffffffffffffffff).to_bytes(8, "little")
            cases.append(data)
        for offset in (40, 72, 112, 144, len(original) - 1):
            data = bytearray(original)
            data[offset] ^= 1
            cases.append(data)
        for data in cases:
            with self.subTest(length=len(data)), self.assertRaises(AssertionError):
                check_v2_contract(self, data)

    def test_self_consistent_wrong_pin_profile_or_mode_is_rejected(self):
        for field, value in (("source_commit", "c" * 40), ("profile", "next"), ("mode", "userspace-debug")):
            old = self.metadata[field]
            self.metadata[field] = value
            self.init_meta[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(SystemExit, "pinned commit"):
                self.invoke()
            self.metadata[field] = old
            self.init_meta[field] = old
        self.assertFalse(self.output().exists())

    def test_incomplete_or_dirty_kernel_build_is_rejected(self):
        self.metadata["status"] = "CONFIGURED_NOT_BUILT"
        with self.assertRaisesRegex(SystemExit, "clean completed build"):
            self.invoke()
        self.metadata["status"] = "HOST_BUILT_NOT_HARDWARE_VERIFIED"
        self.metadata["source_dirty"] = True
        with self.assertRaisesRegex(SystemExit, "clean completed build"):
            self.invoke()

    def test_image_config_and_initramfs_drift_are_rejected(self):
        for filename, expected in (("Image", "manifest/image"), ("config", "config/manifest"),
                                   ("initramfs.cpio.gz", "Initramfs manifest")):
            path = self.source / filename
            original = path.read_bytes()
            path.write_bytes(original + b"changed after build")
            with self.subTest(filename=filename), self.assertRaisesRegex(SystemExit, expected):
                self.invoke()
            path.write_bytes(original)
        self.assertFalse(self.output().exists())

    def test_matching_hash_cannot_make_a_non_efi_image_bootable(self):
        kernel = bytearray(64)
        kernel[56:60] = b"ARM\x64"
        (self.source / "Image").write_bytes(kernel)
        self.metadata["image"]["sha256"] = sha(kernel)
        self.init_meta["kernel_image"]["sha256"] = sha(kernel)
        with self.assertRaisesRegex(SystemExit, "manifest/image"):
            self.invoke()

    def test_initramfs_commit_profile_mode_and_kernel_image_provenance(self):
        mutations = [("source_commit", "b" * 40, "commit mismatch"),
                     ("profile", "next", "profile/mode"), ("mode", "userspace-debug", "profile/mode")]
        for field, value, message in mutations:
            old = self.init_meta[field]
            self.init_meta[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(SystemExit, message):
                self.invoke()
            self.init_meta[field] = old
        self.init_meta["kernel_image"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(SystemExit, "provenance/image"):
            self.invoke()

    def test_nested_initramfs_hash_is_required_and_legacy_fallback_is_rejected(self):
        self.init_meta["sha256"] = sha(self.initrd)
        self.init_meta["compressed_sha256"] = sha(self.initrd)
        self.init_meta["initramfs"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(SystemExit, "Initramfs manifest"):
            self.invoke()
        del self.init_meta["initramfs"]
        with self.assertRaisesRegex(SystemExit, "Initramfs manifest"):
            self.invoke()

    def test_truncated_non_fdt_and_wrong_total_size_are_rejected(self):
        good = self.dtb.read_bytes()
        wrong_size = bytearray(good)
        struct.pack_into(">I", wrong_size, 4, len(good) - 1)
        for data in (good[:39], bytes(72), wrong_size, good + b"unaccounted bytes"):
            self.dtb.write_bytes(data)
            with self.subTest(length=len(data)), self.assertRaisesRegex(SystemExit, "complete explicitly selected"):
                self.invoke()
        self.assertFalse(self.output().exists())


if __name__ == "__main__":
    unittest.main()
