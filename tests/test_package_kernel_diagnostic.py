"""Strict diagnostic topic provenance using real temporary Git/CPIO histories."""

import copy
import gzip
import importlib.util
import json
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from test_build_kernel import make_efi_image
from test_make_kernel_initramfs import static_elf


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("kernel_diagnostic", ROOT / "tools/package_kernel_diagnostic.py")
tool = importlib.util.module_from_spec(SPEC)
with patch.object(sys, "path", [str(ROOT / "tools"), *sys.path]):
    SPEC.loader.exec_module(tool)


class DiagnosticBundleTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repo = self.root / "source"
        self.repo.mkdir()
        self.git("init", "-q", "--initial-branch=topic/piano-efi-entry-debug")
        (self.repo / "code.c").write_text("base\n")
        self.git("add", "code.c")
        self.git("commit", "-qm", "base")
        self.base = self.git("rev-parse", "HEAD")
        (self.repo / "code.c").write_text("diagnostic marker\n")
        self.git("add", "code.c")
        self.git("commit", "-qm", "diagnostic")
        self.commit = self.git("rev-parse", "HEAD")
        self.kernel = self.root / "kernel"
        self.kernel.mkdir()
        image = make_efi_image() + bytes(4096 - len(make_efi_image()))
        self.config = b"\n".join([
            *(key.encode() + b"=y" for key in ("CONFIG_PIANO_EFI_ENTRY_DEBUG", "CONFIG_ARM64",
                "CONFIG_ARCH_QCOM", "CONFIG_EFI", "CONFIG_EFI_STUB", "CONFIG_ARM64_4K_PAGES",
                "CONFIG_DEBUG_KERNEL", "CONFIG_BLK_DEV_INITRD", "CONFIG_RD_GZIP", "CONFIG_PSTORE",
                "CONFIG_PSTORE_CONSOLE", "CONFIG_PSTORE_RAM")),
            b"# CONFIG_BLOCK is not set", b"# CONFIG_CMDLINE_FORCE is not set", b'CONFIG_CMDLINE=""', b"",
        ])
        (self.kernel / "Image").write_bytes(image)
        (self.kernel / "config").write_bytes(self.config)
        self.dtb = self.root / "live.dtb"
        body = struct.pack(">I", 1) + bytes(4) + struct.pack(">II", 2, 9)
        self.dtb.write_bytes(struct.pack(">10I", 0xd00dfeed, 72, 56, 72, 40, 17, 16, 0, 0, 16) + bytes(16) + body)
        self.busybox = self.root / "busybox"
        self.busybox.write_bytes(static_elf())
        self.provenance = self.root / "busybox-source.json"
        self.provenance.write_text(json.dumps({"package": {"A": "aarch64", "P": "busybox-static"}}))
        self.init = self.root / "init"
        self.init.write_bytes((ROOT / "bootprofiles/kernel-ram/init").read_bytes())
        self.topic = "piano-efi-entry-debug"
        self.policy = {"branch": "topic/" + self.topic, "commit": self.commit, "base_commit": self.base,
                       "base_config_sha256": "b" * 64,
                       "image_sha256": tool.sha(image), "config_sha256": tool.sha(self.config),
                       "dtb_sha256": tool.sha(self.dtb.read_bytes()), "busybox_sha256": tool.sha(self.busybox.read_bytes()),
                       "busybox_provenance_sha256": tool.sha(self.provenance.read_bytes()),
                       "init_sha256": tool.sha(self.init.read_bytes())}
        self.metadata = {"artifact_kind": self.topic, "profile": self.topic, "mode": "ram",
                         "source_commit": self.commit, "base_commit": self.base,
                         "source_repo": str(self.repo), "source_branch": self.policy["branch"],
                         "source_clean": True, "diagnostic_only": True, "hardware_verified": False,
                         "base_config_sha256": self.policy["base_config_sha256"],
                         "build": {"exit_code": 0, "kernel_release": "fixture-piano-debug"},
                         "files": {"Image": {"sha256": tool.sha(image), "bytes": len(image)},
                                   "config": {"sha256": tool.sha(self.config), "bytes": len(self.config)}}}
        self.manifest = self.kernel / "manifest.json"
        self.refresh_manifest()
        self.output = self.root / "diagnostic"

    def git(self, *args):
        return subprocess.check_output(["git", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
            "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", *args],
            cwd=self.repo, text=True, stderr=subprocess.PIPE).strip()

    def refresh_manifest(self):
        self.manifest.write_bytes(tool.json_bytes(self.metadata))
        self.policy["kernel_manifest_sha256"] = tool.sha(self.manifest.read_bytes())

    def generate(self):
        return tool.generate(self.topic, self.policy, self.manifest, self.dtb,
                             self.busybox, self.provenance, self.init, self.output)

    def inputs(self):
        return tool.read_inputs(self.topic, self.policy, self.manifest, self.dtb,
                                self.busybox, self.provenance, self.init)

    def verify(self):
        return tool.verify_bundle(self.output, self.topic, self.policy, self.inputs())

    def test_valid_topic_has_new_module_free_cpio_and_exact_v2_bytes(self):
        result = self.generate()
        image, initrd, dtb = tool.inspect_payload((self.output / "linux-payload.bin").read_bytes())
        self.assertEqual(image, (self.kernel / "Image").read_bytes())
        self.assertEqual(dtb, self.dtb.read_bytes())
        rows = {row["name"]: row for row in tool.ram.inspect_newc(gzip.decompress(initrd))}
        self.assertFalse(any(".ko" in name or name.startswith("lib/") for name in rows))
        self.assertEqual(self.init.read_bytes(), rows["init"]["data"])
        self.assertIn(self.commit.encode(), rows["etc/piano/kernel-build"]["data"])
        self.assertEqual(self.policy["dtb_sha256"], json.loads(rows["etc/piano/diagnostic-topic.json"]["data"])["dtb_sha256"])
        self.assertFalse(result["hardware_verified"])
        self.assertFalse(result["storage_drivers_allowed"])
        self.assertEqual(result, self.verify())
        self.assertEqual(result["payload_sha256"], self.generate()["payload_sha256"])

    def test_metadata_cannot_self_relabel_topic_source_or_claim_hardware(self):
        original = copy.deepcopy(self.metadata)
        changes = [("profile", "next"), ("mode", "userspace-debug"), ("source_commit", "a" * 40),
                   ("base_commit", "a" * 40), ("source_clean", False), ("hardware_verified", True)]
        for key, value in changes:
            self.metadata = copy.deepcopy(original)
            self.metadata[key] = value
            self.refresh_manifest()  # fixture re-authorizes only JSON hash, not the independent topic/source pins.
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.generate()
        self.assertFalse(self.output.exists())

    def test_incomplete_or_boolean_exit_status_is_rejected(self):
        for value in (1, False, "0"):
            self.metadata["build"]["exit_code"] = value
            self.refresh_manifest()
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "completed host-only"):
                self.generate()

    def test_image_config_dtb_busybox_init_and_manifest_drift_are_rejected(self):
        for path in (self.kernel / "Image", self.kernel / "config", self.dtb, self.busybox,
                     self.provenance, self.init, self.manifest):
            original = path.read_bytes()
            path.write_bytes(original + b"drift")
            with self.subTest(path=path.name), self.assertRaisesRegex(ValueError, "SHA256 differs"):
                self.generate()
            path.write_bytes(original)
        self.assertFalse(self.output.exists())

    def test_self_consistent_modified_config_cannot_change_independent_pin(self):
        config = self.config + b"CONFIG_BLOCK=y\n"
        (self.kernel / "config").write_bytes(config)
        self.metadata["files"]["config"].update(sha256=tool.sha(config), bytes=len(config))
        self.refresh_manifest()
        with self.assertRaisesRegex(ValueError, "Config SHA256"):
            self.generate()

    def test_approved_hash_still_requires_valid_efi_header_and_config_gate(self):
        image = bytearray((self.kernel / "Image").read_bytes())
        image[64:68] = b"NOPE"
        (self.kernel / "Image").write_bytes(image)
        self.policy["image_sha256"] = tool.sha(image)
        self.metadata["files"]["Image"]["sha256"] = tool.sha(image)
        self.refresh_manifest()
        with self.assertRaisesRegex(ValueError, "EFI stub header"):
            self.generate()
        (self.kernel / "Image").write_bytes(make_efi_image() + bytes(3072))
        self.policy["image_sha256"] = tool.sha((self.kernel / "Image").read_bytes())
        self.metadata["files"]["Image"]["sha256"] = self.policy["image_sha256"]
        config = self.config.replace(b"CONFIG_PIANO_EFI_ENTRY_DEBUG=y", b"# CONFIG_PIANO_EFI_ENTRY_DEBUG is not set")
        (self.kernel / "config").write_bytes(config)
        self.policy["config_sha256"] = tool.sha(config)
        self.metadata["files"]["config"].update(sha256=tool.sha(config), bytes=len(config))
        self.refresh_manifest()
        with self.assertRaisesRegex(ValueError, "requires CONFIG_PIANO_EFI_ENTRY_DEBUG"):
            self.generate()

    def test_dirty_or_renamed_or_moved_real_git_topic_is_rejected(self):
        code = self.repo / "code.c"
        original = code.read_bytes()
        code.write_bytes(original + b"dirty")
        with self.assertRaisesRegex(ValueError, "worktree is dirty"):
            self.generate()
        code.write_bytes(original)
        self.git("branch", "-m", "other-topic")
        with self.assertRaisesRegex(ValueError, "branch mismatch"):
            self.generate()
        self.git("branch", "-m", self.policy["branch"])
        code.write_bytes(original + b"new commit")
        self.git("add", "code.c")
        self.git("commit", "-qm", "moved")
        with self.assertRaisesRegex(ValueError, "HEAD changed"):
            self.generate()

    def test_mid_generation_input_or_source_mutation_does_not_publish(self):
        original_generate = tool.ram.generate
        for change in (self.kernel / "config", self.repo / "code.c"):
            original = change.read_bytes()
            def mutate(*args, **kwargs):
                result = original_generate(*args, **kwargs)
                change.write_bytes(original + b"changed during generation")
                return result
            with self.subTest(change=change), patch.object(tool.ram, "generate", side_effect=mutate), self.assertRaises(ValueError):
                self.generate()
            change.write_bytes(original)
            self.assertFalse(self.output.exists())

    def test_output_cannot_overwrite_input_or_modify_topic_tree(self):
        for location in (self.kernel, self.repo / "output"):
            self.output = location
            with self.subTest(location=location), self.assertRaisesRegex(ValueError, "separate from authoritative"):
                self.generate()

    def test_published_nested_manifest_or_config_drift_is_rejected(self):
        self.generate()
        for filename in ("config", "initramfs-manifest.json"):
            path = self.output / filename
            original = path.read_bytes()
            if filename.endswith(".json"):
                meta = json.loads(original)
                meta["kernel_config"]["sha256"] = "0" * 64
                path.write_bytes(tool.json_bytes(meta))
            else:
                path.write_bytes(original + b"drift")
            with self.subTest(filename=filename), self.assertRaises(ValueError):
                self.verify()
            path.write_bytes(original)

    def test_payload_bounds_hashes_header_and_trailing_bytes_are_rejected(self):
        self.generate()
        original = (self.output / "linux-payload.bin").read_bytes()
        cases = [original[:143], original[:-1], original + b"trailing"]
        for offset in (16, 20, 24, 32, 104):
            bad = bytearray(original)
            bad[offset:offset + (4 if offset in (16, 20) else 8)] = b"\xff" * (4 if offset in (16, 20) else 8)
            cases.append(bad)
        for offset in (40, 72, 112, 144, len(original) - 1):
            bad = bytearray(original)
            bad[offset] ^= 1
            cases.append(bad)
        for data in cases:
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                tool.inspect_payload(data)

    def test_cpio_rejects_modules_wrong_modes_and_changed_embedded_binding(self):
        self.generate()
        inputs = self.inputs()
        binding = tool.diagnostic_binding(self.topic, self.policy, inputs)
        initrd = (self.output / "initramfs.cpio.gz").read_bytes()
        original = tool.ram.inspect_newc(gzip.decompress(initrd))
        cases = []
        rows = copy.deepcopy(original)
        rows.append({"name": "lib/modules/6.6/old.ko", "mode": stat.S_IFREG | 0o644, "data": b"old module"})
        cases.append(rows)
        rows = copy.deepcopy(original)
        next(row for row in rows if row["name"] == "init")["mode"] = stat.S_IFREG | 0o644
        cases.append(rows)
        rows = copy.deepcopy(original)
        next(row for row in rows if row["name"] == "etc/piano/diagnostic-topic.json")["data"] = b"{}"
        cases.append(rows)
        for rows in cases:
            with self.subTest(names=len(rows)), self.assertRaises(ValueError):
                tool.check_archive(gzip.compress(tool.ram.make_newc(rows)), inputs, binding)

    def test_bounded_archive_decompression_rejects_expansion_bomb(self):
        with patch.object(tool, "MAX_INITRD", 1024), self.assertRaisesRegex(ValueError, "CPIO exceeds"):
            tool.unpack_archive(gzip.compress(bytes(2048)))


if __name__ == "__main__":
    unittest.main()
