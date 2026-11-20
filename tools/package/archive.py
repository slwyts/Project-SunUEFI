"""Read metadata from native Debian and pacman archives."""
import subprocess


def debian_identity(path):
    output = subprocess.check_output(
        ['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture'], text=True)
    fields = dict(line.split(': ', 1) for line in output.splitlines())
    return fields['Package'], fields['Version'], fields['Architecture']


def pacman_info(archive):
    text = subprocess.check_output(['bsdtar', '-xOf', archive, '.PKGINFO'], text=True)
    fields = {}
    for line in text.splitlines():
        if ' = ' in line:
            name, value = line.split(' = ', 1)
            fields.setdefault(name, []).append(value)
    return fields


def pacman_identity(archive):
    fields = pacman_info(archive)
    names = ('pkgname', 'pkgver', 'arch')
    if any(len(fields.get(name, ())) != 1 for name in names):
        raise ValueError('Missing or repeated identity in ' + archive.name)
    return tuple(fields[name][0] for name in names)
