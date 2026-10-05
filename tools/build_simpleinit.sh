#!/usr/bin/env bash
# Host-only ARM64 UEFI simple-init build. Never contacts a device.
set -euo pipefail
sun_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sun_mu="$sun_root/upstream/Mu-Silicium"
sun_si="$sun_root/upstream/simple-init"
sun_build="$sun_root/build/simpleinit-edk2"
sun_output="$sun_root/artifacts/simpleinit"
sun_prepare_args=()
while (($#)); do
  case "$1" in
    --product-gui-pump)
      sun_prepare_args+=(--product-gui-pump)
      sun_build="$sun_root/build/simpleinit-product-edk2"
      sun_output="$sun_root/artifacts/simpleinit/product"
      shift;;
    --output-dir)
      [[ $# -ge 2 ]] || { echo '--output-dir requires a directory' >&2; exit 2; }
      sun_output="$2"; shift 2;;
    *) echo "Unknown argument: $1" >&2; exit 2;;
  esac
done
mkdir -p "$sun_output"
sun_output="$(cd -- "$sun_output" && pwd)"
rm -f "$sun_output/build-ok.json"
sun_tools="$sun_root/build/host-tools/usr"
export PATH="$sun_tools/bin:$sun_root/.venv/bin:$PATH"
export LD_LIBRARY_PATH="$sun_tools/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CLANGDWARF_BIN="$sun_tools/bin/"
export CLANG_BIN="$sun_tools/bin/"
export IASL_PREFIX="$sun_tools/bin/"
export PYTHON_COMMAND="$sun_root/.venv/bin/python"
export WORKSPACE="$sun_build"
export EDK_TOOLS_PATH="$sun_mu/Mu_Basecore/BaseTools"
export PACKAGES_PATH="$sun_si:$sun_mu/Mu_Basecore:$sun_root/platforms:$sun_mu/Silicon/Silicium"
export PYTHONPATH="$EDK_TOOLS_PATH/Source/Python${PYTHONPATH:+:$PYTHONPATH}"
export PATH="$EDK_TOOLS_PATH/Source/C/bin:$EDK_TOOLS_PATH/BinWrappers/PosixLike:$PATH"
mkdir -p "$sun_build/Conf" "$sun_si/build"
for sun_conf in target tools_def build_rule; do
  if ! cmp -s "$EDK_TOOLS_PATH/Conf/$sun_conf.template" "$sun_build/Conf/$sun_conf.txt"; then
    cp "$EDK_TOOLS_PATH/Conf/$sun_conf.template" "$sun_build/Conf/$sun_conf.txt"
  fi
done
# Keep compatibility with simple-init's older EDK2 package paths in a local DSC.
python3 "$sun_root/tools/prepare_simpleinit.py" "${sun_prepare_args[@]}"
python3 "$sun_root/tools/prepare_product_pump.py" verify --manifest "$sun_build/product-pump-hooks.json" >/dev/null
NOBUILD=1 bash "$sun_si/scripts/gen-rootfs-source.sh" "$sun_si" "$sun_si/build" "$sun_si/root"
pushd "$sun_si/build" >/dev/null
llvm-objcopy -I binary -O elf64-littleaarch64 -B aarch64 rootfs.bin rootfs_data.o
popd >/dev/null
cd "$sun_build"
"$PYTHON_COMMAND" "$EDK_TOOLS_PATH/Source/Python/build/build.py" \
  -a AARCH64 -b NOOPT -t CLANGDWARF -p "$sun_build/SunSimpleInit.dsc" -n 8 \
  -y "$sun_build/simpleinit-build-report.txt"
python3 "$sun_root/tools/prepare_product_pump.py" verify --manifest "$sun_build/product-pump-hooks-final.json" >/dev/null
cmp "$sun_build/product-pump-hooks.json" "$sun_build/product-pump-hooks-final.json"
cp "$sun_build/Build/SimpleInit/NOOPT_CLANGDWARF/AARCH64/SimpleInitMain.efi" \
  "$sun_output/SimpleInit.efi"
cp "$sun_build/source-manifest.json" "$sun_output/source-manifest.json"
cp "$sun_build/product-pump-hooks-final.json" "$sun_output/product-pump-hooks.json"
python3 "$sun_root/tools/simpleinit_build_identity.py" seal \
  --build-dir "$sun_build" --output-dir "$sun_output" "${sun_prepare_args[@]}" >/dev/null
