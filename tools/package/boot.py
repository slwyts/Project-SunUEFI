#!/usr/bin/env python3
"""Build versioned Piano DTB and standard kernel-install integration packages."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BOOT_SPACE_HOOK = Path(__file__).with_name('85-piano-boot-space.install')
PACMAN_REPOSITORIES = ('sunuefi', 'sunuefi-steamos')
PUBLIC_REPOSITORY_URL = 'https://sunuefi-repo.okassets.uk'


def pacman_repository(name):
    if name not in PACMAN_REPOSITORIES:
        raise ValueError('Unknown Piano pacman repository: ' + name)
    return (f'[{name}]\nSigLevel = Required TrustedOnly\n'
            f'Server = {PUBLIC_REPOSITORY_URL}/pacman/$repo/os/$arch\n')


@contextmanager
def boot_package_workspace(build):
    # makepkg includes the recipe and build paths in .BUILDINFO.
    work = Path(build) / 'boot-packages'
    work.mkdir()
    try:
        yield work
    finally:
        shutil.rmtree(work)


def prepare_dtb(manifest, build):
    from package_release import (apply_cpu_model_overlay, normalize_mainline_bindings,
                                 prepare_dtb_includes, select_panel_vendor)
    policy = json.loads((ROOT / 'config/release.json').read_text())['kernel']
    with tempfile.TemporaryDirectory(prefix='kernel-dtb-', dir=build) as temporary:
        work = Path(temporary)
        dtb = work / 'board.dtb'
        shutil.copyfile(ROOT / 'vendor/piano-linux/board.dtb', dtb)
        apply_cpu_model_overlay(dtb, ROOT / policy['cpu_model_overlay'], work)
        includes, _ = prepare_dtb_includes(Path(manifest['source_worktree']), manifest['source_commit'], work)
        for overlay in policy['additional_dtb_overlays']:
            apply_cpu_model_overlay(dtb, ROOT / overlay, work, cpu_only=False, include_dirs=includes)
        normalize_mainline_bindings(dtb)
        select_panel_vendor(dtb, policy['preferred_display_mode']['panel_vendor'])
        return dtb.read_bytes()


def add_packages(plan, output, manifest, digest, *, image_modules=()):
    """Keep device configuration separate from the unmodified kernel recipe."""
    release = plan['kernel_release']
    name = 'linux-piano-dtb-' + release.lower().replace('_', '-')
    dtb = prepare_dtb(manifest, Path(plan['build']))
    version = plan['version']
    abi_version = plan['abi_version']
    epoch = str(plan['package_epoch'])
    arguments = [value for value in manifest['command_line'].split()
                 if value not in ('ro', 'rw') and
                 not value.startswith(('rdinit=', 'piano.root=', 'root='))]
    install = {
        'etc/kernel/install.conf': 'layout=bls\n',
        'usr/lib/kernel/devicetree': 'piano/board.dtb\n',
        'usr/lib/kernel/cmdline': ' '.join(dict.fromkeys(
            ['root=LABEL=PIANOROOT', 'rw', *arguments])) + '\n',
        f'usr/lib/kernel/install.d/{BOOT_SPACE_HOOK.name}': BOOT_SPACE_HOOK.read_text(),
        'usr/lib/kernel/install.d/89-piano-loaderentry-remove.install': (
            '#!/bin/sh\nset -eu\n'
            'case "$1:$2" in remove:*-piano-*) ;; *) exit 0 ;; esac\n'
            '[ "${KERNEL_INSTALL_LAYOUT:-}" != bls ] || exit 0\n'
            '[ "${KERNEL_INSTALL_BOOT_ENTRY_TYPE:-}" != type2 ] || exit 0\n'
            '# systemd skips Type 1 removal when another layout is selected.\n'
            'for plugin in /etc/kernel/install.d/90-loaderentry.install \\\n'
            '              /run/kernel/install.d/90-loaderentry.install \\\n'
            '              /usr/local/lib/kernel/install.d/90-loaderentry.install \\\n'
            '              /usr/lib/kernel/install.d/90-loaderentry.install; do\n'
            ' if [ -e "$plugin" ] || [ -L "$plugin" ]; then\n'
            '  [ -f "$plugin" ] && [ -x "$plugin" ] || exit 0\n'
            '  KERNEL_INSTALL_LAYOUT=bls "$plugin" "$@"\n'
            '  # Let kernel-install remove the matching BLS resource directory.\n'
            '  BOOT_ROOT="${KERNEL_INSTALL_BOOT_ROOT:?}" KERNEL_INSTALL_PLUGINS=/usr/bin/true \\\n'
            '   exec kernel-install --make-entry-directory=yes \\\n'
            '    --entry-token="literal:${KERNEL_INSTALL_ENTRY_TOKEN:?}" remove "$2"\n'
            ' fi\n'
            'done\n'),
    }
    # Debian's systemd-boot already supplies kernel and initramfs hooks.
    # Arch's kernel-install setup replaces the preset-based ALPM hooks while
    # retaining the distro's unmodified mkinitcpio and systemd plugins.
    if plan['format'] == 'debian':
        dependencies = ('systemd (>= 258), systemd-boot, systemd-boot-efi, '
                        'systemd-ukify, initramfs-tools')
        install['usr/share/initramfs-tools/hooks/piano'] = (
            '#!/bin/sh\nset -eu\n'
            'case "${1:-}" in prereqs) exit 0 ;; esac\n'
            '. /usr/share/initramfs-tools/hook-functions\n'
            'for module in arm_smmu ufs_qcom phy_qcom_qmp_ufs '
            'pinctrl_sm8750; do\n'
            ' manual_add_modules "$module"\ndone\n')
        install['etc/apt/sources.list.d/sunuefi.sources'] = (
            f'Types: deb\nURIs: {PUBLIC_REPOSITORY_URL}/apt\n'
            'Suites: sid\nComponents: main\nArchitectures: arm64\n'
            'Signed-By: /usr/share/keyrings/sunuefi-archive-keyring.gpg\n')
    else:
        dependencies = "'systemd>=258' systemd-ukify mkinitcpio"
        install['etc/kernel/install.conf'] += 'initrd_generator=mkinitcpio\n'
        install['etc/mkinitcpio.conf.d/zz-piano.conf'] = (
            'MODULES+=(arm_smmu ufs_qcom phy_qcom_qmp_ufs pinctrl_sm8750)\n'
            '# Valve recovery hooks expect Frame storage, firmware, and ADB.\n'
            'piano_hooks=()\n'
            'for piano_hook in "${HOOKS[@]}"; do\n'
            ' case "$piano_hook" in\n'
            '  blockdev_ro|partsets|initrd_shell|factory_reset|etc_overlay|deckard_fw) ;;\n'
            '  *) piano_hooks+=("$piano_hook") ;;\n'
            ' esac\n'
            'done\n'
            'HOOKS=("${piano_hooks[@]}")\n'
            'unset piano_hooks piano_hook\n')
        install['usr/share/libalpm/hooks/95-piano-kernel-install.hook'] = (
            '[Trigger]\nType = Path\nOperation = Install\nOperation = Upgrade\n'
            'Target = usr/lib/modules/*/vmlinuz\n\n'
            '[Trigger]\nType = Path\nOperation = Install\nOperation = Upgrade\nOperation = Remove\n'
            'Target = usr/lib/initcpio/*\nTarget = usr/lib/firmware/*\n'
            'Target = usr/lib/modules/*/extramodules/*\nTarget = usr/lib/modules/*/dtb/*\n'
            'Target = usr/lib/kernel/*\n'
            'Target = usr/src/*/dkms.conf\nTarget = usr/lib/modprobe.d/*\n'
            'Target = usr/lib/systemd/systemd\nTarget = usr/lib/systemd/systemd-udevd\n'
            'Target = usr/bin/cryptsetup\nTarget = usr/bin/lvm\nTarget = usr/bin/mdadm\n'
            'Target = usr/bin/pdata_tools\nTarget = usr/lib/libcryptsetup.so*\n'
            'Target = usr/lib/libp11-kit.so*\nTarget = usr/lib/libpcsclite.so*\n\n'
            '[Trigger]\nType = Package\nOperation = Install\nOperation = Upgrade\n'
            'Target = mkinitcpio\nTarget = mkinitcpio-git\n\n'
            '[Action]\nDescription = Installing kernel and initramfs...\n'
            'When = PostTransaction\nNeedsTargets\nExec = /usr/libexec/piano-kernel-transaction add\n')
        install['usr/share/libalpm/hooks/40-piano-kernel-remove.hook'] = (
            '[Trigger]\nType = Path\nOperation = Remove\n'
            'Target = usr/lib/modules/*/vmlinuz\n\n'
            '[Action]\nDescription = Removing kernel boot entries...\n'
            'When = PreTransaction\nAbortOnFail\nNeedsTargets\n'
            'Exec = /usr/libexec/piano-kernel-transaction remove\n')
        install['usr/libexec/piano-kernel-transaction'] = (
            '#!/bin/bash\nset -euo pipefail\nshopt -s nullglob\n'
            'action="${1:?}"; rebuild=0\ndeclare -A versions=()\n'
            'while IFS= read -r target; do\n'
            ' case "$target" in\n'
            '  usr/lib/modules/*/vmlinuz|usr/lib/modules/*/dtb/*|usr/lib/modules/*/extramodules/*)\n'
            '   version="${target#usr/lib/modules/}"\n'
            '   version="${version%%/*}"\n'
            '   if [[ -f "/usr/lib/modules/$version/vmlinuz" ]]; then versions["$version"]=1; fi ;;\n'
            '  *) rebuild=1 ;;\n'
            ' esac\n'
            'done\n'
            'if (( rebuild )); then\n'
            ' for image in /usr/lib/modules/*/vmlinuz; do\n'
            '  version="${image#/usr/lib/modules/}"; version="${version%/vmlinuz}"; versions["$version"]=1\n'
            ' done\n'
            'fi\n'
            'if [[ "${SUNUEFI_OFFLINE_ROOTFS:-0}" == 1 ]]; then\n'
            ' [[ "$action" == add ]] || exit 1\n'
            ' for version in "${!versions[@]}"; do depmod -a "$version"; done\n'
            ' exit 0\n'
            'fi\n'
            '(( ${#versions[@]} )) || exit 0\n'
            'if ! boot_root=$(bootctl --print-boot-path) || ! mountpoint -q "$boot_root"; then\n'
            ' echo "Mount the EFI boot partition before updating or removing a kernel." >&2\n'
            ' exit 1\n'
            'fi\n'
            'for version in "${!versions[@]}"; do\n'
            ' if [[ "$action" == add ]]; then\n'
            '  kernel-install add "$version" "/usr/lib/modules/$version/vmlinuz"\n'
            ' else\n'
            '  kernel-install remove "$version"\n'
            ' fi\n'
            'done\n')
        for repository in PACMAN_REPOSITORIES:
            install[f'etc/pacman.d/{repository}.conf'] = pacman_repository(repository)
    result = []
    with boot_package_workspace(plan['build']) as work:
        # The versioned device tree has its own ownership and can coexist with
        # older kernels; the normal kernel package owns in-tree generic DTBs.
        tree = work / 'dtb'
        destination = ('usr/lib/linux-image-' + release if plan['format'] == 'debian' else
                       'usr/lib/modules/' + release + '/dtb')
        (tree / destination / 'piano').mkdir(parents=True)
        (tree / destination / 'piano/board.dtb').write_bytes(dtb)
        trees = [(name, tree, 'Matched Piano device tree', '')]
        config = work / 'boot'
        for relative, text in install.items():
            path = config / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            path.chmod(0o755 if text.startswith('#!') else 0o644)
        if plan['format'] == 'pacman':
            for hook in ('60-mkinitcpio-remove.hook', '90-mkinitcpio-install.hook'):
                target = config / 'etc/pacman.d/hooks' / hook
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to('/dev/null')
        if plan['format'] == 'debian':
            keyring = config / 'usr/share/keyrings/sunuefi-archive-keyring.gpg'
        else:
            keyring = config / 'usr/share/pacman/keyrings/sunuefi.gpg'
            keyring.parent.mkdir(parents=True, exist_ok=True)
            fingerprint = subprocess.check_output([
                'gpg', '--batch', '--with-colons', '--show-keys',
                ROOT / 'config/keys/sunuefi-archive-keyring.gpg'], text=True)
            primary = next(row.split(':')[9] for row in fingerprint.splitlines() if row.startswith('fpr:'))
            (keyring.parent / 'sunuefi-trusted').write_text(primary + ':4:\n')
            (keyring.parent / 'sunuefi-revoked').touch()
        keyring.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / 'config/keys/sunuefi-archive-keyring.gpg', keyring)
        trees.append(('piano-boot-config', config, 'Piano standard kernel-install integration', dependencies))
        if plan['format'] == 'debian':
            for kind in ('image', 'headers'):
                meta = work / ('meta-' + kind)
                meta.mkdir()
                depends = f'linux-{kind}-{release} (= {abi_version}-1)'
                if kind == 'image':
                    depends += f', {name} (= {version}-1), piano-boot-config (>= {version}-1)'
                    depends += ''.join(f', {package} (= {revision})'
                                       for package, revision in image_modules)
                trees.append(('linux-' + kind + '-piano', meta,
                              'Latest Piano Linux ' + kind, depends))
        else:
            base = plan['environment']['PACMAN_PKGBASE']
            # Versioned native packages keep old modules/headers available for
            # the old boot entries. Stable metapackages select the latest generation.
            native_release = re.sub(r'-rc(\d+)', r'rc\1', release, count=1)
            native_version = native_release.replace('-', '_') + '-1'
            for kind in ('image', 'headers'):
                meta = work / ('meta-' + kind)
                meta.mkdir()
                depends = base + ('-headers' if kind == 'headers' else '') + '=' + native_version
                if kind == 'image':
                    depends += ' ' + name + '=' + version + "-1 'piano-boot-config>=" + version + "-1'"
                    depends += ''.join(f' {package}={revision}' for package, revision in image_modules)
                trees.append(('linux-piano' + ('-headers' if kind == 'headers' else ''),
                              meta, 'Latest Piano Linux ' + kind, depends))
        for package_name, payload, description, depends in trees:
            # Keep packaging timestamps fixed for this revision.
            for path in (*payload.rglob('*'), payload):
                if not path.is_symlink():
                    os.utime(path, (int(epoch), int(epoch)))
            env = dict(os.environ, SOURCE_DATE_EPOCH=epoch, LC_ALL='C', TZ='UTC')
            if plan['format'] == 'debian':
                control = payload / 'DEBIAN'
                control.mkdir()
                (control / 'control').write_text(
                    f'Package: {package_name}\nVersion: {version}-1\nArchitecture: all\n'
                    f'Maintainer: Project SunUEFI <slwyts@tuta.io>\nDescription: {description}\n' +
                    ('Depends: ' + depends + '\n' if depends else ''))
                conffiles = [path for path in payload.rglob('*')
                             if path.is_file() and path.relative_to(payload).parts[0] == 'etc']
                if conffiles:
                    (control / 'conffiles').write_text(''.join(
                        '/' + path.relative_to(payload).as_posix() + '\n' for path in conffiles))
                if package_name == name:
                    postinst = control / 'postinst'
                    postinst.write_text(
                        '#!/bin/sh\nset -eu\n'
                        '[ "$1" = configure ] && [ -n "${2:-}" ] || exit 0\n'
                        f'kernel=/boot/vmlinuz-{release}\n'
                        f'initrd=/boot/initrd.img-{release}\n'
                        '[ -f "$kernel" ] && [ -f "$initrd" ] || exit 0\n'
                        'command -v bootctl >/dev/null && bootctl is-installed --quiet || exit 0\n'
                        f'exec kernel-install add {release} "$kernel" "$initrd"\n')
                    postinst.chmod(0o755)
                archive = output / f'{package_name}_{version}-1_all.deb'
                subprocess.run(['dpkg-deb', '--root-owner-group', '--build', payload, archive],
                               env=env, check=True)
            else:
                archive_source = work / (package_name + '.tar')
                with tarfile.open(archive_source, 'w') as archive:
                    for path in sorted(payload.rglob('*')):
                        if path.is_file() or path.is_symlink():
                            info = archive.gettarinfo(path, arcname=path.relative_to(payload))
                            info.uid = info.gid = 0
                            info.uname = info.gname = 'root'
                            info.mtime = int(epoch)
                            if info.isfile():
                                with path.open('rb') as stream:
                                    archive.addfile(info, stream)
                            else:
                                archive.addfile(info)
                recipe = work / ('recipe-' + package_name)
                recipe.mkdir()
                shutil.copy2(archive_source, recipe / 'payload.tar')
                (recipe / 'PKGBUILD').write_text(
                    f'pkgname={package_name}\npkgver={version}\npkgrel=1\narch=(any)\n'
                    f'pkgdesc="{description}"\nlicense=(GPL-2.0-only)\n'
                    f'depends=({depends})\nsource=(payload.tar)\n'
                    + ('backup=(' + ' '.join(path.relative_to(payload).as_posix()
                       for path in sorted(payload.rglob('*'))
                       if path.is_file() and not path.is_symlink() and
                       path.relative_to(payload).parts[0] == 'etc') + ')\n'
                       if package_name == 'piano-boot-config' else '') +
                    f'sha256sums=({digest(recipe / "payload.tar")})\n'
                    'package() { bsdtar -xf "$srcdir/payload.tar" -C "$pkgdir"; }\n')
                env.update(PKGDEST=str(output), PKGEXT='.pkg.tar.zst', PACKAGER='Project SunUEFI <slwyts@tuta.io>')
                command = ['makepkg', '--noconfirm', '--nodeps']
                if os.geteuid() == 0:
                    subprocess.run(['chown', '-R', 'sunuefi-build:sunuefi-build', work], check=True)
                    command = ['runuser', '-u', 'sunuefi-build', '--', *command]
                subprocess.run(command, cwd=recipe, env=env, check=True)
                archive = output / f'{package_name}-{version}-1-any.pkg.tar.zst'
                if not archive.is_file():
                    raise ValueError('makepkg did not produce the expected package: ' + archive.name)
            result.append(archive)
    return result
