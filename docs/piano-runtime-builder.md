# Reproducible Piano runtime bundle

`tools/build_piano_runtime_helpers.py` is the single host entry for the three
actual ARM64 helpers, AudioReach topology and the matching v4l2loopback module.
It does not invoke a guest device, install global software, alter the public
repositories or overwrite prior bundles. By default it only builds a new output.

Prerequisites are the verified workspace LLVM binaries, official Debian ARM64
GCC14/glibc static sysroot in `build/distros/piano-runtime-build`, the generated
997-header UAPI tree in `build/piano-runtime/uapi`, workspace ALSA tools, and the
fixed public/macro/v4l2 checkouts. The separate native build root was prepared
from the original official Debian base through `run_distro_userland.py`'s private
namespace backend, using authenticated APT to install gcc/libc6-dev/make; no
unsigned fetch-arm64-tools script is executed.

The public C sources are fixed at Debian builder commit
`fd6266d73f3442b23362260c3aa0c86782e0b52c` and also individually SHA pinned. The
helper UAPI is fixed at kernel commit `352508459733d3e6d349ea5581a8dd2fd8bb4180`,
with a complete header-tree digest. Public C files remain unchanged: generated
entry sources rename the original main symbol and implement `--help` before
forwarding normal operation. This is necessary because camerad and pd-locator
otherwise ignore command-line arguments and would enter their device paths.
Static AArch64 ELF headers and actual QEMU `--help` exit 0 are verified.

AudioReach macros are fixed at
`993a17dcb672357998a463a73f120064d6c74f4f`; the unchanged public topology script
runs with workspace ALSA 1.2.14 tools. Its output must match the previously
validated binary SHA and decode successfully. The earlier ALSA decode→recompile
vendor-data limitation remains documented; this tool does not claim that roundtrip.

v4l2loopback is fixed at `0f9ee86760b7f2bea174b7e3e7a1d38845da0ab4` (0.15.4).
Tracked source files are copied into each new bundle before compilation, so the
source checkout and old `.ko` artifacts are preserved. The module builds against
the exact requested kernel source/build/release, with config/Module.symvers and
UTS-release hashes bound before and after. It is stripped and its real modinfo
vermagic must match the requested release. No module is loaded.

```sh
python3 tools/build_piano_runtime_helpers.py \
  --output build/piano-runtime/BUNDLE_NEW_NAME
```

To bind a later committed SMMU integration kernel without changing the public
helper UAPI or deleting the old full candidate:

```sh
python3 tools/build_piano_runtime_helpers.py \
  --kernel-source /absolute/clean/new-kernel-worktree \
  --kernel-commit EXACT_40_HEX_COMMIT \
  --kernel-build /absolute/new-kernel-O \
  --kernel-release EXACT_KERNEL_RELEASE \
  --output build/piano-runtime/NEW_SMMU_RUNTIME
```

The output contains binaries, generated entry sources, compile/decoder/module
commands and diagnostics in `build.log`, and a manifest binding every source,
runtime/sysroot/tool input and output SHA. Inputs are rechecked before sealing.
Existing output paths are refused, including an incomplete previous build.

Optional `--stage-rootfs /absolute/workspace/build/distros/DERIVED_ROOT` installs
the five runtime files and regenerates depmod. The matching full kernel module
tree must already exist. Source hashes and every destination boundary are
validated before mutation; symlink escape, malformed install contracts, changed
bundle bytes and staging into the original artifact base are refused. The tool
does not modify the account, locale, GDM policy or firmware overrides. Rebuild
into a new output directory for every generation; this preserves old evidence.

`tests/unit/test_piano_runtime_builder.py` runs the actual canonical compile/link,
QEMU help, topology compile/decode and external module path in a separate output,
then verifies wrong kernel release and forbidden staging are refused. It does
not modify the already staged rootfs. Runtime packaging success is distinct
from physical touch, camera, audio or kernel/PID1 acceptance.
