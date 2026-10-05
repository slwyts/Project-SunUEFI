#!/usr/bin/env python3
"""Reproduce and verify the product wait/GUI hooks in pinned vendor sources.

Only host files are touched. Product builds must fingerprint ``verify`` output
and bind PianoProductPumpLib (not its default Null implementation) in the DSC.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PINS = {
    "upstream/Mu-Silicium": "66e7bd1e7bcb757d4b28629bd6409d7209d3b242",
    "upstream/Mu-Silicium/Mu_Basecore": "bb557081f80f4883ed832e34ab36bdca6ede1e10",
    "upstream/simple-init": "3d66a6e78d519dd050fbebde4db6c5ac933f9aa4",
}
BASE = "upstream/Mu-Silicium/Mu_Basecore/"
SI = "upstream/simple-init/"
PREVIOUS_WAIT_HOOK = """    // Product library binding is Null for legacy diagnostics. Mu all-TPL
    // wait behavior stays intact; APP work never runs at CALLBACK/NOTIFY.
    if (gEfiCurrentTpl == TPL_APPLICATION) {
      PianoProductPumpApplication (PIANO_PRODUCT_PUMP_WAIT_EVENT, 1000);
      if (!PianoProductPumpBootServicesAlive ()) {
        return EFI_ABORTED;
      }
      BOOLEAN ReturnCore = PianoProductReturnCoreRequested ();
      if (!PianoProductPumpBootServicesAlive () || ReturnCore) {
        return EFI_ABORTED;
      }
    }
"""
WAIT_HOOK = PREVIOUS_WAIT_HOOK.replace(
    "      PianoProductPumpApplication (PIANO_PRODUCT_PUMP_WAIT_EVENT, 1000);",
    "      EFI_STATUS ProductStatus = PianoProductPumpApplication (PIANO_PRODUCT_PUMP_WAIT_EVENT, 1000);")
WAIT_HOOK = WAIT_HOOK.replace(
    "      if (!PianoProductPumpBootServicesAlive () || ReturnCore) {",
    "      if (!PianoProductPumpBootServicesAlive () || ReturnCore || ProductStatus == EFI_ABORTED) {")
# One exact migration from the earlier local hook; unknown edits still fail.
LEGACY_WAIT_HOOK = PREVIOUS_WAIT_HOOK.replace("""      BOOLEAN ReturnCore = PianoProductReturnCoreRequested ();
      if (!PianoProductPumpBootServicesAlive () || ReturnCore) {
        return EFI_ABORTED;
      }
""", "")
HOOKS = (
    (BASE + "MdeModulePkg/Core/Dxe/Event/Event.c", '#include "Event.h"\n',
     '#include "Event.h"\n#include <Library/PianoProductPumpLib.h>\n'),
    (BASE + "MdeModulePkg/Core/Dxe/Event/Event.c", "  for ( ; ;) {\n    for (Index = 0; Index < NumberOfEvents; Index++) {",
     "  for ( ; ;) {\n" + WAIT_HOOK + "    for (Index = 0; Index < NumberOfEvents; Index++) {"),
    (BASE + "MdeModulePkg/Core/Dxe/DxeMain.inf", "[LibraryClasses]\n", "[LibraryClasses]\n  PianoProductPumpLib\n"),
    (BASE + "MdePkg/MdePkg.dec", "[LibraryClasses]\n", "[LibraryClasses]\n  PianoProductPumpLib|Include/Library/PianoProductPumpLib.h\n"),
    (BASE + "MdePkg/MdeLibs.dsc.inc", "[LibraryClasses]\n", "[LibraryClasses]\n  PianoProductPumpLib|MdePkg/Library/PianoProductPumpLibNull/PianoProductPumpLibNull.inf\n"),
    ("upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc",
     "!include MdePkg/MdeLibs.dsc.inc\n\n[LibraryClasses]\n",
     "!include MdePkg/MdeLibs.dsc.inc\n\n[LibraryClasses]\n  PianoProductPumpLib|MdePkg/Library/PianoProductPumpLibNull/PianoProductPumpLibNull.inf\n"),
    (SI + "src/gui/gui_init.c", '#include"gui/clipboard.h"\n', '#include"gui/clipboard.h"\n#include"piano_product_runtime.h"\n'),
    (SI + "src/gui/gui_init.c", "\twhile(gui_run){\n\t\t// 10 seconds inactive sleep",
     "\twhile(gui_run){\n\t\t// Explicit APP slice outside gui_lock; Null unless product enables it.\n\t\tpiano_product_gui_pump();\n\t\tif(!gui_run)break;\n\t\t// 10 seconds inactive sleep"),
    (SI + "src/gui/SimpleInitGUI.inf", "  gui_init.c\n", "  gui_init.c\n  piano_product_runtime.c\n"),
    (SI + "src/gui/SimpleInitGUI.inf", "[LibraryClasses]\n", "[LibraryClasses]\n  PianoProductPumpLib\n"),
)


def canonical_files(root=ROOT):
    source = root / "bootprofiles/product-pump"
    copies = {}
    for directory, destination in ((source / "Mu_Basecore", root / BASE),
                                   (source / "simple-init", root / SI)):
        for path in sorted(directory.rglob("*")):
            if path.is_file():
                copies[destination / path.relative_to(directory)] = path.read_bytes()
    copies[root / (BASE + "MdePkg/Include/Protocol/PianoProductRuntime.h")] = (
        root / "bootprofiles/uefi-app/Protocol/PianoProductRuntime.h").read_bytes()
    return copies


def transform(text, old, new, label):
    """One exact hook: refuse partial, duplicated, or altered installations."""
    if text.count(new) == 1:
        # Some insertion anchors are substrings of their replacement.
        without = text.replace(new, "", 1)
        if old in without:
            raise ValueError("duplicate hook anchor: " + label)
        return text
    if new in text or text.count(old) != 1:
        raise ValueError("unexpected or modified hook source: " + label)
    return text.replace(old, new, 1)


def prepare(root=ROOT, apply=False, check_pins=True):
    if check_pins:
        for directory, pin in PINS.items():
            actual = subprocess.check_output(["git", "-C", str(root / directory), "rev-parse", "HEAD"], text=True).strip()
            if actual != pin:
                raise ValueError("unexpected source commit: " + directory)
    desired = {}
    newline = {}
    for relative, old, new in HOOKS:
        path = root / relative
        if path not in desired:
            raw = path.read_bytes()
            newline[path] = b"\r\n" if b"\r\n" in raw else b"\n"
            desired[path] = raw.decode().replace("\r\n", "\n")
            if relative == BASE + "MdeModulePkg/Core/Dxe/Event/Event.c" and WAIT_HOOK not in desired[path]:
                for prior in (LEGACY_WAIT_HOOK,PREVIOUS_WAIT_HOOK):
                    legacy = "  for ( ; ;) {\n" + prior + "    for (Index = 0; Index < NumberOfEvents; Index++) {"
                    if desired[path].count(legacy) == 1:
                        desired[path] = desired[path].replace(legacy,"  for ( ; ;) {\n    for (Index = 0; Index < NumberOfEvents; Index++) {",1)
        desired[path] = transform(desired[path], old, new, relative)
    for relative, marker in (
        (BASE + "MdeModulePkg/Core/Dxe/Event/Event.c", WAIT_HOOK),
        (BASE + "MdeModulePkg/Core/Dxe/Event/Event.c", "PianoProductPumpApplication (PIANO_PRODUCT_PUMP_WAIT_EVENT, 1000);"),
        (SI + "src/gui/gui_init.c", '#include"piano_product_runtime.h"'),
        (SI + "src/gui/gui_init.c", "piano_product_gui_pump();"),
        (SI + "src/gui/SimpleInitGUI.inf", "  piano_product_runtime.c"),
    ):
        if desired[root / relative].count(marker) != 1:
            raise ValueError("duplicated product hook: " + relative)
    outputs = {path: value.replace("\n", newline[path].decode()).encode() for path, value in desired.items()}
    outputs.update(canonical_files(root))
    # Check every source before mutations; a mismatched tracked hook is fatal.
    if not apply:
        for path, expected in outputs.items():
            if not path.is_file() or path.read_bytes() != expected:
                raise ValueError("product hook not prepared or stale: " + str(path.relative_to(root)))
    else:
        for path, expected in outputs.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_bytes() != expected:
                path.write_bytes(expected)
    hashes = {str(path.relative_to(root)): hashlib.sha256(value).hexdigest() for path, value in sorted(outputs.items())}
    return {"source_pins": PINS, "files": hashes, "sha256": hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest(),
            "default_binding": "PianoProductPumpLibNull", "product_requires_real_library_override": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("apply", "verify"))
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    result = prepare(apply=args.operation == "apply")
    encoded = json.dumps(result, indent=2) + "\n"
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(encoded)
    print(encoded, end="")


if __name__ == "__main__":
    main()
