"""Provenance/build-gate tests; compiler and Git commands are always mocked."""

import contextlib
import hashlib
import importlib.util
import io
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("test_kernel_builder", ROOT / "tools/build_kernel.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


def make_efi_image():
    """Synthetic ARM64 Linux header with a bounded PE32+ executable section."""
    data = bytearray(1024)
    data[:2] = b"MZ"
    data[56:60] = b"ARM\x64"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 68, 0xaa64, 1, 0, 0, 0, 112, 0x2022)
    struct.pack_into("<H", data, 88, 0x20b)
    struct.pack_into("<II", data, 88 + 16, 0x1000, 0x1000)
    struct.pack_into("<II", data, 88 + 32, 4096, 512)
    struct.pack_into("<II", data, 88 + 56, 8192, 512)
    struct.pack_into("<H", data, 88 + 68, 10)
    struct.pack_into("<I", data, 88 + 108, 0)
    struct.pack_into("<8sIIIIIIHHI", data, 200, b".text\0\0\0", 512, 0x1000,
                     512, 512, 0, 0, 0, 0, 0x60000020)
    data[512:516] = b"\xc0\x03\x5f\xd6"
    return bytes(data)


class ImageHeaderTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.image = Path(temporary.name) / "Image"

    def info(self, data):
        self.image.write_bytes(data)
        return builder.image_info(self.image)

    def test_valid_section_exactly_at_end_of_file(self):
        data = make_efi_image()
        info = self.info(data)
        self.assertTrue(info["efi_stub"])
        self.assertEqual(len(data), info["bytes"])
        self.assertEqual(hashlib.sha256(data).hexdigest(), info["sha256"])

    def test_non_efi_arm64_image_is_not_mislabelled(self):
        data = bytearray(64)
        data[56:60] = b"ARM\x64"
        self.assertFalse(self.info(data)["efi_stub"])

    def test_truncated_wrong_architecture_and_bad_pe_offsets(self):
        cases = [make_efi_image()[:63], bytes(64), make_efi_image()[:70]]
        for offset in (63, 0xfffffff0):
            data = bytearray(make_efi_image())
            struct.pack_into("<I", data, 60, offset)
            cases.append(data)
        data = bytearray(make_efi_image())
        struct.pack_into("<H", data, 68, 0x8664)
        cases.append(data)
        data = bytearray(make_efi_image())
        data[64:68] = b"NOPE"
        cases.append(data)
        for data in cases:
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                self.info(data)

    def test_optional_directory_and_section_bounds_are_enforced(self):
        mutations = [(84, "<H", 111), (88, "<H", 0x10b), (196, "<I", 1),
                     (196, "<I", 17), (70, "<H", 0), (70, "<H", 97),
                     (216, "<I", 513), (220, "<I", 239), (220, "<I", 1024)]
        for offset, fmt, value in mutations:
            data = bytearray(make_efi_image())
            struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                self.info(data)


class BuildGateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.pin = "a" * 40
        self.base = "b" * 40
        self.resolved_pin = self.pin
        self.work_head = self.pin
        self.dirty = ""
        self.commands = []
        self.drift_stage = None
        self.drift_fragment = "piano-ram.config"
        self.fail_image = False
        self.config = 'CONFIG_EFI=y\nCONFIG_BLK_DEV_INITRD=y\n# CONFIG_BLOCK is not set\n# CONFIG_CMDLINE_FORCE is not set\n'
        (self.root / "tools").mkdir()
        (self.root / "kernel-profiles.json").write_text(json.dumps({"repository_path": "repo", "profiles": {
            "stable": {"commit": self.pin, "branch": "piano-stable", "base_commit": self.base,
                       "base_config": "piano_defconfig"}}}))
        (self.root / "repo").mkdir()
        self.work = self.root / "build/kernel-worktrees" / ("stable-" + self.pin[:12])
        self.work.mkdir(parents=True)
        for name in ("piano-ram.config", "piano-userspace-debug.config"):
            path = self.root / "configs/linux" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# fixture fragment " + name + "\n")

    def fake_output(self, argv, cwd=None):
        args = [str(value) for value in argv]
        if args[:2] == ["git", "rev-parse"]:
            return self.work_head if args[-1] == "HEAD" else self.resolved_pin
        if args[:2] == ["git", "status"]:
            return self.dirty
        if args[:2] == ["git", "rev-list"]:
            return self.pin
        if args[-1] == "--version":
            return "clang version fixture-only"
        self.fail("Unexpected command: " + repr(args))

    def fake_run(self, argv, **kwargs):
        args = [str(value) for value in argv]
        self.commands.append(args)
        if args[0] == "bash":
            return
        self.assertEqual("make", args[0], "Tests must never execute real build/Git commands")
        out = Path(next(value[2:] for value in args if value.startswith("O=")))
        if args[-1] == "piano_defconfig":
            (out / ".config").write_text(self.config)
        if self.drift_stage and self.drift_stage in args:
            with (self.root / "configs/linux" / self.drift_fragment).open("a") as stream:
                stream.write("# changed during operation\n")
        if "Image" in args:
            if self.fail_image:
                raise subprocess.CalledProcessError(2, args)
            image = out / "arch/arm64/boot/Image"
            image.parent.mkdir(parents=True, exist_ok=True)
            image.write_bytes(make_efi_image())
            (out / "System.map").write_text("fixture System.map\n")
            release = out / "include/config/kernel.release"
            release.parent.mkdir(parents=True, exist_ok=True)
            release.write_text("fixture-piano\n")

    def invoke(self, *args):
        with patch.object(builder, "__file__", str(self.root / "tools/build_kernel.py")), \
             patch.object(builder, "output", side_effect=self.fake_output), \
             patch.object(builder, "run", side_effect=self.fake_run), \
             patch.object(builder.shutil, "which", return_value="/fixture/clang"), \
             patch.object(sys, "argv", ["build_kernel.py", "--profile", "stable", *args]), \
             contextlib.redirect_stdout(io.StringIO()):
            builder.main()

    def manifest_path(self, mode="ram"):
        return self.root / "artifacts/kernels/stable" / mode / "manifest.json"

    def test_wrong_source_pin_or_worktree_is_rejected_before_make(self):
        self.resolved_pin = "c" * 40
        with self.assertRaisesRegex(SystemExit, "Pinned source"):
            self.invoke()
        self.assertEqual([], self.commands)
        self.resolved_pin = self.pin
        self.work_head = "d" * 40
        with self.assertRaisesRegex(SystemExit, "another commit"):
            self.invoke()
        self.assertEqual([], self.commands)

    def test_dirty_worktree_is_not_reported_clean(self):
        self.dirty = " M drivers/example.c"
        with self.assertRaisesRegex(SystemExit, "dirty"):
            self.invoke()
        self.assertEqual([], self.commands)

    def test_downstream_cmdline_efi_and_storage_config_gates(self):
        for config in ('CONFIG_EFI=y\nCONFIG_CMDLINE_FORCE=y\n', '# CONFIG_EFI is not set\n',
                       'CONFIG_EFI=y\nCONFIG_BLOCK=y\n'):
            self.config = config
            with self.subTest(config=config), self.assertRaises(SystemExit):
                self.invoke("--configure-only")
            self.assertFalse(self.manifest_path().exists())

    def test_configure_only_drift_is_rejected_before_manifest_publication(self):
        self.drift_stage = "olddefconfig"
        with self.assertRaisesRegex(SystemExit, "changed during configuration"):
            self.invoke("--configure-only")
        self.assertFalse(self.manifest_path().exists())

    def test_second_fragment_drift_during_image_build_is_rejected(self):
        self.drift_stage = "Image"
        self.drift_fragment = "piano-userspace-debug.config"
        with self.assertRaisesRegex(SystemExit, "changed during the build"):
            self.invoke("--mode", "userspace-debug")
        self.assertFalse(self.manifest_path("userspace-debug").exists())

    def test_configure_only_has_no_image_and_records_both_fragments(self):
        self.invoke("--mode", "userspace-debug", "--configure-only")
        result = json.loads(self.manifest_path("userspace-debug").read_text())
        self.assertEqual("CONFIGURED_NOT_BUILT", result["status"])
        self.assertEqual(2, len(result["fragments"]))
        self.assertEqual(self.pin, result["source_commit"])
        self.assertNotIn("image", result)
        self.assertFalse(any("Image" in argv for argv in self.commands))
        self.assertFalse(result["hardware_verified"])

    def test_failed_build_invalidates_old_manifest_and_no_implicit_dtb(self):
        old = self.manifest_path()
        old.parent.mkdir(parents=True)
        old.write_text('{"status":"old"}')
        self.fail_image = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke()
        self.assertFalse(old.exists())
        self.fail_image = False
        self.invoke()
        result = json.loads(old.read_text())
        self.assertIsNone(result["dtb"])
        self.assertEqual("HOST_BUILT_NOT_HARDWARE_VERIFIED", result["status"])
        self.assertFalse(result["hardware_verified"])


if __name__ == "__main__":
    unittest.main()
