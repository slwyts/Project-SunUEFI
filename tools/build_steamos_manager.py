#!/usr/bin/env python3
"""Rebuild the matching Valve Manager with optional kernel trace collection."""
import hashlib
import json
from pathlib import Path
import shutil

import build_arch_package
from build_piano_runtime_helpers import source_patches
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[1]
COMMIT = 'a0d399defebd60db30ba38325d9cf47dbda49bee'
SERIES = ROOT / 'patches/steamos-manager/26.4.1/series.json'
RECIPE = ROOT / 'linux/bsp/steamos/manager/PKGBUILD'
INPUTS = (
    ('manager.tar.gz',
     f'https://gitlab.steamos.cloud/holo/steamos-manager/-/archive/{COMMIT}/steamos-manager-{COMMIT}.tar.gz',
     'e6d909ed5c8074ef7d12a84d6db8be3c759cd4283ab737edfe95ed3ab713d749'),
    ('rustc.tar.xz',
     'https://static.rust-lang.org/dist/2026-10-15/rustc-1.99.1-aarch64-unknown-linux-gnu.tar.xz',
     'acdcdfe09ae1f103aa9a2b7f88903a9588bc2e99288af10e6fbf5514e2e49f25'),
    ('rust-std.tar.xz',
     'https://static.rust-lang.org/dist/2026-10-15/rust-std-1.99.1-aarch64-unknown-linux-gnu.tar.xz',
     'e25668477083d6a9ddb87b9beb9c00742225f38233ccb4bfba3afa7a07427f52'),
    ('cargo.tar.xz',
     'https://static.rust-lang.org/dist/2026-10-15/cargo-1.99.1-aarch64-unknown-linux-gnu.tar.xz',
     '5156832e0c1254e9b49a79edabdc6d4c35f32f8fc1cf3ca700f268ff96087ab0'),
)


def source_inputs():
    patch, = source_patches(SERIES)
    return {'commit': COMMIT, 'archives': INPUTS,
            'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
            'recipe_sha256': hashlib.sha256(RECIPE.read_bytes()).hexdigest()}


def is_current(output):
    path = output / 'SOURCE.json'
    return build_arch_package.matches_release(output) and path.is_file() and (
        json.loads(path.read_text()) == json.loads(json.dumps(source_inputs())))


def build(root, output):
    patch, = source_patches(SERIES)
    recipe = output.parent / 'manager-recipe'
    recipe.mkdir(parents=True, exist_ok=True)
    for filename, url, checksum in INPUTS:
        download(url, recipe / filename, checksum)
    shutil.copy2(patch, recipe / patch.name)
    hashes = [INPUTS[0][2], source_inputs()['patch_sha256'],
              *[row[2] for row in INPUTS[1:]]]
    (recipe / 'PKGBUILD').write_text(RECIPE.read_text().replace(
        '@SOURCE_HASHES@', ' '.join("'" + value + "'" for value in hashes)))
    build_arch_package.build(root, recipe, output)
    (output / 'SOURCE.json').write_text(json.dumps(source_inputs(), indent=2) + '\n')
