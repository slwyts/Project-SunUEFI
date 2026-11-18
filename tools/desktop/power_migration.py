"""Retire the image-generated GNOME power helper on native package updates."""
from pathlib import Path
from shlex import quote


ROOT = Path(__file__).resolve().parents[2]
GNOME_OVERLAY = ROOT / 'linux/desktops/gnome/overlay'
POWER_DEFAULTS = 'etc/dconf/db/local.d/00-power'
SERVICE = 'usr/lib/systemd/user/piano-power-button.service'
SERVICE_LINK = 'etc/systemd/user/graphical-session.target.wants/piano-power-button.service'
HELPER = 'usr/lib/piano/power-button'

# The local product overlay and the upstream Debian image were both shipped
# without package ownership. Leave files outside these exact defaults alone.
IMAGE_FILES = {
    HELPER: (
        '5711ae9bd096157f96a68b4438d90c9dd5eb2a71f2566c907922e9ac977d6287',
        '938d0d9dbeccbfec9c3266e4108b86907c06fce61eef09fa1c80eb041ffd3f9b',
    ),
    SERVICE: (
        '1485dbfeccfcb07f90b1457bc3da736e85b9de4ceed345114daba9e2866dac2a',
        '700b087b73f8418e48e4eab177af6b89a51474a49467926485fd9a6910f0797b',
    ),
    'etc/udev/rules.d/70-piano-power-button.rules': (
        'c84a8b7a145fef4d426e3f8563c2c8ddda62061df1b46e921528a4de7356ef49',
        'f26025b1cfe9adca547d89b325eb60752b3430c9772f3a7173ec1b0222580ade',
    ),
    'etc/systemd/logind.conf.d/piano.conf': (
        'ea442362a3f043586c0a2e24a4422e3d0507c99a4548902baa583abd58d6ff9d',
        'a112f8d67286d5f9a2fd2beb3b3c10eca263060c9608fbe8813e2d256ebb15e3',
    ),
    'etc/piano/screen.conf': (
        '8d237e94bf6d3a74f48eb76a4069e8a08ad77fcea3c02ee10c520fa6361c2d1d',
    ),
    POWER_DEFAULTS: (
        'da9511ff19172086b223cf88f006f0bcec59f1a25ffb1461cc95e50c8fbe56db',
        '42be1174afbc96d1dc3384ef81fbcf60e7fd5b27c38a4d48ebb58e83eb732bf9',
    ),
}
RETIRED_OVERLAY = {*IMAGE_FILES, SERVICE_LINK}


def script(package_format):
    configs = ''.join(f'        /{path}) accepted="{ " ".join(digests)}";;\n'
                      for path, digests in IMAGE_FILES.items())
    owner_query = ('dpkg-query --search' if package_format == 'deb' else 'pacman -Qoq')
    body = f'''\
unowned() {{
    ! {owner_query} -- "$1" >/dev/null 2>&1
}}

image_default() {{
    [ ! -L "$1" ] && [ -f "$1" ] && unowned "$1" || return 1
    case "$1" in
{configs}        *) return 1;;
    esac
    checksum=$(sha256sum -- "$1") || return 1
    case " $accepted " in
        *" ${{checksum%% *}} "*) return 0;;
        *) return 1;;
    esac
}}

image_default_or_absent() {{
    if [ -e "$1" ] || [ -L "$1" ]; then
        image_default "$1"
    fi
}}

if image_default_or_absent /{HELPER} && image_default_or_absent /{SERVICE}; then
    link=/{SERVICE_LINK}
    if [ -L "$link" ] && [ "$(readlink -- "$link")" = '/{SERVICE}' ] && unowned "$link"; then
        rm -- "$link"
    fi
'''
    obsolete = ''.join(f' /{path}' for path in IMAGE_FILES if path != POWER_DEFAULTS)
    defaults = (GNOME_OVERLAY / POWER_DEFAULTS).read_text()
    body += f'''\
    for path in{obsolete}; do
        if image_default "$path"; then rm -- "$path"; fi
    done
    defaults=/{POWER_DEFAULTS}
    if image_default "$defaults"; then
        tmp=$(mktemp "$defaults.XXXXXX")
        trap 'rm -f -- "$tmp"' EXIT
        printf '%s' {quote(defaults)} > "$tmp"
        chmod 0644 "$tmp"
        mv -- "$tmp" "$defaults"
        trap - EXIT
        if command -v dconf >/dev/null; then dconf update; fi
    fi
fi
'''
    if package_format == 'deb':
        return '#!/bin/sh\nset -eu\n[ "$1" = configure ] || exit 0\n' + body
    return ('post_install() (\n    set -eu\n' + body +
            ')\n\npost_upgrade() { post_install; }\n')
