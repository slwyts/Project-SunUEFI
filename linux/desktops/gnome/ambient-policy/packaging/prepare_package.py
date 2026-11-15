#!/usr/bin/env python3
"""Prepare normal distro packages; never modify the supplied source or install."""
import argparse
import hashlib
import json
from datetime import datetime, timezone
from email.utils import format_datetime
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[5]
PATCHES = ROOT / "patches/gnome-settings-daemon"
SERIES = PATCHES / '51.0/series.json'


def package_patches():
    series = json.loads(SERIES.read_text())
    paths = []
    for row in series['patches']:
        path = (SERIES.parent / row['file']).resolve()
        if hashlib.sha256(path.read_bytes()).hexdigest() != row['sha256']:
            raise ValueError('GSD patch checksum differs: ' + row['file'])
        paths.append(path)
    return paths


def debian(source: Path, destination: Path) -> None:
    if not (source / "debian/changelog").is_file():
        raise SystemExit("Provide the unpacked Debian gnome-settings-daemon source package")
    first = (source / "debian/changelog").read_text().splitlines()[0]
    version = first.split("(", 1)[1].split(")", 1)[0]
    if version.split('-', 1)[0] not in json.loads(SERIES.read_text())['versions']:
        raise SystemExit("The shared Debian/Arch patches require supported GNOME 51 source")
    if "+sunuefi" in version:
        raise SystemExit("Source is already a SunUEFI package; use the original Debian source")
    if (source / "debian/source/format").read_text().strip() != "3.0 (quilt)":
        raise SystemExit("Expected the standard Debian 3.0 (quilt) source format")
    shutil.copytree(source, destination, symlinks=True)
    patch_dir = destination / "debian/patches"
    patch_dir.mkdir(exist_ok=True)
    extra_names = []
    for path in package_patches():
        shutil.copyfile(path, patch_dir / path.name)
        extra_names.append(path.name)
    series = patch_dir / "series"
    existing = series.read_text() if series.exists() else ""
    series.write_text(existing.rstrip() + "\n" + "\n".join(extra_names) + "\n")
    changelog = destination / "debian/changelog"
    entry = (
        f"gnome-settings-daemon ({version}+sunuefi1) UNRELEASED; urgency=medium\n\n"
        "  * Add device ambient-light profiles and keyboard backlight hotplug.\n"
        "  * Restore the current screensaver state when power management starts.\n"
        "  * Use relative luminance for ambient changes and pause while blanked.\n\n"
        f" -- Project SunUEFI <sunuefi@users.noreply.github.com>  {format_datetime(datetime.now(timezone.utc))}\n\n"
    )
    changelog.write_text(entry + changelog.read_text())
    # Apply all Debian patches through dpkg-source, so context failures are real.
    subprocess.run(["dpkg-source", "--before-build", str(destination)], check=True)
    print(f"Prepared {destination}; build there with dpkg-buildpackage -b -us -uc")


def arch(destination: Path, rootfs: Path) -> None:
    result = subprocess.check_output(['chroot', str(rootfs), 'env', 'LC_ALL=C',
                                      'pacman', '-Si', 'gnome-settings-daemon'], text=True)
    repository_version = re.search(r'^Version\s*:\s*(\S+)$', result, re.M)[1]
    installed = subprocess.check_output(['chroot', str(rootfs), 'pacman', '-Q', 'gnome-settings-daemon'],
                                        text=True).split()[1]
    version, release = repository_version.rsplit('-', 1)
    if installed.rsplit('-', 1)[0] != version:
        raise ValueError('Update the target GSD to its repository version before preparing patches')
    data = json.loads(SERIES.read_text())
    if version not in data['sources']:
        raise ValueError('No verified shared GSD source for the target repository version: ' + version)
    origin = data['sources'][version]
    destination.mkdir(parents=True)
    shutil.copyfile(Path(__file__).with_name("PKGBUILD"), destination / "PKGBUILD")
    extra_patches = package_patches()
    for path in extra_patches:
        shutil.copyfile(path, destination / path.name)
    # The recipe pins the public release and the locally prepared patch.
    recipe = destination / "PKGBUILD"
    sources = ' '.join(path.name for path in extra_patches)
    checksums = ' '.join("'" + hashlib.sha256(path.read_bytes()).hexdigest() + "'"
                         for path in extra_patches)
    recipe.write_text(recipe.read_text().replace('@VERSION@', version).replace('@RELEASE@', release + '.1').replace(
        '@SOURCE_URL@', origin['url']).replace('@SOURCE_SHA256@', origin['sha256']).replace(
        '@PATCHES@', sources).replace('@PATCH_SHA256@', checksums))
    (destination / 'SOURCE.json').write_text(json.dumps({
        'repository_version': repository_version, 'upstream_version': version, 'source': origin,
        'patches': [{'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                    for path in extra_patches]}, indent=2) + '\n')
    print(f"Prepared {destination}; build there as an unprivileged user with makepkg -s")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("distro", choices=("debian", "arch"))
    parser.add_argument("destination", type=Path)
    parser.add_argument("--source", type=Path, help="Unpacked signed-APT Debian GNOME 51 source package")
    parser.add_argument('--rootfs', type=Path, help='Arch ARM64 root with the current signed pacman repository indexes')
    args = parser.parse_args()
    destination = args.destination.resolve()
    if destination.exists():
        parser.error("destination must be new; existing sources and builds are preserved")
    if args.distro == "debian":
        if args.source is None:
            parser.error("--source is required for Debian")
        debian(args.source.resolve(), destination)
    else:
        if args.rootfs is None:
            parser.error('--rootfs is required for Arch source and ABI selection')
        arch(destination, args.rootfs.resolve())


if __name__ == "__main__":
    main()
