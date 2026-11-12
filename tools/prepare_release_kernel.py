#!/usr/bin/env python3
"""Prepare the release kernel from a public baseline and tracked mail patches.

Host-only. No kernel build, device access, branch switch, or original-target
fetch is performed. Failed worktrees are preserved for inspection.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TARGET = 'release'
SERIES_FILE = 'patches/linux/7.3/series.json'
_release = json.loads((ROOT / SERIES_FILE).read_text())
TARGETS = {DEFAULT_TARGET: {
    **{key: _release[key] for key in (
        'base_commit', 'target_tree', 'original_target_commit', 'source_url')},
    'series_file': SERIES_FILE,
    'kernel_version': _release['kernel_version'],
    'worktree_name': 'release',
    'manifest_directory': 'release',
}}
COMMITTER_NAME = 'slwyts'
COMMITTER_EMAIL = 'slwyts@foxmail.com'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(repository, *args, env=None, check=True):
    result = subprocess.run(
        ['git', '-C', str(repository), *map(str, args)], env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if check and result.returncode:
        raise ValueError('Git command failed: ' + result.stderr.strip()[:6000])
    return result.stdout.strip() if check else result


def object_id(value, label):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{40}', value):
        raise ValueError('Invalid ' + label)
    return value


def load_series(root, target):
    if target not in TARGETS:
        raise ValueError('Unknown release kernel target: ' + str(target))
    policy = TARGETS[target]
    manifest_path = root / policy.get('series_file', 'patches/linux/series.json')
    folder = manifest_path.parent
    raw = manifest_path.read_bytes()
    series = json.loads(raw)
    if series.get('schema_version') != 1 or series.get('target') != policy.get('series_target', target):
        raise ValueError('Patch series schema/target mismatch')
    for key in ('base_commit', 'target_tree', 'original_target_commit', 'source_url'):
        expected = policy.get('patch_tree', policy['target_tree']) if key == 'target_tree' else policy[key]
        if series.get(key) != expected:
            raise ValueError('Patch series pin mismatch: ' + key)
    object_id(series.get('base_tree'), 'base tree')
    rows = series.get('patches')
    if not isinstance(rows, list) or not 1 <= len(rows) <= 256:
        raise ValueError('Invalid patch series length')
    if policy.get('patch_commits') is not None and [row.get('original_commit') for row in rows] != policy['patch_commits']:
        raise ValueError('Original patch commit sequence changed')
    previous = policy['base_commit']
    seen = set()
    patches = []
    for row in rows:
        name = row.get('file', '')
        if not re.fullmatch(r'[0-9]{4}-[A-Za-z0-9._-]+\.patch', name) or name in seen:
            raise ValueError('Invalid/duplicate patch filename')
        seen.add(name)
        commit = object_id(row.get('original_commit'), 'original commit')
        if row.get('original_parent') != previous:
            raise ValueError('Original patch parent chain changed')
        previous = commit
        object_id(row.get('tree'), 'patch tree')
        date = row.get('committer_date')
        if not isinstance(date, str):
            raise ValueError('Invalid original committer date')
        try:
            parsed_date = datetime.datetime.fromisoformat(date)
        except ValueError as exc:
            raise ValueError('Invalid original committer date') from exc
        if parsed_date.tzinfo is None:
            raise ValueError('Original committer date needs a timezone')
        path = folder / name
        if path.is_symlink() or path.resolve().parent != folder.resolve():
            raise ValueError('Patch path escapes its series directory')
        data = path.read_bytes()
        if sha(data) != row.get('sha256'):
            raise ValueError('Patch SHA256 mismatch: ' + name)
        if not data.startswith(('From ' + commit + ' Mon Sep 17 00:00:00 2001\n').encode()):
            raise ValueError('Patch original commit header mismatch: ' + name)
        patches.append((path, dict(row)))
    if previous != policy['original_target_commit'] or rows[-1]['tree'] != policy.get('patch_tree', policy['target_tree']):
        raise ValueError('Patch series target mismatch')
    return series, patches, sha(raw)


def is_repository(path):
    if not path.is_dir():
        return False
    bare = git(path, 'rev-parse', '--is-bare-repository', check=False)
    if bare.returncode:
        return False
    if bare.stdout.strip() == 'true':
        location = git(path, 'rev-parse', '--absolute-git-dir')
    else:
        location = git(path, 'rev-parse', '--show-toplevel')
    # An empty, uninitialized submodule directory would otherwise resolve to
    # the enclosing SunUEFI repository, polluting it with the kernel fetch.
    return Path(location).resolve() == path.resolve()


def ensure_repository(root, repository, policy):
    if repository is not None:
        repo = Path(repository).resolve()
        if not is_repository(repo):
            raise ValueError('Expected an explicit kernel repository root')
    else:
        # The kernel checkout is not tracked: only a disposable object cache is
        # kept under build/, and it is filled from the pinned source URL.
        repo = root / 'build/kernel-sources/linux-piano.git'
        if not is_repository(repo):
            if not repo.exists():
                repo.parent.mkdir(parents=True, exist_ok=True)
                result = subprocess.run(['git', 'init', '--bare', str(repo)], capture_output=True, text=True)
                if result.returncode:
                    raise ValueError('Cannot create release source repository: ' + result.stderr.strip())
            if not is_repository(repo):
                raise ValueError('Invalid project-local kernel source repository')
    required = [policy['base_commit']]
    if policy.get('upstream_base_commit'):
        required.append(policy['upstream_base_commit'])
    for commit in required:
        if git(repo, 'cat-file', '-e', commit + '^{commit}', check=False).returncode:
            # The URL is fixed in TARGETS; user remotes and original local
            # target commit are never used as fallback fetch sources.
            git(repo, 'fetch', '--no-tags', '--depth=128', policy['source_url'], commit)
        if git(repo, 'rev-parse', commit + '^{commit}') != commit:
            raise ValueError('Public baseline object mismatch')
    if policy.get('upstream_base_commit'):
        ancestor = policy['upstream_base_commit']
        if git(repo, 'merge-base', '--is-ancestor', ancestor, policy['base_commit'], check=False).returncode:
            # The fixed Piano tip has 61 commits after v7.2.6. Having both
            # objects is insufficient when the tip remains a shallow boundary.
            git(repo, 'fetch', '--no-tags', '--depth=128', policy['source_url'], policy['base_commit'])
        if git(repo, 'merge-base', '--is-ancestor', ancestor, policy['base_commit'], check=False).returncode:
            raise ValueError('Public Piano baseline ancestry is incomplete')
    if policy.get('stable_commit'):
        stable = policy['stable_commit']
        ancestry = git(repo, 'merge-base', '--is-ancestor', policy['upstream_base_commit'], stable, check=False)
        if ancestry.returncode:
            # Fetch only this fixed stable tip. Deepening every shallow ref is
            # unnecessary; 2048 commits cover the 1633 fixes after v7.2.6.
            git(repo, 'fetch', '--no-tags', '--depth=2048', policy['stable_url'], stable)
        if (git(repo, 'rev-parse', stable + '^{tree}') != policy['stable_tree'] or
                git(repo, 'merge-base', '--is-ancestor', policy['upstream_base_commit'], stable, check=False).returncode):
            raise ValueError('Stable source tree/ancestry mismatch')
    return repo


def clean(work):
    if git(work, 'status', '--porcelain=v1', '--untracked-files=all'):
        raise ValueError('Release kernel worktree is dirty')
    flags = git(work, 'ls-files', '-v').splitlines()
    if any(line and (line[0].islower() or line[0] == 'S') for line in flags):
        raise ValueError('Release kernel index hides working-tree changes')
    git_dir = Path(git(work, 'rev-parse', '--absolute-git-dir'))
    if (git_dir / 'rebase-apply').exists() or (git_dir / 'MERGE_HEAD').exists():
        raise ValueError('Release kernel patch application is incomplete')


def write_manifest(path, record):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    if temporary.exists():
        raise ValueError('Preserve pending source manifest: ' + str(temporary))
    temporary.write_text(json.dumps(record, indent=2) + '\n')
    temporary.replace(path)


def update_patches(root, policy):
    result = {}
    for key in ('conflict_resolution', 'cpu_model_patch', 'fastrpc_dma_patch', 'panel_depth_patch', 'flash_cleanup_patch', 'va_clock_order_patch', 'gcv2_backend_patch', 'gcv2_catalog_patch', 'dsi_stop_order_patch', 'dsi_fixed_bpc_patch', 'flash_torch_rounding_patch', 'va_mono_patch', 'dsi_video_phases_patch', 'dsi_post_hs_sync_patch', 'piano_panel_reset_patch', 'dsi_post_resync_reset_patch', 'piano_privacy_led_patch', 'nvt_panel_follower_patch', 'p81c_pressure_hid_patch', 'piano_preferred_120hz_patch', 'piano_function_row_patch', 'dsi_pll_registration_patch', 'dsi_slave_restore_patch', 'dpu_video_stop_order_patch', 'piano_pen_dock_patch', 'nvt_system_resume_patch', 'pcie_active_wakeup_patch', 'smp2p_sleepstate_patch', 'nanosic_backlight_default_patch', 'q6apm_global_lifetime_patch', 'fastrpc_freezable_invoke_patch', 'dsi_phy_runtime_clock_patch', 'backlight_disable_ramp_patch', 'mca_hyperos4_pd_patch', 'sc96231_pen_charger_patch', 'nvt_deferred_pen_mode_patch', 'piano_low_refresh_patch', 'piano_seamless_vfp_patch', 'piano_panel_vendor_order_patch', 'piano_igcv5_degamma_patch', 'iris_drc_patch', 'iris_deinit_irq_patch', 'qmp_optional_bridge_patch', 'piano_igcv5_precision_patch', 'gunyah_import_patch', 'gunyah_mainline_patch'):
        if key not in policy:
            continue
        row = policy[key]
        path = root / row['file']
        if path.is_symlink() or not path.resolve().is_relative_to(root / 'patches/linux') or sha(path.read_bytes()) != row['sha256']:
            raise ValueError('Stable update patch SHA/path mismatch: ' + row['file'])
        result[key] = path
    return result


def merge_stable(work, policy, paths):
    env = os.environ.copy()
    env.update({'GIT_AUTHOR_NAME': COMMITTER_NAME, 'GIT_AUTHOR_EMAIL': COMMITTER_EMAIL,
                'GIT_COMMITTER_NAME': COMMITTER_NAME, 'GIT_COMMITTER_EMAIL': COMMITTER_EMAIL,
                'GIT_AUTHOR_DATE': policy['merge_date'], 'GIT_COMMITTER_DATE': policy['merge_date']})
    before = git(work, 'rev-parse', 'HEAD')
    merged = git(work, '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null',
                 'merge', '--no-ff', '--no-commit', '--no-stat', policy['stable_commit'], env=env, check=False)
    conflicts = git(work, 'diff', '--name-only', '--diff-filter=U').splitlines()
    if merged.returncode:
        resolution = policy.get('conflict_resolution')
        if not resolution or conflicts != [resolution['source']]:
            raise ValueError('Stable merge failed: ' + merged.stderr.strip() + '; conflicts=' + repr(conflicts))
        # Resolve the one reviewed CCI overlap from our exact source, retaining
        # all other auto-merged stable changes already staged by Git.
        git(work, 'checkout', 'HEAD', '--', resolution['source'])
        git(work, 'apply', '--check', '--index', paths['conflict_resolution'])
        git(work, 'apply', '--index', paths['conflict_resolution'])
    if git(work, 'write-tree') != policy['stable_merged_tree']:
        raise ValueError('Stable merge result tree mismatch')
    git(work, '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null',
        'commit', '-m', 'merge: Linux ' + policy['stable_version'] + ' stable fixes', env=env)
    merge_commit = git(work, 'rev-parse', 'HEAD')
    parents = git(work, 'show', '-s', '--format=%P', merge_commit).split()
    if parents != [before, policy['stable_commit']]:
        raise ValueError('Stable merge parent provenance mismatch')
    metadata = {'source_url': policy['stable_url'], 'source_commit': policy['stable_commit'],
                'source_tree': policy['stable_tree'], 'version': policy['stable_version'],
                'merge_commit': merge_commit, 'parents': parents,
                'merged_tree': policy['stable_merged_tree'], 'resolved_conflicts': conflicts}
    for key, message, commit_key in (
        ('cpu_model_patch', 'fix(arm64): expose DT CPU model in cpuinfo', 'cpu_model_commit'),
        ('fastrpc_dma_patch', 'fix(iommu): use translated DMA for SM8750 FastRPC', 'fastrpc_dma_commit'),
        ('panel_depth_patch', 'fix(drm): report Piano DSC color depth', 'panel_depth_commit'),
        ('flash_cleanup_patch', 'fix(leds): correct qcom flash cleanup indexing', 'flash_cleanup_commit'),
        ('va_clock_order_patch', 'fix(asoc): start VA DMIC clocks before filter settling', 'va_clock_order_commit'),
        ('gcv2_backend_patch', 'feat(drm): add GCv2 REGDMA backend', 'gcv2_backend_commit'),
        ('gcv2_catalog_patch', 'feat(drm): bind SM8750 GCv2 capability', 'gcv2_catalog_commit'),
        ('dsi_stop_order_patch', 'fix(drm): stop bonded DSI slave before clock master', 'dsi_stop_order_commit'),
        ('dsi_fixed_bpc_patch', 'fix(drm): preserve actual DSC component depth', 'dsi_fixed_bpc_commit'),
        ('flash_torch_rounding_patch', 'fix(leds): preserve qcom torch current steps', 'flash_torch_rounding_commit'),
        ('dsi_video_phases_patch', 'fix(drm): stage DSI command and video through bridge phases', 'dsi_video_phases_commit'),
        ('dsi_post_hs_sync_patch', 'fix(drm): resynchronize SM8750 PHYs after HS clocks', 'dsi_post_hs_sync_commit'),
        ('va_mono_patch', 'fix(asoc): use real mono capture on Piano VA', 'va_mono_commit'),
        ('piano_panel_reset_patch', 'fix(drm): use factory reset sequence for Piano panels', 'piano_panel_reset_commit'),
        ('dsi_post_resync_reset_patch', 'fix(drm): reset SM8750 bonded controllers after PHY sync', 'dsi_post_resync_reset_commit'),
        ('piano_privacy_led_patch', 'feat(media): wire shared Piano camera privacy indicator', 'piano_privacy_led_commit'),
        ('nvt_panel_follower_patch', 'fix(input): detach NVT panel follower before cleanup', 'nvt_panel_follower_commit'),
        ('p81c_pressure_hid_patch', 'fix(hid): do not map P81C pressure to keyboard plus', 'p81c_pressure_hid_commit'),
        ('piano_preferred_120hz_patch', 'fix(panel): prefer Piano 120 Hz cold-start mode', 'piano_preferred_120hz_commit'),
        ('piano_function_row_patch', 'feat(hid): add Piano right Ctrl function row', 'piano_function_row_commit'),
        ('dsi_pll_registration_patch', 'fix(drm): defer unprogrammed DSI PLL during provider registration', 'dsi_pll_registration_commit'),
        ('dsi_slave_restore_patch', 'fix(drm): restore SM8750 bonded slave clock state', 'dsi_slave_restore_commit'),
        ('dpu_video_stop_order_patch', 'fix(drm): clean video pipeline before stopping frame boundaries', 'dpu_video_stop_order_commit'),
        ('piano_pen_dock_patch', 'feat(input): add Piano magnetic pen-dock switch', 'piano_pen_dock_commit'),
        ('nvt_system_resume_patch', 'fix(input): re-enable touch after system resume', 'nvt_system_resume_commit'),
        ('pcie_active_wakeup_patch', 'fix(clk): preserve SM8750 active PCIe wakeup domains', 'pcie_active_wakeup_commit'),
        ('smp2p_sleepstate_patch', 'feat(soc): notify the DSPs before system sleep', 'smp2p_sleepstate_commit'),
        ('nanosic_backlight_default_patch', 'feat(hid): default the Piano cover backlight on', 'nanosic_backlight_default_commit'),
        ('q6apm_global_lifetime_patch', 'fix(asoc): synchronize global APM readiness with removal', 'q6apm_global_lifetime_commit'),
        ('fastrpc_freezable_invoke_patch', 'fix(fastrpc): freeze in-flight invocation waiters in place', 'fastrpc_freezable_invoke_commit'),
        ('dsi_phy_runtime_clock_patch', 'fix(drm): manage DSI PHY interface clocks during runtime PM', 'dsi_phy_runtime_clock_commit'),
        ('backlight_disable_ramp_patch', 'fix(backlight): wait for the KTZ8866 disable ramp', 'backlight_disable_ramp_commit'),
        ('mca_hyperos4_pd_patch', 'fix(power): use the selected ADSP PD protocol', 'mca_hyperos4_pd_commit'),
        ('sc96231_pen_charger_patch', 'feat(power): supply the SC96231 pen charger through MCA', 'sc96231_pen_charger_commit'),
        ('nvt_deferred_pen_mode_patch', 'fix(input): apply sleeping pen-mode requests on resume', 'nvt_deferred_pen_mode_commit'),
        ('piano_low_refresh_patch', 'feat(drm): expose the factory low refresh modes', 'piano_low_refresh_commit'),
        ('piano_seamless_vfp_patch', 'feat(drm): update Piano vertical blanking without a modeset', 'piano_seamless_vfp_commit'),
        ('piano_panel_vendor_order_patch', 'fix(drm): preserve each panel vendor command order', 'piano_panel_vendor_order_commit'),
        ('piano_igcv5_degamma_patch', 'feat(drm): expose IGCv5 through standard degamma', 'piano_igcv5_degamma_commit'),
        ('piano_igcv5_precision_patch', 'feat(drm): expose the IGCv5 high precision grid', 'piano_igcv5_precision_commit'),
        ('iris_drc_patch', 'fix(media): preserve Iris PSC_LAST drain completion', 'iris_drc_commit'),
        ('iris_deinit_irq_patch', 'fix(media): quiesce Iris IRQ before teardown locking', 'iris_deinit_irq_commit'),
        ('qmp_optional_bridge_patch', 'fix(phy): register QMP display bridge only for a graph', 'qmp_optional_bridge_commit'),
        ('gunyah_import_patch', 'feat(virt): import Android Gunyah virtualization drivers', 'gunyah_import_commit'),
        ('gunyah_mainline_patch', 'fix(virt): adapt Gunyah to mainline kernel APIs', 'gunyah_mainline_commit'),
    ):
        if key not in paths:
            continue
        git(work, 'apply', '--check', '--index', paths[key])
        git(work, 'apply', '--index', paths[key])
        git(work, '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null',
            'commit', '-m', message, env=env)
        metadata[commit_key] = git(work, 'rev-parse', 'HEAD')
    makefile = (work / 'Makefile').read_text()
    fields = [re.search(r'^' + key + r' = (\d+)$', makefile, re.M)
              for key in ('VERSION', 'PATCHLEVEL', 'SUBLEVEL')]
    if not all(fields):
        raise ValueError('Prepared kernel Makefile version fields missing')
    version = '.'.join(field.group(1) for field in fields)
    if version != policy['stable_version']:
        raise ValueError('Prepared kernel Makefile version mismatch')
    return metadata


def prepare(root=ROOT, repository=None, worktree=None, source_manifest=None, target=DEFAULT_TARGET, refresh=False):
    root = Path(root).resolve()
    series, patches, series_hash = load_series(root, target)
    policy = TARGETS[target]
    paths = update_patches(root, policy)
    # Source snapshots follow their content, while one generated record selects
    # the current release. Updating a pin never resets an older checkout.
    default_name = policy.get('worktree_name', target) + '-' + policy['target_tree'][:12]
    work = Path(worktree or root / 'build/kernel-worktrees' / default_name).resolve()
    marker = Path(source_manifest or root / 'build' / policy.get('manifest_directory', 'release-kernel') / 'source-manifest.json').resolve()
    if not work.is_relative_to(root / 'build/kernel-worktrees'):
        raise ValueError('Release worktree must stay under project build/kernel-worktrees')
    if not marker.is_relative_to(root / 'build') or marker.is_relative_to(work):
        raise ValueError('Source manifest must stay under project build and outside the worktree')
    snapshot = work.with_name(work.name + '.source.json')
    if work.exists():
        current = json.loads(marker.read_text()) if marker.is_file() else None
        selected = (current is not None and current.get('worktree') == str(work) and
                    current.get('status') == 'SOURCE_PREPARED_NOT_BUILT')
        if not selected and not refresh:
            raise ValueError('Use --refresh to select a different prepared source snapshot')
        if not selected and not snapshot.is_file():
            raise ValueError('Existing release worktree has no completed source manifest; preserve it')
        record = current if selected else json.loads(snapshot.read_text())
        if (record.get('status') != 'SOURCE_PREPARED_NOT_BUILT' or record.get('target') != target or
                record.get('series_sha256') != series_hash or record.get('worktree') != str(work) or
                record.get('public_baseline') != policy['base_commit'] or
                record.get('original_target_commit') != policy['original_target_commit'] or
                record.get('actual_tree') != policy['target_tree']):
            raise ValueError('Existing release source manifest mismatch; preserve the worktree')
        clean(work)
        if git(work, 'rev-parse', 'HEAD') != record.get('actual_commit') or git(work, 'rev-parse', 'HEAD^{tree}') != policy['target_tree']:
            raise ValueError('Prepared release source HEAD/tree drifted')
        if git(work, 'merge-base', '--is-ancestor', policy['base_commit'], record['actual_commit'], check=False).returncode:
            raise ValueError('Prepared source no longer descends from the public baseline')
        if policy.get('stable_commit'):
            stable = record.get('stable_update', {})
            if (stable.get('source_commit') != policy['stable_commit'] or
                    stable.get('source_tree') != policy['stable_tree'] or
                    record.get('update_patches') != {key: policy[key] for key in paths} or
                    git(work, 'merge-base', '--is-ancestor', policy['stable_commit'], record['actual_commit'], check=False).returncode):
                raise ValueError('Prepared stable update provenance mismatch')
        if not snapshot.is_file():
            write_manifest(snapshot, record)
        if not selected:
            write_manifest(marker, record)
        return record
    if marker.exists():
        previous = json.loads(marker.read_text())
        if (not refresh or previous.get('status') not in ('SOURCE_PREPARED_NOT_BUILT', 'SOURCE_PREPARATION_FAILED') or
                previous.get('target') != target or previous.get('worktree') == str(work)):
            raise ValueError('Source record needs explicit --refresh for a completed older release snapshot')
    repo = ensure_repository(root, repository, policy)
    if git(repo, 'rev-parse', policy['base_commit'] + '^{tree}') != series['base_tree']:
        raise ValueError('Public baseline tree mismatch')
    record = {
        'schema_version': 1, 'status': 'SOURCE_PREPARATION_IN_PROGRESS', 'target': target,
        'source_url': policy['source_url'], 'public_baseline': policy['base_commit'],
        'base_tree': series['base_tree'], 'original_target_commit': policy['original_target_commit'],
        'expected_tree': policy['target_tree'], 'series_sha256': series_hash,
        'kernel_version': policy.get('kernel_version'),
        'repository': str(repo), 'worktree': str(work), 'patches': [],
        'tool_sha256': sha(Path(__file__).read_bytes()), 'kernel_built': False,
        'device_operation_performed': False,
    }
    if policy.get('stable_commit'):
        record['update_patches'] = {key: policy[key] for key in paths}
    current_patch = None
    try:
        work.parent.mkdir(parents=True, exist_ok=True)
        git(repo, '-c', 'core.hooksPath=/dev/null', 'worktree', 'add', '--detach', work, policy['base_commit'])
        for path, row in patches:
            current_patch = path.name
            env = os.environ.copy()
            env.update({'GIT_COMMITTER_NAME': COMMITTER_NAME, 'GIT_COMMITTER_EMAIL': COMMITTER_EMAIL,
                        'GIT_COMMITTER_DATE': row['committer_date']})
            git(work, '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null',
                '-c', 'am.threeWay=false', 'am', '--whitespace=error', path, env=env)
            applied_commit = git(work, 'rev-parse', 'HEAD')
            applied_tree = git(work, 'rev-parse', 'HEAD^{tree}')
            record.update(actual_commit=applied_commit, actual_tree=applied_tree)
            record['patches'].append({**row, 'applied_commit': applied_commit, 'applied_tree': applied_tree})
            if applied_tree != row['tree']:
                raise ValueError('Applied patch tree mismatch: ' + path.name)
        if policy.get('stable_commit'):
            current_patch = 'merge Linux ' + policy['stable_version']
            record['stable_update'] = merge_stable(work, policy, paths)
            record['kernel_version'] = record['stable_update']['version']
            record.update(actual_commit=git(work, 'rev-parse', 'HEAD'), actual_tree=git(work, 'rev-parse', 'HEAD^{tree}'))
        if git(work, 'rev-parse', 'HEAD^{tree}') != policy['target_tree']:
            raise ValueError('Final release kernel tree mismatch')
        clean(work)
        if load_series(root, target)[2] != series_hash:
            raise ValueError('Patch series changed during preparation')
        update_patches(root, policy)
        record['status'] = 'SOURCE_PREPARED_NOT_BUILT'
        write_manifest(snapshot, record)
        write_manifest(marker, record)
        return record
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        record.update(status='SOURCE_PREPARATION_FAILED', failing_patch=current_patch, error=str(exc))
        if work.exists() and not git(work, 'rev-parse', 'HEAD', check=False).returncode:
            record['actual_commit'] = git(work, 'rev-parse', 'HEAD')
            record['actual_tree'] = git(work, 'rev-parse', 'HEAD^{tree}')
        write_manifest(work.with_name(work.name + '.failure.json'), record)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, help='Existing kernel Git repository; default is project-local')
    parser.add_argument('--worktree', type=Path, help='Fresh worktree under project build/kernel-worktrees')
    parser.add_argument('--source-manifest', type=Path, help='New provenance record under project build')
    parser.add_argument('--target', default=DEFAULT_TARGET)
    parser.add_argument('--refresh', action='store_true',
                        help='Update the generated current-source record when the release pin changes; preserve older worktrees')
    args = parser.parse_args()
    try:
        result = prepare(repository=args.repository, worktree=args.worktree,
                         source_manifest=args.source_manifest, target=args.target, refresh=args.refresh)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(2, str(exc) + '\n')
    print(json.dumps({key: result[key] for key in ('status', 'worktree', 'actual_commit', 'actual_tree', 'public_baseline')}, indent=2))


if __name__ == '__main__':
    main()
