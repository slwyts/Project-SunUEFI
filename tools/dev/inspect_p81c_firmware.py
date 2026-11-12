#!/usr/bin/env python3
"""Inspect a P81C / Dialog SUOTA image on the host; never connect to a device.

Only Python's standard library is required. Optional exports are raw inputs for
disassemblers, not firmware intended for flashing. VA values use the DA1469x
application's remapped address space, not physical pen flash addresses.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from datetime import datetime, timezone
import zipfile
import zlib


REFERENCE = (
    "https://lpccs-docs.renesas.com/um-b-092-da1469x_software_platform_reference/"
    "User_guides/User_guides.html"
)
GROUPS = {
    "platform": ("Dialog BLE", "DA14", "da14697", "P81C_", "Xiaomi Focus Pen"),
    "ota": ("ble_suota", "SPOTAR", "suota p81c"),
    "haptics": ("haptic", "AW86224", "cdv2624", "SOFT_PRESS", "SOFT_RELEASE"),
    "pressure": ("press lib", "press-lib", "press cal", "press_adc", "psensor"),
    "squeeze_touch": ("bl7488d_platform", "bl6486_platform", "[APP-I:Touch]"),
    "inertial_air_mouse": ("icm42670p", "bmi325", "air_mouse", "sidespin"),
    "mipp": ("[APP-I:mipp]", "default mipp config", "uplink.id"),
    "power": ("app_bms", "charger_da14697", "cps4520", "sy6302", "CW2215"),
    "buses": ("drv_spi", "drv_i2c"),
}
VECTOR_NAMES = (
    "initial_stack", "reset", "nmi", "hard_fault", "mem_manage", "bus_fault",
    "usage_fault", "secure_fault", "reserved8", "reserved9", "reserved10",
    "svc", "debug_monitor", "reserved13", "pendsv", "systick",
)


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_input(path: Path) -> tuple[bytes, dict]:
    raw = path.read_bytes()
    source = {"name": path.name, "size": len(raw), "sha256": sha256(raw)}
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            candidates = [i for i in archive.infolist() if i.filename.lower().endswith(".img")]
            if len(candidates) != 1:
                raise ValueError("ZIP must contain exactly one .img entry")
            entry = candidates[0]
            if entry.file_size > 16 * 1024 * 1024:
                raise ValueError("IMG exceeds the 16 MiB inspection limit")
            data = archive.read(entry)  # zipfile verifies the member CRC.
            source["zip_entry"] = {
                "name": entry.filename, "size": len(data),
                "sha256": sha256(data), "crc32": f"{entry.CRC:08x}",
            }
    else:
        data = raw
    return data, source


def startup_tables(body: bytes, reset: int) -> dict | None:
    """Read tables only when the observed SDK startup opcode sequence matches.

    Thumb LDR literals locate the copy/zero tables. Table coordinates are
    decoded from this image rather than hardcoded from the 0.0.32 sample.
    """
    if reset + 0x90 > len(body):
        return None
    code = body[reset:reset + 0x90]
    if code[0x0e:0x10] != b"\xac\x42" or code[0x12:0x18] != b"\x21\x68\x62\x68\xa3\x68":
        return None

    def ldr_literal(relative: int, register: int) -> int:
        op = struct.unpack_from("<H", code, relative)[0]
        if op & 0xFF00 != 0x4800 | (register << 8):
            raise ValueError("unexpected startup LDR literal")
        literal = ((reset + relative + 4) & ~3) + ((op & 0xff) << 2)
        if literal + 4 > len(body):
            raise ValueError("startup literal outside image")
        return u32(body, literal)

    try:
        copy_start = ldr_literal(0x0a, 4)
        copy_end = ldr_literal(0x0c, 5)
        zero_start = ldr_literal(0x46, 3)
        zero_end = ldr_literal(0x48, 4)
    except ValueError:
        return None
    if not (copy_start <= copy_end <= len(body) and (copy_end - copy_start) % 12 == 0):
        raise ValueError("invalid startup copy table bounds")
    if not (zero_start <= zero_end <= len(body) and (zero_end - zero_start) % 8 == 0):
        raise ValueError("invalid startup zero table bounds")
    copies = []
    for offset in range(copy_start, copy_end, 12):
        src, dst, length = struct.unpack_from("<III", body, offset)
        if src + length > len(body) or not (0x20000000 <= dst <= dst + length <= 0x20080000):
            raise ValueError("invalid startup copy region")
        copies.append({"table_va": offset, "source_va": src, "destination_va": dst, "length": length})
    zeros = []
    for offset in range(zero_start, zero_end, 8):
        dst, length = struct.unpack_from("<II", body, offset)
        if not (0x20000000 <= dst <= dst + length <= 0x20080000):
            raise ValueError("invalid startup zero region")
        zeros.append({"table_va": offset, "destination_va": dst, "length": length})
    return {"decoder": "observed SDK Thumb startup literal sequence", "copies": copies, "zeros": zeros}


def command_tables(body: bytes, image_offset: int) -> list[dict]:
    """Locate table candidates; an entry's semantics still require code review.

    P81C's observed entry layout is [id:u8, payload_size:u8, 0:u16,
    Thumb function pointer:u32]. Use the gain id/length pair as an anchor,
    then require a substantial run of ordered ids and valid pointers.
    """
    def entry(offset: int) -> tuple[int, int, int] | None:
        if offset < 0 or offset + 8 > len(body):
            return None
        cmd, size, reserved, address = struct.unpack_from("<BBHI", body, offset)
        if reserved or size > 64 or not address & 1 or not 0x200 <= address - 1 < len(body):
            return None
        return cmd, size, address

    result = []
    for match in re.finditer(re.escape(b"\x5a\x01\x00\x00"), body):
        anchor = match.start()
        current = entry(anchor)
        if current is None or anchor % 4:
            continue
        start = end = anchor
        while (previous := entry(start - 8)) is not None and previous[0] < entry(start)[0]:
            start -= 8
        while (following := entry(end + 8)) is not None and following[0] > entry(end)[0]:
            end += 8
        if (end - start) // 8 + 1 < 8:
            continue
        entries = []
        for offset in range(start, end + 1, 8):
            cmd, size, address = entry(offset)
            entries.append({"command_id": cmd, "payload_size_candidate": size,
                            "table_file_offset": image_offset + offset, "handler_thumb_va": address,
                            "handler_file_offset": image_offset + (address & ~1)})
        result.append({"file_offset": image_offset + start,
                       "confidence": "layout candidate; individual handlers need disassembly / client comparison",
                       "entries": entries})
    return result


def inspect(data: bytes, source: dict) -> tuple[dict, bytes]:
    if len(data) < 42 or data[:2] != b"Qq":
        raise ValueError("not a Dialog Qq image")
    length, expected_crc = u32(data, 2), u32(data, 6)
    ivt_offset = u32(data, 30)
    if ivt_offset < 42 or ivt_offset + length != len(data) or length < 0x200:
        raise ValueError("header size / IVT offset / file length do not match")
    body = data[ivt_offset:]
    actual_crc = zlib.crc32(body) & 0xffffffff
    if actual_crc != expected_crc:
        raise ValueError(f"payload CRC32 mismatch: {actual_crc:08x} != {expected_crc:08x}")
    version = data[10:26].split(b"\0", 1)[0].decode("ascii")
    timestamp = u32(data, 26)
    # Section identifiers are byte tags AA 22 / AA 44; lengths are LE16.
    position = 34
    extensions = []
    for tag, name in ((b"\xaa\x22", "security"), (b"\xaa\x44", "administration")):
        if position + 4 > ivt_offset or data[position:position + 2] != tag:
            raise ValueError(f"missing {name} header section")
        size = struct.unpack_from("<H", data, position + 2)[0]
        if position + 4 + size > ivt_offset:
            raise ValueError(f"{name} section exceeds IVT offset")
        extensions.append({"name": name, "file_offset": position, "tag_hex": tag.hex(), "length": size})
        position += 4 + size
    initial_sp, reset_vector = u32(body, 0), u32(body, 4)
    if not (0x20000000 <= initial_sp <= 0x20080000 and reset_vector & 1 and reset_vector - 1 < length):
        raise ValueError("vector table is not compatible with the observed Cortex-M image layout")
    tables = startup_tables(body, reset_vector & ~1)

    def va_to_file(value: int) -> int | None:
        address = value & ~1
        if address < len(body):
            return ivt_offset + address
        for region in (tables or {}).get("copies", []):
            dst = region["destination_va"]
            if dst <= address < dst + region["length"]:
                return ivt_offset + region["source_va"] + address - dst
        return None

    vectors = []
    for index in range(0x200 // 4):
        value = u32(body, index * 4)
        vectors.append({
            "index": index, "name": VECTOR_NAMES[index] if index < 16 else f"irq_{index - 16}",
            "table_file_offset": ivt_offset + index * 4, "value": value,
            "handler_file_offset": None if index == 0 or value == 0 else va_to_file(value),
        })
    strings = []
    for match in re.finditer(rb"[ -~]{6,}", body):
        value = match.group().decode("ascii")
        groups = [name for name, needles in GROUPS.items() if any(s in value for s in needles)]
        if not groups:
            continue
        address = match.start()
        # Pointer occurrences are candidate references, not proof of a function.
        needle = struct.pack("<I", address)
        refs = [ivt_offset + m.start() for m in re.finditer(re.escape(needle), body)]
        strings.append({
            "file_offset": ivt_offset + address, "remapped_va": address,
            "text": value, "groups": groups, "pointer_reference_file_offsets": refs,
        })
    copies = (tables or {}).get("copies", [])
    initialized_start = min((r["source_va"] for r in copies), default=len(body))
    sections = [
        {"name": "image_header", "file_offset": 0, "length": ivt_offset, "kind": "container"},
        {"name": "vector_table", "file_offset": ivt_offset, "length": 0x200, "remapped_va": 0, "kind": "vectors"},
        {"name": "flash_code_constants_embedded_data", "file_offset": ivt_offset + 0x200,
         "length": initialized_start - 0x200, "remapped_va": 0x200, "kind": "mixed_not_pure_code"},
    ]
    for index, region in enumerate(copies):
        sections.append({"name": f"ram_initialization_{index}", "file_offset": ivt_offset + region["source_va"],
                         "length": region["length"], "runtime_va": region["destination_va"], "kind": "copied_code_or_data"})
    manifest = {
        "schema": 1, "source": source, "image_sha256": sha256(data), "image_size": len(data),
        "header": {"magic": "Qq", "version": version, "payload_length": length,
                   "payload_offset": ivt_offset, "crc32": f"{actual_crc:08x}", "crc32_valid": True,
                   "timestamp_unix": timestamp, "timestamp_utc": datetime.fromtimestamp(timestamp, timezone.utc).isoformat(),
                   "extensions": extensions,
                   "remaining_header_padding_is_ff": all(x == 0xff for x in data[position:ivt_offset])},
        "architecture": {"instruction_set": "Arm Thumb / Cortex-M compatible",
                         "platform_family_inference": "Dialog / Renesas DA1469x (Cortex-M33)",
                         "exact_chip_part": None,
                         "evidence": "Qq/AA22/AA44 header, Dialog BLE/DA14 strings, SDK startup and RAM map",
                         "reference": REFERENCE},
        "addressing": {"body_remapped_va": 0, "initial_stack_pointer": initial_sp,
                       "reset_vector": reset_vector, "reset_file_offset": va_to_file(reset_vector),
                       "physical_flash_address": None,
                       "note": "Physical QSPI location depends on boot product header / cache remap not included in this OTA file."},
        "startup_tables": tables, "sections": sections, "vectors": vectors, "interesting_strings": strings,
        "candidate_mipp_command_tables": command_tables(body, ivt_offset),
        "limitations": ["No ELF symbols or original section names.",
                        "Mixed range includes constants and other MCU payloads; do not disassemble all bytes as M33 instructions.",
                        "Driver strings identify code present, not the actual pen BOM or enabled hardware.",
                        "Empty security section and readable code do not reveal device OTP/security configuration."],
    }
    return manifest, body


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="local .img or single-image .zip")
    parser.add_argument("--output", type=Path, help="write the JSON manifest here")
    parser.add_argument("--export-dir", type=Path, help="export raw analysis ranges and manifest here")
    args = parser.parse_args()
    try:
        data, source = read_input(args.image)
        manifest, body = inspect(data, source)
        encoded = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(encoded)
        if args.export_dir:
            args.export_dir.mkdir(parents=True, exist_ok=True)
            (args.export_dir / "manifest.json").write_text(encoded)
            (args.export_dir / "body-remapped-00000000.bin").write_bytes(body)
            for section in manifest["sections"]:
                offset, length = section["file_offset"], section["length"]
                (args.export_dir / f"{section['name']}.bin").write_bytes(data[offset:offset + length])
            rows = ["file_offset\tremapped_va\tgroup\ttext"]
            for item in manifest["interesting_strings"]:
                rows.append(f"0x{item['file_offset']:x}\t0x{item['remapped_va']:x}\t{','.join(item['groups'])}\t{item['text']}")
            (args.export_dir / "interesting-strings.tsv").write_text("\n".join(rows) + "\n")
        if not args.output and not args.export_dir:
            print(encoded, end="")
        else:
            header = manifest["header"]
            print(f"{header['version']}: {manifest['image_size']} bytes, payload CRC32 {header['crc32']} OK, "
                  f"reset file offset 0x{manifest['addressing']['reset_file_offset']:x}")
        return 0
    except (ValueError, OSError, UnicodeError, struct.error, zipfile.BadZipFile) as error:
        parser.exit(1, f"inspect_p81c_firmware: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
