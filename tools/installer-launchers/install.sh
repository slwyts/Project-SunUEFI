#!/bin/sh
set -eu
installer_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
if [ -n "${SUNUEFI_PYTHON:-}" ]; then
    installer_python=$SUNUEFI_PYTHON
elif command -v python3 >/dev/null 2>&1; then
    installer_python=python3
elif command -v python >/dev/null 2>&1; then
    installer_python=python
else
    printf '%s\n' 'Python 3.10+ is required. Install Python and Android platform-tools, then try again.' >&2
    exit 2
fi
if ! "$installer_python" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'; then
    printf '%s\n' 'Python 3.10+ is required.' >&2
    exit 2
fi
exec "$installer_python" "$installer_dir/installer_launcher.py" "$@"
