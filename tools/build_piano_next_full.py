#!/usr/bin/env python3
"""Build/check the complete pinned Piano Next kernel and same-release modules.

This tool never switches/fetches Git refs, changes rescue pins, runs guest code,
selects Android userdata, or operates a device. Each build owns a new artifact
and output directory. A separately audited runtime board DTB remains required.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

import build_piano_full_kernel as full
from build_kernel import image_info

ROOT = Path(__file__).resolve().parents[1]
COMMIT = '498569101e34cbb6ec9c27ebe609949279a59bce'
OFFICIAL_BASE = '67f0943b394d920b6c142aad8c6af94340342ae7'
PUBLIC_FULL = '352508459733d3e6d349ea5581a8dd2fd8bb4180'
PUBLIC_BASE = '500df175a7f9e6bc1a9c328590ca5150f84f9ff0'
WORK = ROOT / 'build/kernel-worktrees/piano-next-full-audit'
REQUIRED_MODULES = (
    'panel_novatek_nt36532', 'ktz8866', 'nt36532e_ts', 'hid_nanosic_wn8030',
    'piano_mca', 'sc8541_charger', 'snd_soc_fs19xx', 'snd_soc_sc8280xp',
    'ath12k', 'hci_uart', 'pwrseq_qcom_wcn', 'msm', 'qcom_camss', 'qcom_iris',
    'ov32d40', 'ufs_qcom', 'qcom_q6v5_pas', 'arm_smmu',
)
# Proven ancestors of OFFICIAL_BASE; retained upstream rather than replayed.
UPSTREAM_OMITTED = {
    'ffa88de8ddb64067df49e4d9f253d09a9c247059': 'DSI packet multi-slice',
    'ce73a5db44e3d5f9c0c061f0868ae209b59605f1': 'MSM DSI multi-slice',
    'bb7c5d6f5b41d192fa81ce404e463f5d3ce70cb3': 'q6v5 handover',
    '34b8b2d78b6276dc2dc4ebc06625a39956f266e4': 'PAS attach',
    '4d084017589312a0da2bec3474ef2035c5f4a407': 'Q6APM TDM operations',
    'c41ac86802fc0a22a886915a43bcad2e8d482b02': 'QAIF clock rename',
    '0a9e00d5ebdfcf460902f463e765f737d3fe935e': 'TDM slot distinction',
    '8593dc5f052e791748eaa76397ad95b9e32edac3': 'SC8280 TDM error handling',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(work, *args):
    return subprocess.check_output(['git', '-C', str(work), *args], text=True).strip()


def ancestor(work, first, last):
    return subprocess.run(['git', '-C', str(work), 'merge-base', '--is-ancestor',
                           first, last], capture_output=True).returncode == 0


def source_proof(work=WORK, commit=COMMIT):
    """Require the frozen full migration lineage, clean bytes and public config."""
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Exact 40-digit canonical Next commit required')
    if git(work, 'rev-parse', 'HEAD') != commit:
        raise ValueError('Next source HEAD drifted')
    if not ancestor(work, COMMIT, commit) or not ancestor(work, OFFICIAL_BASE, COMMIT):
        raise ValueError('Next source must descend from the frozen complete migration')
    if git(work, 'status', '--porcelain=v1', '--untracked-files=all'):
        raise ValueError('Next source is dirty')
    # Hidden assume-unchanged/skip-worktree bits must not conceal source drift.
    flags = git(work, 'ls-files', '-v').splitlines()
    if any(row and (row[0].islower() or row[0] == 'S') for row in flags):
        raise ValueError('Hidden Git worktree flags prevent source-byte proof')
    rows = git(work, 'ls-files', '-s').splitlines()
    for row in rows:
        meta, name = row.split('\t', 1)
        mode, blob, stage = meta.split()
        path = work / name
        if stage != '0' or mode not in ('100644', '100755', '120000'):
            raise ValueError('Unsupported/unmerged tracked source entry: ' + name)
        if mode == '120000':
            if not path.is_symlink():
                raise ValueError('Tracked source symlink changed: ' + name)
            data = os.fsencode(os.readlink(path))
        else:
            if path.is_symlink() or not path.is_file():
                raise ValueError('Tracked source file missing/changed: ' + name)
            data = path.read_bytes()
        actual = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if actual != blob:
            raise ValueError('Tracked source bytes drifted: ' + name)
        if mode != '120000' and bool(path.stat().st_mode & 0o111) != (mode == '100755'):
            raise ValueError('Tracked source executable mode drifted: ' + name)
    for name, expected in full.SOURCE_PINS.items():
        if sha(work / name) != expected:
            raise ValueError('Pinned public full config changed: ' + name)
    for upstream_commit in UPSTREAM_OMITTED:
        if not ancestor(work, upstream_commit, OFFICIAL_BASE):
            raise ValueError('Missing positive upstream ancestry proof: ' + upstream_commit)
    patch_commits = git(work, 'rev-list', '--reverse', OFFICIAL_BASE + '..' + commit).splitlines()
    return {'source_commit': commit, 'official_base_commit': OFFICIAL_BASE,
            'complete_migration_baseline': COMMIT, 'public_full_source': PUBLIC_FULL,
            'public_full_base': PUBLIC_BASE, 'source_clean': True,
            'tracked_source_bytes_verified': len(rows),
            'canonical_device_patch_commits': patch_commits,
            'upstream_generic_patches_omitted': [
                {'commit': value, 'reason': reason, 'proven_official_base_ancestor': True}
                for value, reason in UPSTREAM_OMITTED.items()]}


def validate_modules(folder, release, required=REQUIRED_MODULES):
    """Actual ELF/vermagic/hash checks plus complete module and depmod closure."""
    rows, summary = full.seal_modules(folder, release)
    by_name = {}
    for row in rows:
        normalized = row['name'].replace('-', '_')
        if normalized in by_name:
            raise ValueError('Duplicate normalized module name: ' + normalized)
        by_name[normalized] = row
    missing = [name for name in required if name not in by_name]
    if missing:
        raise ValueError('Required hardware modules missing: ' + ', '.join(missing))
    for row in rows:
        for name in filter(None, row['depends'].split(',')):
            if name.replace('-', '_') not in by_name:
                raise ValueError('Missing modinfo module dependency: ' + name)
    library = folder / 'lib/modules' / release
    expected_paths = {str((folder / row['path']).relative_to(library)) for row in rows}
    indexed = set()
    for line in (library / 'modules.dep').read_text().splitlines():
        if ':' not in line:
            raise ValueError('Malformed modules.dep line')
        name, dependencies = line.split(':', 1)
        if name not in expected_paths or name in indexed:
            raise ValueError('Unknown/duplicate modules.dep module: ' + name)
        indexed.add(name)
        for dependency in dependencies.split():
            if dependency not in expected_paths:
                raise ValueError('Missing modules.dep dependency: ' + dependency)
    if indexed != expected_paths:
        raise ValueError('Incomplete modules.dep module index')
    summary.update(all_modinfo_dependencies_resolved=True, complete_modules_dep_verified=True)
    return rows, summary, {name: by_name[name]['path'] for name in required}


def input_hashes():
    paths = (ROOT / 'configs/linux/piano-full.config', Path(__file__),
             ROOT / 'tools/build_piano_full_kernel.py', ROOT / 'tools/build_kernel.py',
             ROOT / 'tools/prepare_linux_modules.py')
    return {str(path.relative_to(ROOT)): sha(path) for path in paths}


def check_hashes(inputs, tools):
    for name, value in inputs.items():
        path = Path(name)
        if path.is_absolute() or '..' in path.parts or sha(ROOT / path) != value:
            raise ValueError('Next input drifted: ' + name)
    for name, value in tools['sha256'].items():
        if sha(Path(tools['paths'][name])) != value:
            raise ValueError('Next tool drifted: ' + name)


def check_candidate(folder, work=WORK, commit=COMMIT):
    """Read-only validation of an existing sealed candidate; never writes it."""
    proof = source_proof(work, commit)
    state = json.loads((folder / 'manifest.json').read_text())
    if (state.get('source_commit') != commit or
            state.get('status') != 'HOST_BUILT_FULL_CANDIDATE_NOT_HARDWARE_VERIFIED' or
            state.get('hardware_verified') is not False or state.get('device_operation_performed') is not False):
        raise ValueError('Candidate identity/status mismatch')
    base = state.get('official_base_commit', state.get('upstream_commit'))
    if base != OFFICIAL_BASE:
        raise ValueError('Candidate official base mismatch')
    check_hashes(state['inputs'], state['toolchain'])
    public = (work / 'arch/arm64/configs/piano_rootfs.config').read_text()
    root_policy = state.get('root_policy', 'ram')
    command = full.command_line((ROOT / 'configs/linux/piano-full.config').read_text(), public, root_policy)
    config_path = folder / 'config'
    config_pin = state.get('config_sha256')
    if config_pin is None:
        legacy_pins = [value for name, value in state['inputs'].items() if name.endswith('/.config')]
        if len(legacy_pins) != 1:
            raise ValueError('Candidate config hash proof missing')
        config_pin = legacy_pins[0]
    if sha(config_path) != config_pin:
        raise ValueError('Candidate config bytes drifted')
    declared_patches = state.get('canonical_device_patch_commits', state.get('canonical_patch_commits'))
    if declared_patches != proof['canonical_device_patch_commits']:
        raise ValueError('Candidate patch lineage mismatch')
    requirements = full.validate_config(config_path.read_text(), public, command)
    if requirements != state['full_profile_requirements']:
        raise ValueError('Candidate requirements mismatch')
    if image_info(folder / 'Image') != state['image'] or not state['image']['efi_stub']:
        raise ValueError('Candidate Image drifted')
    rows, summary, required = validate_modules(folder / 'modules', state['kernel_release'])
    if rows != state['modules'] or required != state['required_hardware_modules']:
        raise ValueError('Candidate module set drifted')
    # Legacy first sealed candidate lacks the two new closure fields only.
    old_summary = dict(state['module_summary'])
    for name in ('all_modinfo_dependencies_resolved', 'complete_modules_dep_verified'):
        old_summary.setdefault(name, True)
    if summary != old_summary:
        raise ValueError('Candidate module metadata drifted')
    return {'status': 'SEALED_NEXT_CANDIDATE_RECHECKED_NOT_HARDWARE_VERIFIED',
            'source_proof': proof, 'image': state['image'],
            'module_summary': summary, 'requirements': len(requirements)}


def reserve_paths(work, out, artifacts):
    if (not work.is_relative_to(ROOT / 'build/kernel-worktrees') or
            not out.is_relative_to(ROOT / 'build/kernels/next-full') or
            not artifacts.is_relative_to(ROOT / 'artifacts/kernels/next-full')):
        raise ValueError('Next paths must use isolated workspace worktree/output/artifact directories')
    if out == ROOT / 'build/kernels/next-full' or artifacts == ROOT / 'artifacts/kernels/next-full':
        raise ValueError('Select a new named Next output/artifact directory')
    if out.exists() or artifacts.exists():
        raise ValueError('Next output/artifact directory exists; choose fresh paths')
    artifacts.mkdir(parents=True, exist_ok=False)
    out.mkdir(parents=True, exist_ok=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worktree', type=Path, default=WORK)
    parser.add_argument('--commit', default=COMMIT)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--root', default='ram')
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--check-only', type=Path, metavar='SEALED_CANDIDATE')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--artifacts', type=Path)
    args = parser.parse_args()
    work = args.worktree.resolve()
    if not 1 <= args.jobs <= 8:
        raise ValueError('Next jobs must be 1..8')
    if args.check_only:
        if args.configure_only or args.build_dir or args.artifacts:
            raise ValueError('Check-only cannot configure/build or write artifacts')
        print(json.dumps(check_candidate(args.check_only.resolve(), work, args.commit), indent=2))
        return
    proof = source_proof(work, args.commit)
    public = work / 'arch/arm64/configs/piano_rootfs.config'
    fragment = ROOT / 'configs/linux/piano-full.config'
    command_line = full.command_line(fragment.read_text(), public.read_text(), args.root)
    env, tools = full.toolchain()
    hashes = input_hashes()
    suffix = args.commit[:12] + '-' + uuid.uuid4().hex[:8]
    out = (args.build_dir or ROOT / 'build/kernels/next-full' / suffix).resolve()
    artifacts = (args.artifacts or ROOT / 'artifacts/kernels/next-full' / suffix).resolve()
    reserve_paths(work, out, artifacts)
    state = {**proof, 'profile': 'next-full', 'mode': 'complete-public-hardware',
             'build_id': str(uuid.uuid4()), 'source_worktree': str(work),
             'source_branch': git(work, 'branch', '--show-current'),
             'inputs': hashes, 'toolchain': tools, 'root_policy': args.root,
             'command_line': command_line, 'hardware_verified': False,
             'device_operation_performed': False, 'android_userdata_selected': False,
             'safe_pianoinit_external_bundle_required': True, 'dtb': None,
             'dtb_reason': 'A separately audited explicit complete folded runtime DTB is required',
             'status': 'CONFIGURING_NOT_BUILT'}
    pending = artifacts / 'pending.json'
    pending.write_text(json.dumps(state, indent=2) + '\n')
    command = ['make', '-C', str(work), 'O=' + str(out), 'ARCH=arm64', 'LLVM=1', 'LLVM_IAS=1']
    effective = out / 'piano-full.effective.config'
    effective_text = 'CONFIG_CMDLINE=' + json.dumps(command_line) + '\n'
    effective.write_text(effective_text)
    log = artifacts / 'build.log'
    config_hash = None

    def fresh():
        source_proof(work, args.commit)
        check_hashes(hashes, tools)
        if effective.read_text() != effective_text:
            raise ValueError('Effective Next root fragment drifted')
        if config_hash is not None and sha(out / '.config') != config_hash:
            raise ValueError('Generated Next config drifted')

    def run(argv):
        with log.open('a') as stream:
            subprocess.run([str(value) for value in argv], cwd=work, env=env,
                           stdout=stream, stderr=subprocess.STDOUT, check=True)

    try:
        fresh()
        run(command + ['piano_defconfig'])
        run(['bash', work / 'scripts/kconfig/merge_config.sh', '-m', '-O', out,
             out / '.config', public, effective])
        run(command + ['olddefconfig'])
        state['full_profile_requirements'] = full.validate_config(
            (out / '.config').read_text(), public.read_text(), command_line)
        config_hash = sha(out / '.config')
        state['config_sha256'] = config_hash
        state['kernel_release'] = (out / 'include/config/kernel.release').read_text().strip() if (out / 'include/config/kernel.release').exists() else None
        state['public_config_sha256'] = full.SOURCE_PINS
        state['public_bt_le_enabled'] = full.config_values((out / '.config').read_text()).get('CONFIG_BT_LE') == 'y'
        fresh()
        shutil.copyfile(out / '.config', artifacts / 'config')
        if args.configure_only:
            state['status'] = 'CONFIGURED_NOT_BUILT'
            (artifacts / 'manifest.json').write_text(json.dumps(state, indent=2) + '\n')
            pending.unlink()
            print(json.dumps({'status': state['status'], 'artifacts': str(artifacts),
                              'requirements': len(state['full_profile_requirements']),
                              'config_sha256': config_hash}, indent=2))
            return
        state['status'] = 'BUILDING_NOT_HARDWARE_VERIFIED'
        pending.write_text(json.dumps(state, indent=2) + '\n')
        run(command + [f'-j{args.jobs}', 'Image', 'modules'])
        fresh()
        state['image'] = image_info(out / 'arch/arm64/boot/Image')
        if not state['image']['efi_stub']:
            raise ValueError('Next complete kernel EFI stub missing')
        state['kernel_release'] = (out / 'include/config/kernel.release').read_text().strip()
        install = artifacts / 'modules'
        if install.exists():
            raise ValueError('Next module destination already exists')
        run(command + ['INSTALL_MOD_PATH=' + str(install), 'INSTALL_MOD_STRIP=1', 'modules_install'])
        fresh()
        state['modules'], state['module_summary'], state['required_hardware_modules'] = validate_modules(install, state['kernel_release'])
        for name, source in [('Image', out / 'arch/arm64/boot/Image'), ('System.map', out / 'System.map')]:
            shutil.copyfile(source, artifacts / name)
        state['all_modinfo_dependencies_resolved'] = True
        state['build_exit'] = 0
        state['status'] = 'HOST_BUILT_FULL_CANDIDATE_NOT_HARDWARE_VERIFIED'
        fresh()
        (artifacts / 'manifest.json').write_text(json.dumps(state, indent=2) + '\n')
        pending.unlink()
        print(json.dumps({'status': state['status'], 'artifacts': str(artifacts),
                          'image': state['image'], 'module_summary': state['module_summary']}, indent=2))
    except Exception as error:
        state['status'] = 'FAILED_NOT_SEALED'
        state['error'] = str(error)
        pending.write_text(json.dumps(state, indent=2) + '\n')
        raise


if __name__ == '__main__':
    main()
