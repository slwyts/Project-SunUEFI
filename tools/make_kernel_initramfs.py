#!/usr/bin/env python3
"""Build a provenance-checked, module-free ARM64 kernel RAM initramfs.

Host-only: no device access, module loading, persistent disk mounting or kernel
build. Defaults to artifacts/kernels/{stable,next}/ram. The kernel Image/config
must match that profile's existing manifest. Generation is not a boot result.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import stat
import struct
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]
RECOVERY_SECONDS = 180
APPLETS = ("sh", "mount", "mknod", "mkdir", "sleep", "reboot", "uname", "cat",
           "tr", "gzip", "sha256sum")
CONFIG_KEYS = {
    "CONFIG_ARM64", "CONFIG_ARCH_QCOM", "CONFIG_EFI", "CONFIG_EFI_STUB",
    "CONFIG_BLK_DEV_INITRD", "CONFIG_CMDLINE", "CONFIG_CMDLINE_FORCE", "CONFIG_EXPERT",
    "CONFIG_BLOCK", "CONFIG_MODULES", "CONFIG_PSTORE", "CONFIG_PSTORE_CONSOLE",
    "CONFIG_PSTORE_RAM", "CONFIG_DRM_SIMPLEDRM", "CONFIG_SYSFB_SIMPLEFB",
    "CONFIG_ARM_SMMU", "CONFIG_USB_DWC3", "CONFIG_USB_DWC3_QCOM", "CONFIG_USB_GADGET",
    "CONFIG_USB_CONFIGFS_ACM", "CONFIG_USB_CONFIGFS_NCM", "CONFIG_SCSI_UFSHCD",
    "CONFIG_SCSI_UFS_QCOM", "CONFIG_USB_STORAGE", "CONFIG_MMC", "CONFIG_MTD",
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_static_arm64_elf(data: bytes) -> dict:
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("BusyBox must be an ELF64 little-endian executable")
    elf_type, machine, version, entry, phoff, _, _, ehsize, phsize, phcount, _, _, _ = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    if machine != 183 or elf_type not in (2, 3) or version != 1 or ehsize != 64:
        raise ValueError("BusyBox must be an AArch64 ELF executable")
    if phsize != 56 or not 0 < phcount < 65535 or phoff < 64 or phoff + phsize * phcount > len(data):
        raise ValueError("Invalid ELF program header table")
    loads = []
    for index in range(phcount):
        kind, flags, offset, address, _, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + phsize * index)
        if filesz and offset + filesz > len(data):
            raise ValueError("ELF segment exceeds file bounds")
        if kind == 3:
            raise ValueError("BusyBox has PT_INTERP; a static executable is required")
        if kind == 1:
            if memsz < filesz:
                raise ValueError("ELF load segment memory is smaller than file data")
            loads.append((address, memsz, flags))
        if kind == 2:
            if filesz % 16:
                raise ValueError("Invalid ELF dynamic segment")
            for pos in range(offset, offset + filesz, 16):
                tag, _ = struct.unpack_from("<qQ", data, pos)
                if tag == 0:
                    break
                if tag == 1:
                    raise ValueError("BusyBox has DT_NEEDED; external shared libraries are forbidden")
    if not any(flags & 1 and address <= entry < address + size for address, size, flags in loads):
        raise ValueError("ELF entry is not inside an executable load segment")
    return {"class": "ELF64", "endianness": "little", "machine": "AArch64",
            "static": True, "pt_interp": False, "dt_needed": False}


def validate_path(name: str) -> None:
    path = PurePosixPath(name)
    if not name or "\0" in name or "\\" in name or path.is_absolute() or any(part in (".", "..") for part in name.split("/")) or str(path) != name:
        raise ValueError("Unsafe/noncanonical CPIO path: " + repr(name))


def make_newc(entries: list[dict]) -> bytes:
    archive = bytearray()
    seen = set()
    for inode, record in enumerate(entries + [{"name": "TRAILER!!!", "mode": 0, "data": b""}], 1):
        name, mode, data = record["name"], record["mode"], record.get("data", b"")
        validate_path(name)
        if name in seen:
            raise ValueError("Duplicate CPIO path: " + name)
        seen.add(name)
        if stat.S_ISLNK(mode):
            target = data.decode()
            validate_path(target)
            if (PurePosixPath(name).parent / target).as_posix() not in {item["name"] for item in entries}:
                raise ValueError("CPIO symlink target is not supplied: " + name)
        raw_name = name.encode() + b"\0"
        fields = [inode, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, 0, len(data),
                  0, 0, record.get("major", 0), record.get("minor", 0), len(raw_name), 0]
        if any(not 0 <= value <= 0xffffffff for value in fields):
            raise ValueError("CPIO field exceeds newc limits")
        archive.extend(b"070701" + b"".join(f"{value:08x}".encode() for value in fields))
        archive.extend(raw_name)
        archive.extend(b"\0" * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b"\0" * (-len(archive) % 4))
    archive.extend(b"\0" * (-len(archive) % 512))
    return bytes(archive)


def inspect_newc(archive: bytes) -> list[dict]:
    records, seen, offset = [], set(), 0
    while offset + 110 <= len(archive):
        if archive[offset:offset + 6] != b"070701":
            raise ValueError("Invalid newc magic")
        try:
            fields = [int(archive[offset + 6 + i * 8:offset + 14 + i * 8], 16) for i in range(13)]
        except ValueError as exc:
            raise ValueError("Invalid newc header") from exc
        mode, size, namesize = fields[1], fields[6], fields[11]
        offset += 110
        if not namesize or offset + namesize > len(archive) or archive[offset + namesize - 1] != 0:
            raise ValueError("Invalid newc filename")
        name = archive[offset:offset + namesize - 1].decode()
        validate_path(name)
        if name in seen:
            raise ValueError("Duplicate archive entry")
        seen.add(name)
        offset = (offset + namesize + 3) & ~3
        if offset + size > len(archive):
            raise ValueError("Truncated newc payload")
        data = archive[offset:offset + size]
        offset = (offset + size + 3) & ~3
        if name == "TRAILER!!!":
            if size or any(archive[offset:]):
                raise ValueError("Unexpected data after newc trailer")
            return records
        records.append({"name": name, "mode": mode, "data": data,
                        "major": fields[9], "minor": fields[10]})
    raise ValueError("Missing newc trailer")


def config_summary(config: bytes) -> bytes:
    lines = []
    values = {}
    for line in config.decode().splitlines():
        match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.*)", line)
        disabled = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line)
        key = match[1] if match else disabled[1] if disabled else None
        if key:
            values[key] = match[2] if match else "n"
            if key in CONFIG_KEYS:
                lines.append(line)
    for key in ("CONFIG_BLK_DEV_INITRD", "CONFIG_RD_GZIP", "CONFIG_PSTORE", "CONFIG_PSTORE_CONSOLE", "CONFIG_PSTORE_RAM"):
        if values.get(key) != "y":
            raise ValueError("RAM kernel config requires " + key + "=y")
    if values.get("CONFIG_BLOCK", "n") != "n" or values.get("CONFIG_CMDLINE_FORCE", "n") != "n":
        raise ValueError("Expected module-free RAM smoke config: BLOCK and CMDLINE_FORCE must be disabled")
    return ("\n".join(lines) + "\n").encode()


def generate(profile: str, kernel_manifest: Path, busybox_path: Path,
             provenance_path: Path, init_path: Path, output_dir: Path) -> dict:
    kernel_raw = kernel_manifest.read_bytes()
    kernel = json.loads(kernel_raw)
    if kernel.get("profile") != profile or kernel.get("mode") != "ram":
        raise ValueError("Kernel manifest does not match the selected profile/mode")
    commit = kernel.get("source_commit", "")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Kernel manifest lacks a full source commit")
    image_path, config_path = kernel_manifest.parent / "Image", kernel_manifest.parent / "config"
    image, config = image_path.read_bytes(), config_path.read_bytes()
    image_info = kernel.get("image") or {}
    if digest(image) != image_info.get("sha256") or len(image) != image_info.get("bytes"):
        raise ValueError("Kernel Image does not match its manifest")
    if digest(config) != kernel.get("config_sha256"):
        raise ValueError("Kernel config does not match its manifest")
    summary = config_summary(config)
    busybox = busybox_path.read_bytes()
    elf = validate_static_arm64_elf(busybox)
    provenance_raw = provenance_path.read_bytes()
    provenance = json.loads(provenance_raw)
    if provenance.get("package", {}).get("A") != "aarch64":
        raise ValueError("BusyBox provenance must describe an aarch64 package")
    init = init_path.read_bytes()
    if not init.startswith(b"#!/bin/sh\n"):
        raise ValueError("Unexpected PID 1 script interpreter")
    metadata = (f"profile={profile}\nmode=ram\nsource_commit={commit}\n"
                f"kernel_release={kernel.get('kernel_release', 'unknown')}\n"
                f"kernel_config_sha256={digest(config)}\n"
                f"kernel_image_sha256={digest(image)}\n").encode()
    entries = [{"name": directory, "mode": stat.S_IFDIR | 0o755}
               for directory in ("bin", "sbin", "dev", "proc", "sys", "tmp", "etc", "etc/piano")]
    for name, major, minor, permissions in (("console", 5, 1, 0o600), ("null", 1, 3, 0o666), ("kmsg", 1, 11, 0o600)):
        entries.append({"name": "dev/" + name, "mode": stat.S_IFCHR | permissions,
                        "major": major, "minor": minor})
    entries += [{"name": "bin/busybox", "mode": stat.S_IFREG | 0o755, "data": busybox},
                {"name": "init", "mode": stat.S_IFREG | 0o755, "data": init},
                {"name": "etc/piano/kernel-build", "mode": stat.S_IFREG | 0o644, "data": metadata},
                {"name": "etc/piano/config-summary", "mode": stat.S_IFREG | 0o644, "data": summary},
                {"name": "etc/piano/busybox-source.json", "mode": stat.S_IFREG | 0o644, "data": provenance_raw}]
    entries += [{"name": "bin/" + name, "mode": stat.S_IFLNK | 0o777, "data": b"busybox"} for name in APPLETS]
    archive = make_newc(entries)
    records = inspect_newc(archive)
    if [row["name"] for row in records] != [row["name"] for row in entries]:
        raise ValueError("CPIO round-trip changed its entry list")
    compressed = gzip.compress(archive, mtime=0)
    if gzip.decompress(compressed) != archive:
        raise ValueError("Gzip round-trip failed")
    # Recheck shared input artifacts before publishing this derived payload.
    for path, original in ((kernel_manifest, kernel_raw), (image_path, image), (config_path, config),
                           (busybox_path, busybox), (provenance_path, provenance_raw), (init_path, init)):
        if digest(path.read_bytes()) != digest(original):
            raise ValueError("Input changed while building: " + str(path))
    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / "initramfs.cpio.gz"
    result = {
        "schema_version": 1, "profile": profile, "mode": "ram", "source_commit": commit,
        "kernel_release": kernel.get("kernel_release"), "status": "HOST_ARTIFACT_ONLY_NOT_BOOTED",
        "hardware_verified": False, "entrypoint": "/init", "modules": 0, "module_loading": False,
        "persistent_filesystem_mounts": False, "mount_types": ["devtmpfs", "proc", "sysfs", "tmpfs"],
        "recovery_timer_seconds": RECOVERY_SECONDS,
        "kernel_manifest": {"path": str(kernel_manifest.resolve()), "sha256": digest(kernel_raw)},
        "kernel_image": {"path": str(image_path.resolve()), "sha256": digest(image), "bytes": len(image)},
        "kernel_config": {"path": str(config_path.resolve()), "sha256": digest(config), "summary_sha256": digest(summary)},
        "busybox": {"path": str(busybox_path.resolve()), "sha256": digest(busybox), "bytes": len(busybox),
                    "elf": elf, "provenance": provenance, "provenance_sha256": digest(provenance_raw)},
        "init_source": {"path": str(init_path.resolve()), "sha256": digest(init)},
        "initramfs": {"path": str(output.resolve()), "sha256": digest(compressed), "bytes": len(compressed),
                      "format": "newc", "compression": "gzip", "uncompressed_sha256": digest(archive),
                      "uncompressed_bytes": len(archive), "entries": [row["name"] for row in records]},
        "limitations": ["Generation and archive checks do not prove a kernel boot.",
                        "The recovery timer starts only after PID 1 runs; panic recovery is a loader cmdline setting.",
                        "USB observation reads sysfs only; this init does not configure a gadget or load platform modules."],
    }
    output.write_bytes(compressed)
    (output_dir / "initramfs-manifest.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("stable", "next"), required=True)
    parser.add_argument("--mode", choices=("ram",), default="ram")
    parser.add_argument("--kernel-manifest", type=Path)
    parser.add_argument("--busybox", type=Path, default=ROOT / "build/linux-ram/busybox")
    parser.add_argument("--busybox-provenance", type=Path, default=ROOT / "build/linux-ram/busybox-source.json")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    artifacts = ROOT / "artifacts/kernels" / args.profile / args.mode
    try:
        result = generate(args.profile, args.kernel_manifest or artifacts / "manifest.json",
                          args.busybox, args.busybox_provenance, ROOT / "bootprofiles/kernel-ram/init",
                          args.output_dir or artifacts)
    except (ValueError, OSError, struct.error) as exc:
        parser.exit(2, f"kernel initramfs: {exc}\n")
    print(json.dumps({"profile": result["profile"], "status": result["status"],
                      "source_commit": result["source_commit"], "initramfs": result["initramfs"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
