#!/system/bin/sh
# No argument is a read-only status action for Magisk's module Action button.
MODPATH=${0%/*}
ui_print() { echo "$*" >&2; }
operation=${1:-status}
target=${2:-}
case "$operation" in
  status) [ -z "$target" ] || exit 2 ;;
  preview|request) case "$target" in android|uefi|linux|setup) ;; *) exit 2 ;; esac ;;
  *) exit 2 ;;
esac
if [ ! -f "$MODPATH/module-policy.sh" ] || [ ! -x "$MODPATH/bin/piano-boot-repack" ]; then
  cat "$MODPATH/webroot/status.json"
  [ "$operation" = status ] && exit 0
  exit 2
fi
. "$MODPATH/common.sh"
. "$MODPATH/module-policy.sh"
piano_check_payload && piano_current_identity || exit 2
tool="$MODPATH/bin/piano-boot-repack"
if [ "$operation" = status ]; then
  "$tool" status --policy "$MODPATH/policy.json" --boot-device "$bootdev" \
    --active-slot "$slot" --installed-state "$MODPATH/install-state.json" --json --read-only
else
  [ "$target" != linux ] || piano_linux_available || exit 2
  if [ "$operation" = request ]; then
    # A future bridge must collect explicit user confirmation, then pass this bounded token.
    [ "${3:-}" = --confirm ] && [ "$#" = 3 ] || exit 2
    mode=--execute
  else
    [ "$#" = 2 ] || exit 2
    mode=--preview
  fi
  "$tool" request --target "$target" --boot-device "$bootdev" --active-slot "$slot" \
    --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint" \
    --installed-state "$MODPATH/install-state.json" --policy "$MODPATH/policy.json" --require-ready "$mode"
fi
# The route persists until another confirmed request selects Android/UEFI/Linux/setup.
# This script does not reboot or write misc/PMIC.
