#!/usr/bin/env bash
# Host-only build. Never invokes adb or fastboot.
set -euo pipefail
sun_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sun_ws="$sun_root/upstream/Mu-Silicium"
sun_tools="${SUNUEFI_TOOLCHAIN_ROOT:-$sun_root/build/host-tools/usr}"
sun_profile="${1:-stage0}"
case "$sun_profile" in
  stage0) sun_name=piano; sun_bootshim="$sun_ws/BootShim" ;;
  probe) sun_name=pianoProbe; sun_bootshim="$sun_root/uefi/handoff/bootshim" ;;
  linux) sun_name=pianoLinux; sun_bootshim="$sun_root/uefi/handoff/bootshim" ;;
  gui) sun_name=pianoGui; sun_bootshim="$sun_root/uefi/handoff/bootshim" ;;
  product) sun_name=pianoProduct; sun_bootshim="$sun_root/uefi/handoff/bootshim" ;;
  *) printf 'Unknown build profile\n' >&2; exit 1 ;;
esac
sun_fd_name=piano-stage0.fd
sun_buildid=SunUEFI-piano-stage0
if [[ "$sun_profile" == product ]]; then
  sun_fd_name=PianoUEFI-product.fd
  sun_buildid=SunUEFI-piano-product
fi
rm -f "$sun_root/artifacts/$sun_profile/build-ok.json"
export PATH="$sun_tools/bin:$sun_root/.venv/bin:$PATH"
export LD_LIBRARY_PATH="$sun_tools/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CLANG_BIN="$sun_tools/bin/"
export IASL_PREFIX="$sun_tools/bin/"
export NASM_PREFIX="$sun_tools/bin/"
export PYTHON_COMMAND="$sun_root/.venv/bin/python"
export WORKSPACE="$sun_ws"
export EDK_TOOLS_PATH="$sun_ws/Mu_Basecore/BaseTools"
export PACKAGES_PATH="$sun_ws/Platforms/Xiaomi:$sun_ws/Common/Mu:$sun_ws/Common/Mu_OEM_Sample:$sun_ws/Mu_Basecore:$sun_ws/Silicon/Qualcomm:$sun_ws/Silicon/Silicium:$sun_ws/Silicium-ACPI:$sun_ws/Silicium-ACPI/Silicon/Qualcomm"
test -f "$sun_ws/Platforms/Xiaomi/${sun_name}Pkg/${sun_name}.dsc"
test -x "$sun_tools/bin/lld-link"
cd "$sun_ws"

# Apply the reference project's required patches to the isolated host checkout.
for sun_patch in Auth-Service.patch Boot-Manager.patch Timer.patch Usb-Bus.patch AsmMacroLib.patch; do
  sun_file="$sun_ws/Resources/MuPatches/$sun_patch"
  if git -C Mu_Basecore apply --check "$sun_file" 2>/dev/null; then
    git -C Mu_Basecore apply "$sun_file"
  elif ! git -C Mu_Basecore apply --reverse --check "$sun_file" 2>/dev/null; then
    printf 'Patch does not match pinned Basecore: %s\n' "$sun_patch" >&2
    exit 1
  fi
done

# Invalidate older outputs before starting. A failed/interrupted build must
# never leave a packageable marker for the previous diagnostic image.
"$PYTHON_COMMAND" "$sun_root/tools/build_integrity.py" start --profile "$sun_profile"

# Build BaseTools from source. Avoid changing system packages for Mono/NuGet.
make -C "$EDK_TOOLS_PATH/Source/C" -j8
mkdir -p Conf
for sun_conf in target tools_def build_rule; do
  if ! cmp -s "$EDK_TOOLS_PATH/Conf/$sun_conf.template" "Conf/$sun_conf.txt"; then
    cp "$EDK_TOOLS_PATH/Conf/$sun_conf.template" "Conf/$sun_conf.txt"
  fi
done
export PATH="$EDK_TOOLS_PATH/Source/C/bin:$EDK_TOOLS_PATH/BinWrappers/PosixLike:$PATH"
export PYTHONPATH="$EDK_TOOLS_PATH/Source/Python${PYTHONPATH:+:$PYTHONPATH}"
"$PYTHON_COMMAND" "$EDK_TOOLS_PATH/Source/Python/build/build.py" \
  -a AARCH64 -b DEBUG -t CLANGPDB -p "${sun_name}Pkg/${sun_name}.dsc" -n 8 \
  -D ENABLE_SECUREBOOT=0 -D FD_BASE=0xA7100000 -D FD_SIZE=0x300000 \
  -D FD_BLOCKS=0x300 -D DEVICE_MODEL=0 -D MEMORY_PROTECTION=TRUE -D SHIP_MODE=FALSE \
  -D BUILDID_STRING="$sun_buildid" \
  -y "$sun_root/build/logs/${sun_name}-build-report.txt"
make -C "$sun_bootshim" REQUIRES_KERNEL_HEADER=1 FD_BASE=0xA7100000 FD_SIZE=0x300000 \
  OBJCOPY="$sun_tools/bin/aarch64-linux-gnu-objcopy"
mkdir -p "$sun_root/artifacts/$sun_profile"
cp "Build/${sun_name}Pkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd" "$sun_root/artifacts/$sun_profile/$sun_fd_name"
cp "$sun_bootshim/BootShim.bin" "$sun_root/artifacts/$sun_profile/BootShim.bin"
"$PYTHON_COMMAND" "$sun_root/tools/build_integrity.py" finish --profile "$sun_profile"
printf 'Stage-0 FD built on host. Hardware boot and OS boot are unverified.\n'
