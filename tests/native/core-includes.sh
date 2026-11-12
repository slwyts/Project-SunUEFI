#!/usr/bin/env bash
# Core sources are grouped by subsystem but include each other by bare name.
# The generated firmware module is flat, so host builds get the same flat view.
sun_core_include_path="uefi/core"
for sun_core_dir in uefi/core/*/; do
  sun_core_include_path="$sun_core_include_path:${sun_core_dir%/}"
done
export CPATH="$sun_core_include_path${CPATH:+:$CPATH}"
unset sun_core_dir sun_core_include_path
