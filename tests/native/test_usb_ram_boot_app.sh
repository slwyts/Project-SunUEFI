#!/usr/bin/env bash
# New actual-C integration entry; no prod edit/prepare/device/firmware build.
set -euo pipefail
piano_ram_app_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$piano_ram_app_root"
python3 -m unittest discover -s tests/unit -p test_usb_ram_boot_app.py -v
