#!/system/bin/sh
# Verify early maintenance after Android has mounted /data. Never resize here.
MODPATH=${0%/*}
[ -f /data/adb/piano-sunuefi/storage/android-expansion.json ] || exit 0
/system/bin/sh "$MODPATH/storage-operations.sh" verify-expansion >/dev/null 2>&1
