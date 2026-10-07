#!/usr/bin/env bash
# Independent actual-C host entry only; never prepare, firmware build or device.
set -euo pipefail
piano_ram_controller_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$piano_ram_controller_root"
python3 -m unittest discover -s tests/unit -p test_usb_ram_boot_controller.py -v
