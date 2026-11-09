#!/system/bin/sh
# Uninstallation may restore only an intact wrapper belonging to this exact current ROM.
MODPATH=${0%/*}
rm -f /mnt/vendor/persist/sunuefi/storage/grow.pending \
  /mnt/vendor/persist/sunuefi/storage/grow.attempt \
  /mnt/vendor/persist/sunuefi/storage/grow.log \
  /mnt/vendor/persist/sunuefi/storage/piano-resize-f2fs \
  /mnt/vendor/persist/sunuefi/storage/fsck.f2fs \
  /mnt/vendor/persist/sunuefi/storage/resize.f2fs \
  /metadata/sunuefi/storage-early.rc
rmdir /mnt/vendor/persist/sunuefi/storage /metadata/sunuefi 2>/dev/null || true
if [ -f "$MODPATH/installed-core.json" ]; then
  echo "已移除引导助手，BOOT 保持原样。"
  exit 0
fi
ui_print() { echo "$*"; }
. "$MODPATH/common.sh"
[ -f "$MODPATH/module-policy.sh" ] && [ -f "$MODPATH/install-state.json" ] || {
  echo "SunUEFI: no installation state; BOOT was not changed by this uninstall."; exit 0;
}
. "$MODPATH/module-policy.sh"
piano_check_payload && piano_current_identity || exit 1
"$MODPATH/bin/piano-boot-repack" restore --boot-device "$bootdev" --active-slot "$slot" \
  --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint" \
  --installed-state "$MODPATH/install-state.json" --policy "$MODPATH/policy.json" --require-ready --execute || {
  echo "SunUEFI: restore refused. No old-ROM image may be written over an OTA BOOT."; exit 1;
}
