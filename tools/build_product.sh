#!/usr/bin/env bash
# One host-built product integration candidate. No device/flash operation.
set -euo pipefail
piano_product_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$piano_product_root/build/logs" "$piano_product_root/artifacts/product"
rm -f "$piano_product_root/artifacts/product/build-ok.json" "$piano_product_root/artifacts/product/manifest.json"
python3 "$piano_product_root/tools/prepare_product_pump.py" apply >/dev/null
python3 "$piano_product_root/tools/prepare_product_ui.py" apply >/dev/null
bash "$piano_product_root/tools/build_simpleinit.sh" --product-gui-pump
python3 "$piano_product_root/tools/prepare_product.py"
bash "$piano_product_root/tools/build_stage0.sh" product
python3 "$piano_product_root/tools/package_product.py"
