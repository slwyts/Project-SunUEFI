"""Apply a publication generation to native package recipes before building."""
import os
import re


ENVIRONMENT = 'SUNUEFI_OVERLAY_RELEASE'
GENERATION = r'([1-9][0-9]{0,19})\.([1-9][0-9]{0,9})'
PKGREL = re.compile(r'([1-9][0-9]*)(?:\.([0-9]+))?')
PKGBUILD_RELEASE = re.compile(
    r'''(?m)^([ \t]*pkgrel=)(['"]?)([1-9][0-9]*(?:\.[0-9]+)?)\2(?=(?:[ \t]+#.*)?[ \t]*$)''')


def generation():
    value = os.environ.get(ENVIRONMENT)
    if value is not None and not re.fullmatch(GENERATION, value):
        raise ValueError(f'{ENVIRONMENT} must be a positive run ID and attempt (RUN_ID.RUN_ATTEMPT)')
    return value


def debian_version(version):
    published = generation()
    return version + '+sunuefi.' + published if published is not None else version


def base_debian_version(version):
    return re.sub(r'\+sunuefi\.' + GENERATION + r'$', '', version)


def uses_debian_generation(version):
    return generation() is None or version == debian_version(base_debian_version(version))


def pacman_generation(published):
    run, attempt = map(int, published.split('.'))
    # Fixed widths preserve numeric ordering across run IDs and attempts.
    return f'{run:020d}{attempt:010d}'


def pacman_release(pkgrel):
    published = generation()
    if published is None:
        return pkgrel
    version = PKGREL.fullmatch(pkgrel)
    if not version:
        raise ValueError('Unsupported native pacman pkgrel: ' + pkgrel)
    major, minor = version.groups()
    # Arch accepts one numeric subrelease. Keep the upstream major and
    # subrelease ordering, and append the publication order inside that field.
    return f'{major}.{int(minor or 0) + 1}{pacman_generation(published)}'


def pacman_recipe(recipe):
    if generation() is None:
        return recipe
    matches = list(PKGBUILD_RELEASE.finditer(recipe))
    if len(matches) != 1:
        raise ValueError('A published PKGBUILD must declare one numeric pkgrel')
    start, end = matches[0].span(3)
    return recipe[:start] + pacman_release(matches[0][3]) + recipe[end:]


def uses_pacman_generation(version):
    published = generation()
    if published is None:
        return True
    pkgrel = PKGREL.fullmatch(version.rsplit('-', 1)[-1])
    if not pkgrel or not pkgrel[2]:
        return False
    generation_digits = pacman_generation(published)
    return len(pkgrel[2]) > len(generation_digits) and pkgrel[2].endswith(generation_digits)
