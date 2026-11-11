#!/usr/bin/env python3
"""Prepare normal distro packages; never modify the supplied source or install."""
import argparse
import hashlib
import json
from datetime import datetime, timezone
from email.utils import format_datetime
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[5]
PATCHES = ROOT / "patches/gnome-settings-daemon"

def hotplug_patch(version):
    directory = PATCHES / version
    series = json.loads((directory / 'series.json').read_text())
    row, = series['patches']
    path = directory / row['file']
    if hashlib.sha256(path.read_bytes()).hexdigest() != row['sha256']:
        raise ValueError('Keyboard backlight patch checksum differs')
    return path


def debian(source: Path, destination: Path) -> None:
    if not (source / "debian/changelog").is_file():
        raise SystemExit("Provide the unpacked Debian gnome-settings-daemon source package")
    first = (source / "debian/changelog").read_text().splitlines()[0]
    if not first.startswith("gnome-settings-daemon (48.1-"):
        raise SystemExit("This Debian patch is for GNOME 48.1; use a matching source package")
    version = first.split("(", 1)[1].split(")", 1)[0]
    if "+sunuefi" in version:
        raise SystemExit("Source is already a SunUEFI package; use the original Debian source")
    if (source / "debian/source/format").read_text().strip() != "3.0 (quilt)":
        raise SystemExit("Expected the standard Debian 3.0 (quilt) source format")
    shutil.copytree(source, destination, symlinks=True)
    patch_dir = destination / "debian/patches"
    patch_dir.mkdir(exist_ok=True)
    patch_name = "sunuefi-ambient-device-profile.patch"
    shutil.copyfile(PATCHES / "0001-power-optional-device-ambient-profile.patch", patch_dir / patch_name)
    hotplug_name = "sunuefi-keyboard-backlight-hotplug.patch"
    shutil.copyfile(hotplug_patch('48.1'), patch_dir / hotplug_name)
    series = patch_dir / "series"
    existing = series.read_text() if series.exists() else ""
    series.write_text(existing.rstrip() + "\n" + patch_name + "\n" + hotplug_name + "\n")
    changelog = destination / "debian/changelog"
    entry = (
        f"gnome-settings-daemon ({version}+sunuefi2) UNRELEASED; urgency=medium\n\n"
        "  * Add device ambient-light profiles and keyboard backlight hotplug.\n\n"
        f" -- slwyts <slwyts@foxmail.com>  {format_datetime(datetime.now(timezone.utc))}\n\n"
    )
    changelog.write_text(entry + changelog.read_text())
    # Apply all Debian patches through dpkg-source, so context failures are real.
    subprocess.run(["dpkg-source", "--before-build", str(destination)], check=True)
    print(f"Prepared {destination}; build there with dpkg-buildpackage -b -us -uc")


def arch(destination: Path) -> None:
    destination.mkdir(parents=True)
    shutil.copyfile(Path(__file__).with_name("PKGBUILD"), destination / "PKGBUILD")
    shutil.copyfile(PATCHES / "0001-power-optional-device-ambient-profile-51.patch",
                    destination / "ambient-device-profile.patch")
    shutil.copyfile(hotplug_patch('51.0'), destination / 'keyboard-backlight-hotplug.patch')
    # The recipe pins the public release and the locally prepared patch.
    recipe = destination / "PKGBUILD"
    checksum = hashlib.sha256((destination / "ambient-device-profile.patch").read_bytes()).hexdigest()
    hotplug_checksum = hashlib.sha256((destination / 'keyboard-backlight-hotplug.patch').read_bytes()).hexdigest()
    recipe.write_text(recipe.read_text().replace("@PATCH_SHA256@", checksum).replace(
        '@HOTPLUG_SHA256@', hotplug_checksum))
    print(f"Prepared {destination}; build there as an unprivileged user with makepkg -s")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("distro", choices=("debian", "arch"))
    parser.add_argument("destination", type=Path)
    parser.add_argument("--source", type=Path, help="Unpacked signed-APT Debian 48.1 source package")
    args = parser.parse_args()
    destination = args.destination.resolve()
    if destination.exists():
        parser.error("destination must be new; existing sources and builds are preserved")
    if args.distro == "debian":
        if args.source is None:
            parser.error("--source is required for Debian")
        debian(args.source.resolve(), destination)
    else:
        arch(destination)


if __name__ == "__main__":
    main()
