#!/system/bin/sh
# One read-only check after Android has mounted /data. No persistent daemon.
MODPATH=${0%/*}
[ -f /data/adb/piano-sunuefi/storage/android-expansion.json ] || exit 0
/system/bin/sh "$MODPATH/storage-operations.sh" verify-expansion >/dev/null 2>&1
