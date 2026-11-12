#!/usr/bin/env python3
"""Prepare the fixed mobile Gamescope source and build a native SteamOS package."""
import hashlib
import configparser
import json
from pathlib import Path
import shutil
import subprocess
import time

import build_arch_package
import build_piano_runtime_helpers as runtime_helpers

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = 'e54dac93afccf76e5fefe998399b6c1b1a23f3a1'
UPSTREAM = 'ad2763da1c48860f649abfe842a087188dcb6e20'  # Valve 3.16.30
SUBPROJECTS = ('wlroots', 'libliftoff', 'vkroots', 'libdisplay-info')
SERIES = ROOT / 'patches/gamescope/series.json'
PATCHES = tuple(runtime_helpers.source_patches(SERIES))
RECIPE = ROOT / 'linux/bsp/steamos/gamescope/PKGBUILD'


def source_inputs():
    return {
        'mobile_commit': REFERENCE,
        'valve_commit': UPSTREAM,
        'patches': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in PATCHES},
        'recipe_sha256': hashlib.sha256(RECIPE.read_bytes()).hexdigest(),
        'preparer_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }


def is_current(output):
    record = output / 'SOURCE.json'
    return (output / 'manifest.json').is_file() and record.is_file() and (
        json.loads(record.read_text()).get('inputs') == source_inputs())


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()


def checkout(path, url, commit, sparse=None):
    if not (path / '.git').exists():
        subprocess.run(['git', 'clone', '--filter=blob:none', '--no-checkout', url, path],
                       check=True)
    if sparse:
        git(path, 'sparse-checkout', 'set', sparse)
    if subprocess.run(['git', '-C', path, 'cat-file', '-e', commit],
                      capture_output=True).returncode:
        git(path, 'fetch', '--depth=1', 'origin', commit)
    git(path, 'checkout', '--detach', commit)
    if git(path, 'rev-parse', 'HEAD') != commit:
        raise ValueError('Gamescope source pin mismatch')


def prepare(output):
    cache = ROOT / 'build/source-cache'
    reference, upstream = cache / 'steamos-arm-gamescope', cache / 'gamescope-upstream'
    checkout(reference, 'https://github.com/hashtagbasit/SteamOS-ARM-Port.git',
             REFERENCE, 'external-and-mods/gamescope')
    checkout(upstream, 'https://github.com/ValveSoftware/gamescope.git', UPSTREAM)
    for attempt in range(3):
        try:
            git(upstream, 'submodule', 'update', '--init', '--depth', '1',
                *('subprojects/' + name for name in SUBPROJECTS))
            break
        except subprocess.CalledProcessError:
            if attempt == 2:
                raise
            wait = 5 * (attempt + 1)
            print(f'Gamescope fixed subproject fetch failed; retrying in {wait}s '
                  f'({attempt + 2}/3).', flush=True)
            time.sleep(wait)
    output.mkdir(parents=True, exist_ok=True)
    source = output / 'gamescope-source'
    if source.exists():
        shutil.rmtree(source)
    shutil.copytree(reference / 'external-and-mods/gamescope', source,
                    ignore=shutil.ignore_patterns('.git', 'subprojects'))
    shutil.copytree(upstream / 'subprojects', source / 'subprojects',
                    ignore=shutil.ignore_patterns('.git'))
    wraps = {}
    for name in ('glm', 'stb'):
        wrap = configparser.ConfigParser()
        wrap.read(source / 'subprojects' / (name + '.wrap'))
        pin = wrap['wrap-git']
        cached = cache / ('gamescope-' + name)
        checkout(cached, pin['url'], pin['revision'])
        destination = source / 'subprojects' / name
        shutil.copytree(cached, destination, ignore=shutil.ignore_patterns('.git'))
        shutil.copytree(source / 'subprojects/packagefiles' / name,
                        destination, dirs_exist_ok=True)
        wraps[name] = {'url': pin['url'], 'commit': pin['revision']}
    for patch in PATCHES:
        subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '-p1',
                        '-i', str(patch)], cwd=source, check=True)
    provenance = {
        'mobile_source': {'url': 'https://github.com/hashtagbasit/SteamOS-ARM-Port',
                          'commit': REFERENCE},
        'valve_source': {'url': 'https://github.com/ValveSoftware/gamescope',
                        'commit': UPSTREAM},
        'subprojects': {name: git(upstream / 'subprojects' / name, 'rev-parse', 'HEAD')
                        for name in SUBPROJECTS},
        'wrap_subprojects': wraps,
        'inputs': source_inputs(),
    }
    (source / 'SUNUEFI-SOURCE.json').write_text(json.dumps(provenance, indent=2) + '\n')
    archive = output / 'gamescope-piano.tar.xz'
    subprocess.run(['tar', '--sort=name', '--mtime=@0', '--owner=0', '--group=0',
                    '-I', 'xz -T4 -1', '-cf', str(archive), '-C', str(output),
                    'gamescope-source'], check=True)
    with archive.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    recipe = RECIPE.read_text()
    (output / 'PKGBUILD').write_text(recipe.replace('@SOURCE_SHA256@', digest))
    (output / 'SOURCE.json').write_text(json.dumps(
        {**provenance, 'archive_sha256': digest}, indent=2) + '\n')
    shutil.rmtree(source)
    return provenance


def build(root, output):
    recipe = output.parent / 'gamescope-recipe'
    prepare(recipe)
    build_arch_package.build(root, recipe, output)
    shutil.copy2(recipe / 'SOURCE.json', output / 'SOURCE.json')
