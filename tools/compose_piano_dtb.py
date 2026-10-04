#!/usr/bin/env python3
"""Compose an explicitly reviewed, disabled Piano display topology candidate.

No device access or automatic downloads. Inputs and allowable edits are bound
by SHA256. The small overlay is compiled with dtc and applied by fdtoverlay;
stock entry0 is never replayed. Unconverted providers remain unknown.
"""

import argparse
import copy
import hashlib
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_fdt(data):
    if len(data) < 40:
        raise ValueError("truncated FDT header")
    magic, total, structs, strings, reserve, version, last, cpu, string_size, struct_size = struct.unpack(">10I", data[:40])
    if magic != 0xd00dfeed or total != len(data) or version != 17 or last > 17:
        raise ValueError("invalid FDT magic, version or total size")
    if structs % 4 or reserve % 8 or min(structs, strings, reserve) < 40:
        raise ValueError("unaligned or overlapping FDT header")
    if structs + struct_size > total or strings + string_size > total or struct_size < 4:
        raise ValueError("FDT block outside file")
    if max(structs, strings) < min(structs + struct_size, strings + string_size):
        raise ValueError("overlapping FDT structure and strings")
    reservations = []
    pos = reserve
    while pos + 16 <= min(structs, strings):
        address, size = struct.unpack_from(">QQ", data, pos)
        pos += 16
        if address == size == 0:
            break
        reservations.append((address, size))
    else:
        raise ValueError("unterminated FDT reserve map")
    tree, stack, handles = {}, [], {}
    pos, end = structs, structs + struct_size
    while pos + 4 <= end:
        token = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if token == 1:
            stop = data.find(b"\0", pos, end)
            if stop < 0:
                raise ValueError("unterminated FDT node name")
            name = data[pos:stop].decode()
            if "/" in name or (not stack and (name or tree)):
                raise ValueError("invalid FDT root/node name")
            path = (stack[-1].rstrip("/") + "/" + name) if stack else "/"
            if path in tree:
                raise ValueError("duplicate FDT node: " + path)
            tree[path] = {}
            stack.append(path)
            pos = (stop + 4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError("unbalanced FDT nodes")
            stack.pop()
        elif token == 3:
            if not stack or pos + 8 > end:
                raise ValueError("invalid FDT property")
            size, nameoff = struct.unpack_from(">II", data, pos)
            pos += 8
            if nameoff >= string_size or pos + size > end:
                raise ValueError("FDT property outside block")
            stop = data.find(b"\0", strings + nameoff, strings + string_size)
            if stop < 0:
                raise ValueError("unterminated FDT property name")
            name = data[strings + nameoff:stop].decode()
            if name in tree[stack[-1]]:
                raise ValueError("duplicate FDT property")
            tree[stack[-1]][name] = data[pos:pos + size]
            pos = (pos + size + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            if stack or any(data[pos:end]):
                raise ValueError("unbalanced FDT end")
            break
        else:
            raise ValueError("unknown FDT structure token")
    else:
        raise ValueError("missing FDT end token")
    for path, props in tree.items():
        candidates = [props[key] for key in ("phandle", "linux,phandle") if key in props]
        if candidates:
            if any(len(value) != 4 for value in candidates) or len(set(candidates)) != 1:
                raise ValueError("invalid/mismatched phandle")
            value = int.from_bytes(candidates[0], "big")
            if value in (0, 0xffffffff) or value in handles:
                raise ValueError("invalid/duplicate phandle")
            handles[value] = path
    return {"tree": tree, "phandles": handles, "reservations": reservations, "boot_cpu": cpu}


def write_fdt(parsed):
    tree = parsed["tree"]
    names, strings, body = {}, bytearray(), bytearray()
    children = {path: [] for path in tree}
    for path in tree:
        if path != "/":
            parent = path.rsplit("/", 1)[0] or "/"
            if parent not in tree:
                raise ValueError("missing FDT parent")
            children[parent].append(path)
    def emit(path):
        body.extend(struct.pack(">I", 1))
        body.extend((path.rsplit("/", 1)[-1] if path != "/" else "").encode() + b"\0")
        body.extend(b"\0" * (-len(body) % 4))
        for name, data in tree[path].items():
            if name not in names:
                names[name] = len(strings)
                strings.extend(name.encode() + b"\0")
            body.extend(struct.pack(">III", 3, len(data), names[name]))
            body.extend(data)
            body.extend(b"\0" * (-len(body) % 4))
        for child in children[path]:
            emit(child)
        body.extend(struct.pack(">I", 2))
    emit("/")
    body.extend(struct.pack(">I", 9))
    reserve = b"".join(struct.pack(">QQ", *row) for row in parsed["reservations"]) + bytes(16)
    structure_offset = 40 + len(reserve)
    string_offset = structure_offset + len(body)
    header = struct.pack(">10I", 0xd00dfeed, string_offset + len(strings), structure_offset,
                         string_offset, 40, 17, 16, parsed["boot_cpu"], len(strings), len(body))
    return header + reserve + body + strings


def verify_references(parsed, paths):
    tree, handles = parsed["tree"], parsed["phandles"]
    for path in paths:
        props = tree[path]
        for key, data in props.items():
            single = key.endswith("-supply") or key in ("remote-endpoint", "backlight", "kinetic,secondary-backlight")
            gpio = key in ("gpios", "gpio", "reset-gpios", "enable-gpios")
            if not single and not gpio:
                continue
            if len(data) % 4 or not data:
                raise ValueError("invalid phandle reference: " + path + ":" + key)
            cells = list(struct.unpack(">" + "I" * (len(data) // 4), data))
            handle = cells[0]
            if handle not in handles:
                raise ValueError("unresolved phandle: " + path + ":" + key)
            target = handles[handle]
            if single and len(cells) != 1:
                raise ValueError("invalid single phandle")
            if gpio:
                count = tree[target].get("#gpio-cells")
                if count is None or len(count) != 4 or len(cells) != 1 + int.from_bytes(count, "big"):
                    raise ValueError("GPIO provider cell mismatch")
            if key == "remote-endpoint":
                back = tree[target].get("remote-endpoint")
                own = props.get("phandle") or props.get("linux,phandle")
                if own is None or back != own:
                    raise ValueError("non-reciprocal endpoint")


def check_delta(before, after, recipe):
    added = sorted(set(after) - set(before))
    if set(added) != set(recipe["new_nodes"]):
        raise ValueError("new node set differs from review")
    if set(before) - set(after):
        raise ValueError("overlay unexpectedly deleted base nodes")
    for path in added:
        if "compatible" in after[path] and after[path].get("status") != b"disabled\0":
            raise ValueError("new compatible device is not disabled")
    changes = []
    approved = set(tuple(row) for row in recipe["allowed_property_changes"])
    for path in before:
        for name in set(before[path]) | set(after[path]):
            if before[path].get(name) != after[path].get(name):
                if path == "/__symbols__" and name in recipe.get("overlay_symbols", []):
                    continue
                if (path, name) not in approved:
                    raise ValueError("unreviewed property change: " + path + ":" + name)
                changes.append({"path": path, "property": name,
                                "before_hex": before[path].get(name, b"").hex(),
                                "after_hex": after[path].get(name, b"").hex()})
    for path in recipe["disabled_nodes"]:
        if after[path].get("status") != b"disabled\0":
            raise ValueError("audit node is not disabled: " + path)
    return {"added_nodes": added, "property_changes": changes}


def compose(base_path, recipe_path, kernel_tree, output_dir, variant, dtc, fdtoverlay,
            schema=None, dt_validate=None):
    recipe_raw = recipe_path.read_bytes()
    recipe = json.loads(recipe_raw)
    base_raw = base_path.read_bytes()
    if sha(base_raw) != recipe["base_sha256"]:
        raise ValueError("base input SHA256 mismatch")
    base = read_fdt(base_raw)
    source = kernel_tree / recipe["overlay_source"]
    if sha(source.read_bytes()) != recipe["overlay_sha256"]:
        raise ValueError("overlay input SHA256 mismatch")
    canonical = subprocess.check_output(["git", "--no-lazy-fetch", "-C", str(kernel_tree), "show",
                                         recipe["overlay_commit"] + ":" + recipe["overlay_source"]])
    if sha(canonical) != recipe["overlay_sha256"]:
        raise ValueError("canonical overlay commit/content mismatch")
    header = kernel_tree / "include/dt-bindings/gpio/gpio.h"
    if sha(header.read_bytes()) != recipe["gpio_header_sha256"]:
        raise ValueError("GPIO header input SHA256 mismatch")
    if any(path in base["tree"] for path in recipe["new_nodes"]):
        raise ValueError("overlay node collision or already-applied candidate")
    prepared = copy.deepcopy(base)
    prepared["tree"].setdefault("/__symbols__", {})
    for alias, path in recipe["base_aliases"].items():
        if path not in base["tree"] or "phandle" not in base["tree"][path]:
            raise ValueError("base alias target missing a phandle: " + path)
        existing = prepared["tree"]["/__symbols__"].get(alias)
        expected = path.encode() + b"\0"
        if existing is not None and existing != expected:
            raise ValueError("base alias collision")
        prepared["tree"]["/__symbols__"][alias] = expected
    prepared_raw = write_fdt(prepared)
    round_trip = read_fdt(prepared_raw)
    if round_trip["tree"] != prepared["tree"] or round_trip["reservations"] != base["reservations"]:
        raise ValueError("base preparation changed semantics")
    with tempfile.TemporaryDirectory(prefix="piano-dtb-compose-") as directory:
        temporary = Path(directory)
        pp, overlay, input_base, output = [temporary / name for name in ("overlay.pp.dts", "overlay.dtbo", "base.dtb", "final.dtb")]
        args = ["cpp", "-nostdinc", "-undef", "-x", "assembler-with-cpp", "-I", str(kernel_tree / "include")]
        if variant == "csot":
            args.append("-DPIANO_PANEL_CSOT=1")
        subprocess.run(args + [str(source), "-o", str(pp)], check=True, capture_output=True)
        subprocess.run([str(dtc), "-@", "-I", "dts", "-O", "dtb", "-o", str(overlay), str(pp)], check=True, capture_output=True)
        read_fdt(overlay.read_bytes())
        input_base.write_bytes(prepared_raw)
        original = temporary / "original.dtb"
        original.write_bytes(base_raw)
        baseline_check = subprocess.run([str(dtc), "-I", "dtb", "-O", "dtb", "-o", str(temporary / "baseline.dtb"), str(original)], check=True, capture_output=True)
        subprocess.run([str(fdtoverlay), "-i", str(input_base), "-o", str(output), str(overlay)], check=True, capture_output=True)
        final_raw = output.read_bytes()
        final = read_fdt(final_raw)
        delta = check_delta(prepared["tree"], final["tree"], recipe)
        verify_references(final, recipe["reference_nodes"])
        if final["reservations"] != base["reservations"]:
            raise ValueError("reserved RAM changed")
        final_check = subprocess.run([str(dtc), "-I", "dtb", "-O", "dtb", "-o", str(temporary / "roundtrip.dtb"), str(output)], check=True, capture_output=True)
        if read_fdt((temporary / "roundtrip.dtb").read_bytes())["tree"] != final["tree"]:
            raise ValueError("dtc final tree round-trip changed properties")
        binding_review = {"status": "unknown", "reason": "scoped schema check not requested"}
        if schema is not None and dt_validate is not None:
            diagnostics = []
            for path in (original, output):
                check = subprocess.run([str(dt_validate), "-s", str(schema), "-l",
                                        "novatek,nt36532:kinetic,ktz8866", str(path)], capture_output=True)
                diagnostics.append((check.returncode, (check.stdout + check.stderr).decode(errors="replace").splitlines()))
            new_diagnostics = sorted(set(diagnostics[1][1]) - set(diagnostics[0][1]))
            binding_review = {"status": "scoped_check_no_new_diagnostics" if not new_diagnostics and diagnostics[1][0] == 0 else "unknown",
                              "schema_sha256": sha(schema.read_bytes()), "scope": ["novatek,nt36532", "kinetic,ktz8866"],
                              "baseline_diagnostics": diagnostics[0][1], "candidate_diagnostics": diagnostics[1][1],
                              "new_diagnostics": new_diagnostics, "candidate_returncode": diagnostics[1][0],
                              "limitations": "legacy global decoder diagnostics and non-display providers are not waived or certified"}
        if (sha(base_path.read_bytes()) != recipe["base_sha256"] or sha(source.read_bytes()) != recipe["overlay_sha256"] or
            sha(header.read_bytes()) != recipe["gpio_header_sha256"] or recipe_path.read_bytes() != recipe_raw):
            raise ValueError("inputs changed during composition")
        output_dir.mkdir(parents=True, exist_ok=True)
        artifact = output_dir / "piano-display-candidate.dtb"
        artifact.write_bytes(final_raw)
        normalize = lambda text, path: [re.sub(r"^[^:]+\.dtb(?=: Warning)", "<fdt>", line)
                                        for line in text.decode(errors="replace").splitlines()]
        baseline_warnings = normalize(baseline_check.stderr, original)
        final_warnings = normalize(final_check.stderr, output)
        metadata = {"schema_version": 1, "status": "DISABLED_DISPLAY_TOPOLOGY_CANDIDATE",
                    "hardware_verified": False, "enable_allowed": False, "variant": variant,
                    "base": {"path": str(base_path.resolve()), "sha256": sha(base_raw)},
                    "overlay": {"source": str(source.resolve()), "source_commit": recipe["overlay_commit"],
                                "sha256": sha(source.read_bytes()), "compiled_sha256": sha(overlay.read_bytes())},
                    "recipe_sha256": sha(recipe_raw), "final": {"path": str(artifact.resolve()), "sha256": sha(final_raw), "bytes": len(final_raw)},
                    "delta": delta, "base_aliases": recipe["base_aliases"],
                    "tools": {"dtc_sha256": sha(dtc.read_bytes()), "fdtoverlay_sha256": sha(fdtoverlay.read_bytes())},
                    "binding_review": binding_review,
                    "dtc_review": {"baseline_warning_lines": len(baseline_warnings),
                                   "final_warning_lines": len(final_warnings),
                                   "new_warning_lines": sorted(set(final_warnings) - set(baseline_warnings))},
                    "unresolved_integration": recipe["unresolved_integration"], "reference_sources": recipe["reference_sources"],
                    "excluded": ["stock entry0 replay", "rootfs", "UFS", "GPU", "audio", "video codec", "touch conversion", "USB conversion", "SMMU bypass script"]}
        (output_dir / "dtb-manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
        return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--review", type=Path, default=ROOT / "configs/linux/dtb/display-review.json")
    parser.add_argument("--kernel-tree", type=Path, default=Path("/home/slwyts/linux-piano-dtb"))
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/dtb/display-topology")
    parser.add_argument("--variant", choices=("boe", "csot"), required=True)
    tools_dir = ROOT / "build/kernel-topics/piano-panel/scripts/dtc"
    parser.add_argument("--dtc", type=Path, default=tools_dir / "dtc")
    parser.add_argument("--fdtoverlay", type=Path, default=tools_dir / "fdtoverlay")
    parser.add_argument("--schema", type=Path, help="explicit processed current-binding schema")
    parser.add_argument("--dt-validate", type=Path, help="optional scoped schema validator; use with --schema")
    args = parser.parse_args()
    try:
        if bool(args.schema) != bool(args.dt_validate):
            raise ValueError("schema and dt-validate must be supplied together")
        result = compose(args.base, args.review, args.kernel_tree, args.output, args.variant, args.dtc, args.fdtoverlay,
                         args.schema, args.dt_validate)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(2, "Piano DTB composition refused: " + str(exc) + "\n")
    print(json.dumps({"status": result["status"], "final": result["final"], "enable_allowed": False}, indent=2))


if __name__ == "__main__":
    main()
