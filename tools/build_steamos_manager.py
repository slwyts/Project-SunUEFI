#!/usr/bin/env python3
"""Rebuild SteamOS Manager from the pinned Valve and Rust releases."""
import hashlib
import json
from pathlib import Path
import shlex
import shutil

import build_arch_package
from build_piano_runtime_helpers import source_patches
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[1]
CONFIG = json.loads((ROOT / 'config/steamos.json').read_text())['manager']
SERIES = ROOT / 'patches/steamos-manager' / CONFIG['version'] / 'series.json'
RECIPE = ROOT / 'linux/bsp/steamos/manager/PKGBUILD'


def archives():
    commit = CONFIG['commit']
    rust = CONFIG['rust']
    yield ('manager.tar.gz',
           'https://gitlab.steamos.cloud/holo/steamos-manager/-/archive/'
           f'{commit}/steamos-manager-{commit}.tar.gz', CONFIG['archive_sha256'])
    for name, digest in rust['archives'].items():
        yield (name + '.tar.xz', f'https://static.rust-lang.org/dist/{rust["release_date"]}/'
               f'{name}-{rust["version"]}-aarch64-unknown-linux-gnu.tar.xz', digest)


def source_inputs():
    return {'commit': CONFIG['commit'], 'archives': tuple(archives()),
            'patches': {patch.name: hashlib.sha256(patch.read_bytes()).hexdigest()
                        for patch in source_patches(SERIES)},
            'recipe_sha256': hashlib.sha256(RECIPE.read_bytes()).hexdigest()}


def is_current(output):
    path = output / 'SOURCE.json'
    return build_arch_package.matches_release(output) and path.is_file() and (
        json.loads(path.read_text()) == json.loads(json.dumps(source_inputs())))


def build(root, output):
    inputs = source_inputs()
    recipe = output.parent / 'manager-recipe'
    recipe.mkdir(parents=True, exist_ok=True)
    for filename, url, checksum in inputs['archives']:
        download(url, recipe / filename, checksum)
    for patch in source_patches(SERIES):
        shutil.copy2(patch, recipe / patch.name)
    text = RECIPE.read_text()
    hashes = [*[row[2] for row in inputs['archives']], *inputs['patches'].values()]
    for key, value in {
        'VERSION': CONFIG['version'],
        'SOURCE_COMMIT': CONFIG['commit'],
        'RUST_VERSION': CONFIG['rust']['version'],
        'PATCHES': ' '.join(map(shlex.quote, inputs['patches'])),
        'SOURCE_HASHES': ' '.join(map(shlex.quote, hashes)),
    }.items():
        text = text.replace('@' + key + '@', value)
    (recipe / 'PKGBUILD').write_text(text)
    build_arch_package.build(root, recipe, output)
    (output / 'SOURCE.json').write_text(json.dumps(inputs, indent=2) + '\n')
