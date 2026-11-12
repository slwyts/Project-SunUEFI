#!/usr/bin/env python3
"""Resolve core source files by name regardless of their subsystem directory.

Core sources are grouped by subsystem under uefi/core, but tools and the
generated INF refer to them by bare filename. Keeping the lookup in one place
means a file can move between subsystems without touching every caller.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'uefi/core'


def core_source(name, root=None):
    base = Path(root) / 'uefi/core' if root is not None else CORE
    direct = base / name
    if direct.is_file():
        return direct
    matches = sorted(path for path in base.rglob(Path(name).name) if path.is_file())
    if not matches:
        raise ValueError('Core source not found: ' + name)
    if len(matches) > 1:
        raise ValueError('Core source name is ambiguous: ' + name + ' -> ' +
                         ', '.join(str(path.relative_to(base)) for path in matches))
    return matches[0]


def has_core_source(name, root=None):
    """True when a core file of this name exists, without raising."""
    try:
        core_source(name, root)
    except ValueError:
        return False
    return True


def core_names(root=None):
    base = Path(root) / 'uefi/core' if root is not None else CORE
    return {path.name for path in base.rglob('*') if path.is_file()}


def core_include_dirs(root=None):
    """Every core subsystem directory, plus core itself for Protocol/."""
    base = Path(root) / 'uefi/core' if root is not None else CORE
    return [base, *sorted(path for path in base.iterdir() if path.is_dir() and path.name != 'Protocol')]


def core_include_flags(root=None):
    """Compiler arguments that resolve bare core header names."""
    flags = []
    for path in core_include_dirs(root):
        flags += ['-I', str(path)]
    return flags

