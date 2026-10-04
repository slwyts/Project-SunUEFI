"""Check the actual archive and provenance gates without executing PID 1."""

import gzip
import importlib.util
import json
import os
import re
import shutil
import signal
import stat
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("kernel_initramfs", ROOT / "tools/make_kernel_initramfs.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


def static_elf(dynamic_needed=False):
    phcount = 2 if dynamic_needed else 1
    size = 64 + phcount * 56 + 32
    data = bytearray(size)
    data[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", data, 16,
                     2, 183, 1, 0x400000 + size - 16, 64, 0, 0, 64, 56, phcount, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0x400000, 0x400000, size, size, 4096)
    if dynamic_needed:
        struct.pack_into("<IIQQQQQQ", data, 120, 2, 4, size - 32, 0x400000 + size - 32, 0, 32, 32, 8)
        struct.pack_into("<qQqQ", data, size - 32, 1, 4, 0, 0)
    return bytes(data)


class KernelInitramfsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.kernel_dir = self.root / "kernel"
        self.kernel_dir.mkdir()
        image = b"synthetic Image for archive/provenance testing"
        config = b"\n".join([
            b"CONFIG_BLK_DEV_INITRD=y", b"CONFIG_RD_GZIP=y", b"CONFIG_PSTORE=y",
            b"CONFIG_PSTORE_CONSOLE=y", b"CONFIG_PSTORE_RAM=y", b"# CONFIG_BLOCK is not set",
            b'CONFIG_CMDLINE=""', b"# CONFIG_CMDLINE_FORCE is not set", b"",
        ])
        (self.kernel_dir / "Image").write_bytes(image)
        (self.kernel_dir / "config").write_bytes(config)
        self.manifest = self.kernel_dir / "manifest.json"
        self.manifest.write_text(json.dumps({"profile": "stable", "mode": "ram", "source_commit": "a" * 40,
            "kernel_release": "test-piano", "config_sha256": tool.digest(config),
            "image": {"bytes": len(image), "sha256": tool.digest(image)}}))
        self.busybox = self.root / "busybox"
        self.busybox.write_bytes(static_elf())
        self.provenance = self.root / "busybox-source.json"
        self.provenance.write_text(json.dumps({"repository": "https://example.invalid/test-only",
                                              "package": {"P": "busybox-static", "A": "aarch64", "V": "fixture"}}))
        self.init = ROOT / "bootprofiles/kernel-ram/init"
        self.output = self.root / "output"

    def generate(self, profile="stable"):
        return tool.generate(profile, self.manifest, self.busybox, self.provenance, self.init, self.output)

    def test_archive_has_only_new_ram_init_and_no_modules_or_disk_mounts(self):
        result = self.generate()
        archive = gzip.decompress((self.output / "initramfs.cpio.gz").read_bytes())
        rows = {record["name"]: record for record in tool.inspect_newc(archive)}
        self.assertFalse(any(name.startswith("lib/") or ".ko" in name for name in rows))
        self.assertEqual(static_elf(), rows["bin/busybox"]["data"])
        self.assertTrue(stat.S_ISREG(rows["init"]["mode"]))
        self.assertEqual(0o755, stat.S_IMODE(rows["init"]["mode"]))
        init = rows["init"]["data"].decode()
        self.assertEqual(self.init.read_text(), init)
        self.assertEqual(["devtmpfs", "proc", "sysfs", "tmpfs"], re.findall(r"(?m)^mount -t (\w+)", init))
        self.assertIsNone(re.search(r"\b(insmod|modprobe|depmod|devmem|switch_root|userdata|beaconinit|pianoinit)\b", init))
        self.assertIn("sleep 180", init)
        self.assertIn("reboot -f", init)
        self.assertIn(b"source_commit=" + b"a" * 40, rows["etc/piano/kernel-build"]["data"])
        self.assertFalse(result["hardware_verified"])
        self.assertEqual(0, result["modules"])
        self.assertEqual(tool.digest(archive), result["initramfs"]["uncompressed_sha256"])
        cpio = shutil.which("cpio")
        if cpio:
            independent = subprocess.run([cpio, "--list", "--quiet"], input=archive, capture_output=True, check=True)
            self.assertEqual(set(rows), set(independent.stdout.decode().splitlines()))

    def test_generation_is_reproducible(self):
        first = self.generate()["initramfs"]["sha256"]
        self.assertEqual(first, self.generate()["initramfs"]["sha256"])

    def test_foreign_profile_and_stale_image_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "profile/mode"):
            self.generate("next")
        (self.kernel_dir / "Image").write_bytes(b"changed kernel")
        with self.assertRaisesRegex(ValueError, "Image does not match"):
            self.generate()
        self.assertFalse(self.output.exists())

    def test_unsafe_cpio_paths_and_symlinks_are_rejected(self):
        for name in ("../escape", "/init", "bin//sh", "bin/./sh", "bad\\path", "a\0b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                tool.make_newc([{"name": name, "mode": stat.S_IFREG | 0o644}])
        with self.assertRaises(ValueError):
            tool.make_newc([{"name": "bin/sh", "mode": stat.S_IFLNK | 0o777, "data": b"../../host"}])

    def test_non_arm64_interpreter_and_shared_dependencies_are_rejected(self):
        data = bytearray(static_elf())
        struct.pack_into("<H", data, 18, 62)
        with self.assertRaisesRegex(ValueError, "AArch64"):
            tool.validate_static_arm64_elf(data)
        data = bytearray(static_elf())
        struct.pack_into("<I", data, 64, 3)
        with self.assertRaisesRegex(ValueError, "PT_INTERP"):
            tool.validate_static_arm64_elf(data)
        with self.assertRaisesRegex(ValueError, "DT_NEEDED"):
            tool.validate_static_arm64_elf(static_elf(dynamic_needed=True))
        with self.assertRaises(ValueError):
            tool.validate_static_arm64_elf(static_elf()[:100])

    def test_pid1_without_a_console_stays_alive_and_reaches_diagnostics(self):
        # Execute the real script in a filesystem sandbox made of regular test
        # files. Mount/mknod/reboot are shell mocks; no host mounts or reboot
        # syscall are reachable. Regular-file kmsg emulates a character sink
        # with append writes so all diagnostic markers can be inspected.
        sandbox = self.root / "runtime"
        for name in ("dev", "proc", "sys", "tmp", "etc/piano"):
            (sandbox / name).mkdir(parents=True, exist_ok=True)
        source = re.sub(r"/(dev|proc|sys|tmp|etc)(?=/|\s|$)",
                        lambda match: str(sandbox) + match[0], self.init.read_text())
        source = source.replace(">" + str(sandbox / "dev/kmsg"), ">>" + str(sandbox / "dev/kmsg"))
        timer = sandbox / "timer-armed"
        unexpected = sandbox / "unexpected-reboot"
        mocks = f"""
mount() {{ return 0; }}
mkdir() {{ return 0; }}
mknod() {{ [ "${{1##*/}}" != console ] || return 1; : >"$1"; }}
uname() {{ printf 'test-arm64-kernel\\n'; }}
sleep() {{
    if [ "$1" = 180 ]; then : >{timer}; /bin/sleep 30;
    else /bin/sleep 0.05; fi
}}
reboot() {{ : >{unexpected}; return 1; }}
"""
        script = sandbox / "test-init"
        script.write_text(mocks + source)
        process = subprocess.Popen(["/bin/sh", str(script)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True)
        try:
            deadline = time.monotonic() + 3
            log = ""
            while time.monotonic() < deadline and process.poll() is None:
                path = sandbox / "dev/kmsg"
                log = path.read_text() if path.exists() else ""
                if "DIAGNOSTICS_READY" in log and timer.exists():
                    break
                time.sleep(0.01)
            self.assertIsNone(process.poll(), "PID 1 exited when its console was unavailable")
            self.assertIn("BEGIN pid=", log)
            self.assertIn("CONSOLE unavailable continuing_with_kmsg", log)
            self.assertIn("DIAGNOSTICS_READY", log)
            self.assertTrue(timer.exists())
            self.assertFalse(unexpected.exists())
        finally:
            os.killpg(process.pid, signal.SIGTERM)
            process.communicate(timeout=3)


if __name__ == "__main__":
    unittest.main()
