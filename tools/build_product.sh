#!/usr/bin/env bash
# One host-built product integration candidate. No device/flash operation.
set -euo pipefail
piano_product_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$piano_product_root/build/logs" "$piano_product_root/artifacts/product"
python3 "$piano_product_root/tools/apply_firmware_patches.py"
# Stage the board DTB from the same fixed bundle used by product preparation.
python3 - "$piano_product_root" <<'PY_STAGE'
import sys, shutil
from pathlib import Path
root=Path(sys.argv[1])
sys.path.insert(0,str(root/'tools'))
from piano_vendor_inputs import load
bundle=load(root)
target=root/'upstream/Mu-Silicium/Resources/DTBs/piano.dtb'
target.parent.mkdir(parents=True,exist_ok=True)
target.write_bytes(bundle['board_dtb'])
PY_STAGE
python3 "$piano_product_root/tools/prepare_product_handoff.py" apply >/dev/null
rm -f "$piano_product_root/artifacts/product/build-ok.json" "$piano_product_root/artifacts/product/manifest.json"
python3 "$piano_product_root/tools/prepare_product_pump.py" apply >/dev/null
python3 "$piano_product_root/tools/prepare_product_ui.py" apply >/dev/null
python3 "$piano_product_root/tools/prepare_nv_runtime_guard.py" apply >/dev/null
bash "$piano_product_root/tools/build_simpleinit.sh" --product-gui-pump
python3 "$piano_product_root/tools/prepare_product.py"
bash "$piano_product_root/tools/build_stage0.sh" product
python3 "$piano_product_root/tools/package_product.py"
