#!/usr/bin/env python3
"""Build/verify a fixed, explicitly approved diagnostic-topic RAM V2 bundle.

This separate entrance never reads or changes stable/next profile pins. Each
approved topic binds an exact clean Git commit, completed build, Image, config,
captured DTB and RAM userspace sources. No kernel build, downloads or devices.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import stat
import struct
import subprocess
import tempfile
from pathlib import Path

import make_kernel_initramfs as ram
from build_kernel import image_info
from compose_piano_dtb import read_fdt


ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"SUNUEFI-LINUXv2\0"
HEADER_BYTES = 144
MAX_KERNEL = 0x4000000
MAX_INITRD = 0x1000000  # RAM profile also fits the raw loader's stricter limit.
MAX_DTB = 0x200000
TOPICS = {
    "piano-efi-entry-debug": {
        "branch": "topic/piano-efi-entry-debug",
        "commit": "094d0b053f61ca20f584e10faa624cd0bc745db0",
        "base_commit": "7704c4c5bb127673b4f0ead839919db573559e38",
        "kernel_manifest_sha256": "e3c464a64a6aa9959c7dfd78c113376d7d1ff228c976a7cf64a3025a3fcda30e",
        "image_sha256": "7091941820b74407d5dd0575495afdea29e8f7ecde699705807cc817f0135ce9",
        "config_sha256": "a7fae7617530e464c9c624bf4d5a16c760060c45b0db8b82a44d9b26c02fc14c",
        "base_config_sha256": "85c048d4f361802c204be5cae880bbfd9d961e170730ede9cba9198b2153fb31",
        "dtb_sha256": "a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7",
        "busybox_sha256": "999cb969d09093a71716cfc747bb53cdada3f332c05eb5046c56e0f66a4d6d22",
        "busybox_provenance_sha256": "c153892fffb695a6af46962fc5a502224e0640c2011ec316fd2659013818db92",
        "init_sha256": "96aa39c200d1acdef712794df241aab93ef17051a06943d654dbfeb21c84dc48",
    },
    "piano-efi-memory-debug": {
        "branch": "topic/piano-efi-memory-debug",
        "commit": "c4bbf928f335174f8518831797a94597a530c575",
        "base_commit": "7704c4c5bb127673b4f0ead839919db573559e38",
        "parent_commit": "094d0b053f61ca20f584e10faa624cd0bc745db0",
        "additional_config": ("CONFIG_PIANO_EFI_MEMORY_DEBUG",),
        "kernel_manifest_sha256": "eb01f9e7588df2f7fac3fce8d0027338932b31dae4aadf0d76d7fffb641ebbb0",
        "image_sha256": "1dcb79e2f3cf4c79f0fe7ba3523091369202c5c4f67a84361c4d1cf4a02e288a",
        "config_sha256": "ec7b9f916b8ec801c218bb7182fa6639d3c84749d516455db9b73cb098004172",
        "base_config_sha256": "85c048d4f361802c204be5cae880bbfd9d961e170730ede9cba9198b2153fb31",
        "dtb_sha256": "a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7",
        "busybox_sha256": "999cb969d09093a71716cfc747bb53cdada3f332c05eb5046c56e0f66a4d6d22",
        "busybox_provenance_sha256": "c153892fffb695a6af46962fc5a502224e0640c2011ec316fd2659013818db92",
        "init_sha256": "96aa39c200d1acdef712794df241aab93ef17051a06943d654dbfeb21c84dc48",
    },
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def json_bytes(value: dict) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode()


def git(repo: Path, *args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", "--no-lazy-fetch", "-C", str(repo), *args],
            text=True, stderr=subprocess.PIPE, timeout=30).strip()
    except (subprocess.SubprocessError, OSError) as exc:
        raise ValueError("Cannot verify diagnostic Git source: " + str(exc)) from exc


def check_repository(repo: Path, policy: dict) -> None:
    if Path(git(repo, "rev-parse", "--show-toplevel")).resolve() != repo.resolve():
        raise ValueError("Diagnostic source must be its Git worktree root")
    if git(repo, "rev-parse", "HEAD") != policy["commit"]:
        raise ValueError("Diagnostic topic HEAD changed from approved commit")
    if git(repo, "symbolic-ref", "--short", "HEAD") != policy["branch"]:
        raise ValueError("Diagnostic topic branch mismatch")
    parent = policy.get("parent_commit", policy["base_commit"])
    if git(repo, "rev-parse", "HEAD^") != parent:
        raise ValueError("Diagnostic topic parent/base mismatch")
    if git(repo, "merge-base", policy["base_commit"], policy["commit"]) != policy["base_commit"]:
        raise ValueError("Diagnostic upstream base is not an ancestor")
    if git(repo, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("Diagnostic topic worktree is dirty")


def capture(path: Path, expected: str, label: str, snapshots: dict) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError(label + " must be a regular input file")
    data = path.read_bytes()
    if sha(data) != expected:
        raise ValueError(label + " SHA256 differs from approved diagnostic input")
    snapshots[path] = data
    return data


def check_snapshots(snapshots: dict) -> None:
    for path, original in snapshots.items():
        if path.is_symlink() or not path.is_file() or path.read_bytes() != original:
            raise ValueError("Diagnostic input changed while packaging: " + str(path))


def read_inputs(topic: str, policy: dict, kernel_manifest: Path, dtb_path: Path,
                busybox_path: Path, provenance_path: Path, init_path: Path) -> dict:
    snapshots = {}
    raw = capture(kernel_manifest, policy["kernel_manifest_sha256"], "Kernel manifest", snapshots)
    meta = json.loads(raw)
    if (meta.get("artifact_kind") != topic or meta.get("profile") != topic or
            meta.get("mode") != "ram" or meta.get("source_commit") != policy["commit"] or
            meta.get("base_commit") != policy["base_commit"] or
            meta.get("source_branch") != policy["branch"]):
        raise ValueError("Diagnostic manifest topic/mode/source/base mismatch")
    if "parent_commit" in policy and meta.get("parent_commit") != policy["parent_commit"]:
        raise ValueError("Diagnostic manifest parent mismatch")
    if (meta.get("diagnostic_only") is not True or meta.get("hardware_verified") is not False or
            meta.get("source_clean") is not True or
            type(meta.get("build", {}).get("exit_code")) is not int or
            meta["build"]["exit_code"] != 0):
        raise ValueError("Diagnostic source must be a clean completed host-only build")
    if meta.get("base_config_sha256") != policy["base_config_sha256"]:
        raise ValueError("Diagnostic base config mismatch")
    repo = Path(meta["source_repo"])
    check_repository(repo, policy)
    image_path, config_path = kernel_manifest.parent / "Image", kernel_manifest.parent / "config"
    image = capture(image_path, policy["image_sha256"], "Image", snapshots)
    config = capture(config_path, policy["config_sha256"], "Config", snapshots)
    for name, data in (("Image", image), ("config", config)):
        record = meta.get("files", {}).get(name, {})
        if record.get("sha256") != sha(data) or record.get("bytes") != len(data):
            raise ValueError("Diagnostic build manifest/file binding mismatch: " + name)
    if not 4096 <= len(image) <= MAX_KERNEL or not image_info(image_path)["efi_stub"]:
        raise ValueError("Diagnostic Image must fit the loader and have a valid ARM64 EFI stub")
    values = dict(re.findall(r"(?m)^(CONFIG_[A-Z0-9_]+)=(.*)$", config.decode()))
    for key in ("CONFIG_PIANO_EFI_ENTRY_DEBUG", "CONFIG_ARM64", "CONFIG_ARCH_QCOM",
                "CONFIG_EFI", "CONFIG_EFI_STUB", "CONFIG_ARM64_4K_PAGES", "CONFIG_DEBUG_KERNEL",
                *policy.get("additional_config", ())):
        if values.get(key) != "y":
            raise ValueError("Diagnostic config requires " + key + "=y")
    ram.config_summary(config)  # also rejects BLOCK/CMDLINE_FORCE and missing RAM dependencies.
    dtb = capture(dtb_path, policy["dtb_sha256"], "Captured live DTB", snapshots)
    if not 40 <= len(dtb) <= MAX_DTB:
        raise ValueError("Diagnostic DTB exceeds loader bounds")
    read_fdt(dtb)
    busybox = capture(busybox_path, policy["busybox_sha256"], "BusyBox", snapshots)
    ram.validate_static_arm64_elf(busybox)
    provenance = capture(provenance_path, policy["busybox_provenance_sha256"], "BusyBox provenance", snapshots)
    if json.loads(provenance).get("package", {}).get("A") != "aarch64":
        raise ValueError("BusyBox provenance architecture mismatch")
    init = capture(init_path, policy["init_sha256"], "RAM PID1 source", snapshots)
    return {"meta": meta, "raw": raw, "repo": repo, "image": image, "config": config,
            "dtb": dtb, "busybox": busybox, "provenance": provenance, "init": init, "snapshots": snapshots}


def diagnostic_binding(topic: str, policy: dict, inputs: dict) -> dict:
    binding = {"topic": topic, "source_commit": policy["commit"], "base_commit": policy["base_commit"],
            "kernel_image_sha256": sha(inputs["image"]), "kernel_config_sha256": sha(inputs["config"]),
            "dtb_sha256": sha(inputs["dtb"]), "kernel_build_manifest_sha256": sha(inputs["raw"]),
            "diagnostic_only": True, "hardware_verified": False}
    if "parent_commit" in policy:
        binding["parent_commit"] = policy["parent_commit"]
    return binding


def inspect_payload(payload: bytes) -> tuple[bytes, bytes, bytes]:
    if len(payload) < HEADER_BYTES or payload[:16] != MAGIC:
        raise ValueError("Diagnostic payload V2 header missing")
    if struct.unpack_from("<II", payload, 16) != (2, HEADER_BYTES):
        raise ValueError("Diagnostic payload version/header size mismatch")
    sizes = [int.from_bytes(payload[offset:offset + 8], "little") for offset in (24, 32, 104)]
    if not (4096 <= sizes[0] <= MAX_KERNEL and 0 < sizes[1] <= MAX_INITRD and 40 <= sizes[2] <= MAX_DTB):
        raise ValueError("Diagnostic payload component bounds invalid")
    if HEADER_BYTES + sum(sizes) != len(payload):
        raise ValueError("Diagnostic payload length/trailing data mismatch")
    cursor, parts = HEADER_BYTES, []
    for size, hash_offset in zip(sizes, (40, 72, 112)):
        part = payload[cursor:cursor + size]
        if hashlib.sha256(part).digest() != payload[hash_offset:hash_offset + 32]:
            raise ValueError("Diagnostic payload component hash mismatch")
        parts.append(part)
        cursor += size
    read_fdt(parts[2])
    return tuple(parts)


def unpack_archive(initrd: bytes) -> bytes:
    with gzip.GzipFile(fileobj=io.BytesIO(initrd)) as stream:
        archive = stream.read(MAX_INITRD + 1)
    if len(archive) > MAX_INITRD:
        raise ValueError("Diagnostic uncompressed CPIO exceeds RAM bound")
    return archive


def check_archive(initrd: bytes, inputs: dict, binding: dict) -> None:
    records = ram.inspect_newc(unpack_archive(initrd))
    rows = {row["name"]: row for row in records}
    expected = {"bin", "sbin", "dev", "proc", "sys", "tmp", "etc", "etc/piano",
                "dev/console", "dev/null", "dev/kmsg", "bin/busybox", "init",
                "etc/piano/kernel-build", "etc/piano/config-summary", "etc/piano/busybox-source.json",
                "etc/piano/diagnostic-topic.json", *("bin/" + name for name in ram.APPLETS)}
    if set(rows) != expected:
        raise ValueError("Diagnostic archive has unexpected entries (modules/rootfs forbidden)")
    if rows["init"]["data"] != inputs["init"] or rows["bin/busybox"]["data"] != inputs["busybox"]:
        raise ValueError("Diagnostic archive PID1/BusyBox provenance mismatch")
    if json.loads(rows["etc/piano/diagnostic-topic.json"]["data"]) != binding:
        raise ValueError("Diagnostic archive topic/Image/config/DTB binding mismatch")
    build = (f"profile={binding['topic']}\nmode=ram\nsource_commit={binding['source_commit']}\n"
             f"kernel_release={inputs['meta']['build']['kernel_release']}\n"
             f"kernel_config_sha256={sha(inputs['config'])}\n"
             f"kernel_image_sha256={sha(inputs['image'])}\n").encode()
    if rows["etc/piano/kernel-build"]["data"] != build or rows["etc/piano/config-summary"]["data"] != ram.config_summary(inputs["config"]):
        raise ValueError("Diagnostic archive build/config metadata mismatch")
    if rows["etc/piano/busybox-source.json"]["data"] != inputs["provenance"]:
        raise ValueError("Diagnostic archive BusyBox source mismatch")
    devices = {"dev/console": (5, 1, 0o600), "dev/null": (1, 3, 0o666), "dev/kmsg": (1, 11, 0o600)}
    for name, row in rows.items():
        if name in devices:
            major, minor, permissions = devices[name]
            expected_mode = stat.S_IFCHR | permissions
            if row["major"] != major or row["minor"] != minor or row["data"]:
                raise ValueError("Diagnostic archive device node mismatch")
        elif name in ("bin", "sbin", "dev", "proc", "sys", "tmp", "etc", "etc/piano"):
            expected_mode = stat.S_IFDIR | 0o755
        elif name in ("init", "bin/busybox"):
            expected_mode = stat.S_IFREG | 0o755
        elif name.startswith("etc/"):
            expected_mode = stat.S_IFREG | 0o644
        else:
            expected_mode = stat.S_IFLNK | 0o777
        if row["mode"] != expected_mode:
            raise ValueError("Diagnostic archive entry mode mismatch")
    for name in ram.APPLETS:
        if not stat.S_ISLNK(rows["bin/" + name]["mode"]) or rows["bin/" + name]["data"] != b"busybox":
            raise ValueError("Diagnostic archive applet symlink mismatch")


def verify_bundle(directory: Path, topic: str, policy: dict, inputs: dict) -> dict:
    binding = diagnostic_binding(topic, policy, inputs)
    kernel_meta = json.loads((directory / "manifest.json").read_bytes())
    init_meta = json.loads((directory / "initramfs-manifest.json").read_bytes())
    payload_meta = json.loads((directory / "payload-manifest.json").read_bytes())
    for meta in (kernel_meta, init_meta, payload_meta):
        if (meta.get("profile") != topic or meta.get("mode") != "ram" or
                meta.get("source_commit") != policy["commit"] or
                meta.get("diagnostic_topic") != binding or meta.get("hardware_verified") is not False):
            raise ValueError("Published diagnostic manifest source/binding/gate mismatch")
    if (kernel_meta.get("source_dirty") is not False or
            kernel_meta.get("status") != "HOST_BUILT_NOT_HARDWARE_VERIFIED" or
            init_meta.get("modules") != 0 or init_meta.get("module_loading") is not False or
            init_meta.get("persistent_filesystem_mounts") is not False or
            init_meta.get("recovery_timer_seconds") != 180 or
            payload_meta.get("storage_drivers_allowed") is not False or
            payload_meta.get("runtime_dtb_from_abl") is not False):
        raise ValueError("Published diagnostic RAM safety contract mismatch")
    payload = (directory / "linux-payload.bin").read_bytes()
    image, initrd, dtb = inspect_payload(payload)
    if (image != inputs["image"] or dtb != inputs["dtb"] or
            (directory / "Image").read_bytes() != image or
            (directory / "config").read_bytes() != inputs["config"] or
            (directory / "live.dtb").read_bytes() != dtb or
            (directory / "initramfs.cpio.gz").read_bytes() != initrd):
        raise ValueError("Published diagnostic artifacts differ from pinned inputs/payload")
    check_archive(initrd, inputs, binding)
    archive = unpack_archive(initrd)
    if (kernel_meta.get("image", {}).get("sha256") != sha(image) or
            kernel_meta.get("image", {}).get("bytes") != len(image) or
            kernel_meta.get("config_sha256") != sha(inputs["config"]) or
            init_meta.get("kernel_manifest", {}).get("sha256") != sha((directory / "manifest.json").read_bytes()) or
            init_meta.get("kernel_image", {}).get("sha256") != sha(image) or
            init_meta.get("kernel_config", {}).get("sha256") != sha(inputs["config"]) or
            init_meta.get("initramfs", {}).get("sha256") != sha(initrd) or
            init_meta.get("initramfs", {}).get("uncompressed_sha256") != sha(archive) or
            init_meta.get("init_source", {}).get("sha256") != sha(inputs["init"]) or
            init_meta.get("busybox", {}).get("sha256") != sha(inputs["busybox"]) or
            init_meta.get("busybox", {}).get("provenance_sha256") != sha(inputs["provenance"]) or
            payload_meta.get("kernel_manifest_sha256") != sha((directory / "manifest.json").read_bytes()) or
            payload_meta.get("initramfs_manifest_sha256") != sha((directory / "initramfs-manifest.json").read_bytes()) or
            payload_meta.get("kernel_sha256") != sha(image) or
            payload_meta.get("initrd_sha256") != sha(initrd) or
            payload_meta.get("dtb_sha256") != sha(dtb) or
            payload_meta.get("config_sha256") != sha(inputs["config"]) or
            payload_meta.get("payload_sha256") != sha(payload) or
            payload_meta.get("payload_bytes") != len(payload)):
        raise ValueError("Published diagnostic manifest hash binding mismatch")
    check_snapshots(inputs["snapshots"])
    check_repository(inputs["repo"], policy)
    return payload_meta


def generate(topic: str, policy: dict, kernel_manifest: Path, dtb_path: Path,
             busybox_path: Path, provenance_path: Path, init_path: Path, output: Path) -> dict:
    inputs = read_inputs(topic, policy, kernel_manifest, dtb_path, busybox_path, provenance_path, init_path)
    binding = diagnostic_binding(topic, policy, inputs)
    # Keep authoritative inputs separate; never overwrite/relabel the topic build or regular profiles.
    if output.resolve().is_relative_to(inputs["repo"].resolve()) or output.resolve() == kernel_manifest.parent.resolve() or any(
            path.parent.resolve() == output.resolve() for path in inputs["snapshots"]):
        raise ValueError("Diagnostic output must be separate from authoritative inputs")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".piano-diagnostic-", dir=output.parent) as temporary:
        stage = Path(temporary)
        (stage / "Image").write_bytes(inputs["image"])
        (stage / "config").write_bytes(inputs["config"])
        (stage / "live.dtb").write_bytes(inputs["dtb"])
        kernel = {"schema_version": 1, "profile": topic, "mode": "ram", "source_commit": policy["commit"],
                  "source_dirty": False, "source_repo": str(inputs["repo"].resolve()),
                  "status": "HOST_BUILT_NOT_HARDWARE_VERIFIED", "hardware_verified": False,
                  "kernel_release": inputs["meta"]["build"]["kernel_release"],
                  "image": {"sha256": sha(inputs["image"]), "bytes": len(inputs["image"]), "efi_stub": True},
                  "config_sha256": sha(inputs["config"]), "diagnostic_topic": binding,
                  "original_topic_build": {"path": str(kernel_manifest.resolve()), "sha256": sha(inputs["raw"])}}
        if "parent_commit" in policy:
            kernel["parent_commit"] = policy["parent_commit"]
            kernel["base_commit"] = policy["base_commit"]
        (stage / "manifest.json").write_bytes(json_bytes(kernel))
        init = ram.generate(topic, stage / "manifest.json", busybox_path, provenance_path, init_path, stage)
        records = ram.inspect_newc(gzip.decompress((stage / "initramfs.cpio.gz").read_bytes()))
        records.append({"name": "etc/piano/diagnostic-topic.json", "mode": stat.S_IFREG | 0o644,
                        "data": json_bytes(binding)})
        archive = ram.make_newc(records)
        initrd = gzip.compress(archive, mtime=0)
        check_archive(initrd, inputs, binding)
        (stage / "initramfs.cpio.gz").write_bytes(initrd)
        init["diagnostic_topic"] = binding
        init["kernel_manifest"]["path"] = str((output / "manifest.json").resolve())
        init["kernel_image"]["path"] = str((output / "Image").resolve())
        init["kernel_config"]["path"] = str((output / "config").resolve())
        init["initramfs"].update({"path": str((output / "initramfs.cpio.gz").resolve()),
                                  "sha256": sha(initrd), "bytes": len(initrd),
                                  "uncompressed_sha256": sha(archive), "uncompressed_bytes": len(archive),
                                  "entries": [row["name"] for row in records]})
        (stage / "initramfs-manifest.json").write_bytes(json_bytes(init))
        header = struct.pack("<16sIIQQ32s32sQ32s", MAGIC, 2, HEADER_BYTES,
                             len(inputs["image"]), len(initrd), hashlib.sha256(inputs["image"]).digest(),
                             hashlib.sha256(initrd).digest(), len(inputs["dtb"]), hashlib.sha256(inputs["dtb"]).digest())
        payload = header + inputs["image"] + initrd + inputs["dtb"]
        inspect_payload(payload)
        (stage / "linux-payload.bin").write_bytes(payload)
        result = {"schema_version": 1, "status": "HOST_PACKAGED_NOT_BOOTED", "payload_version": 2,
                  "profile": topic, "mode": "ram", "source_commit": policy["commit"],
                  "config_sha256": sha(inputs["config"]), "kernel_sha256": sha(inputs["image"]),
                  "initrd_sha256": sha(initrd), "dtb_source": str(dtb_path.resolve()), "dtb_sha256": sha(inputs["dtb"]),
                  "payload_sha256": sha(payload), "payload_bytes": len(payload), "diagnostic_topic": binding,
                  "kernel_manifest_sha256": sha((stage / "manifest.json").read_bytes()),
                  "initramfs_manifest_sha256": sha((stage / "initramfs-manifest.json").read_bytes()),
                  "runtime_dtb_from_abl": False, "storage_drivers_allowed": False,
                  "dtb_board_topology_verified": False, "hardware_verified": False,
                  "payload_path": str((output / "linux-payload.bin").resolve())}
        (stage / "payload-manifest.json").write_bytes(json_bytes(result))
        verify_bundle(stage, topic, policy, inputs)
        output.mkdir(parents=True, exist_ok=True)
        # Publish the manifest last. Verification rejects mixed or interrupted generations.
        for name in ("Image", "config", "live.dtb", "manifest.json", "initramfs.cpio.gz",
                     "initramfs-manifest.json", "linux-payload.bin", "payload-manifest.json"):
            os.replace(stage / name, output / name)
    return verify_bundle(output, topic, policy, inputs)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topic", choices=tuple(TOPICS), required=True)
    parser.add_argument("--kernel-manifest", type=Path)
    parser.add_argument("--dtb", type=Path, default=ROOT / "private/captures/2026-10-03-piano/live.dtb")
    parser.add_argument("--busybox", type=Path, default=ROOT / "build/linux-ram/busybox")
    parser.add_argument("--busybox-provenance", type=Path, default=ROOT / "build/linux-ram/busybox-source.json")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--verify-output", action="store_true")
    args = parser.parse_args()
    artifact = ROOT / "artifacts/kernel-topics" / args.topic
    manifest, output = args.kernel_manifest or artifact / "manifest.json", args.output_dir or artifact / "ram"
    init = ROOT / "bootprofiles/kernel-ram/init"
    policy = TOPICS[args.topic]
    try:
        if args.verify_output:
            inputs = read_inputs(args.topic, policy, manifest, args.dtb, args.busybox, args.busybox_provenance, init)
            result = verify_bundle(output, args.topic, policy, inputs)
        else:
            result = generate(args.topic, policy, manifest, args.dtb, args.busybox, args.busybox_provenance, init, output)
    except (ValueError, OSError, KeyError, struct.error) as exc:
        parser.exit(2, "kernel diagnostic: " + str(exc) + "\n")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
