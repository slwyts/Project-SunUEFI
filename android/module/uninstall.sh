#!/system/bin/sh
# Uninstallation may restore only an intact wrapper belonging to this exact current ROM.
MODPATH=${0%/*}
ui_print() { echo "$*"; }
. "$MODPATH/common.sh"
[ -f "$MODPATH/module-policy.sh" ] && [ -f "$MODPATH/install-state.json" ] || {
  echo "SunUEFI: no installation state; BOOT was not changed by this uninstall."; exit 0;
}
. "$MODPATH/module-policy.sh"
piano_check_payload && piano_current_identity || exit 1
"$MODPATH/bin/piano-boot-repack" restore --boot-device "$bootdev" --active-slot "$slot" \
  --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint" \
  --installed-state "$MODPATH/install-state.json" --policy "$MODPATH/policy.json" --execute || {
  echo "SunUEFI: restore refused. No old-ROM image may be written over an OTA BOOT."; exit 1;
}
