#!/system/bin/sh
set -eu
unset PIANO_CONFIRMED_UNVERIFIED_BOOTLOADER
MODPATH=${0%/*}
ui_print() { echo "$*" >&2; }
. "$MODPATH/common.sh"
piano_current_identity
repack="$MODPATH/bin/piano-boot-repack"
request="$MODPATH/bin/piano-boot-request"
policy="$MODPATH/policy.json"
state="$MODPATH/install-state.json"
operation=${1:-status}
target=${2:-}
case "$operation" in
 switch|reinstall)
  [ ! -d /data/adb/piano-sunuefi/storage/lock ] || {
   echo '存储操作正在进行，请完成后再切换系统或重新安装引导。' >&2; exit 1;
  } ;;
esac

# Check the installed boot manager and Linux entry on a read-only mount.
piano_installed_linux() (
  [ -b /dev/block/by-name/sunuefi_esp ] &&
    [ -b /dev/block/by-name/sunuefi_root ] || exit 1
  dir=$(mktemp -d "$MODPATH/.esp-read.XXXXXX") || exit 2
  mounted=false
  trap '[ "$mounted" = false ] || umount "$dir"; rmdir "$dir"' EXIT
  mount -t vfat -o ro,nosuid,nodev,noexec /dev/block/by-name/sunuefi_esp "$dir" 2>/dev/null || exit 2
  mounted=true
  piano_esp_linux_present "$dir"
)

piano_quick_status() {
  wrapped=false; route=unknown; destination=unknown; chosen=null; sequence=null
  request_status=unavailable
  if info=$("$request" status --device "$bootdev" 2>/dev/null); then
    chosen=$(printf '%s\n' "$info" | sed -n 's/.*"target":\([0-3]\).*/\1/p')
    sequence=$(printf '%s\n' "$info" | sed -n 's/.*"sequence":\([0-9]*\).*/\1/p')
    case "$chosen" in
      0) route=android; destination=menu ;;
      1) route=uefi; destination=menu ;;
      2) route=uefi; destination=linux ;;
      3) route=uefi; destination=setup ;;
      *) echo 'Cannot read the current boot selection.' >&2; return 2 ;;
    esac
    wrapped=true; request_status=ok
  fi
  state_available=false
  [ ! -f "$state" ] || state_available=true
  linux_available=false; linux_checked=true
  if [ -d /data/adb/piano-sunuefi/storage/lock ]; then
    linux_checked=false
  elif piano_installed_linux; then
    linux_available=true
  else
    [ "$?" = 1 ] || linux_checked=false
  fi
  entries='[]'
  if [ "$wrapped" = true ]; then
    entries='[{"id":"uefi","destination":"menu","kind":"tool","available":true}'
    if [ "$linux_available" = true ]; then
      entries="$entries"',{"id":"linux","destination":"linux","kind":"system","available":true}'
    fi
    entries="$entries"']'
  fi
  confirm_bootloader=false
  [ "$wrapped" = true ] || [ "$(getprop ro.boot.flash.locked)" = 0 ] || confirm_bootloader=true
  printf '{"operation":"quick-status","route":"%s","destination":"%s","wrapped":%s,"state_available":%s,"linux_available":%s,"linux_checked":%s,"slot":"%s","request_status":"%s","request_target":%s,"request_sequence":%s,"setup_available":false,"read_only":true,"bootloader_confirm_required":%s,"entries":%s}\n' \
    "$route" "$destination" "$wrapped" "$state_available" "$linux_available" "$linux_checked" "$slot" "$request_status" "$chosen" "$sequence" "$confirm_bootloader" "$entries"
}

case "$operation" in
 storage-reboot)
  [ -f /data/adb/piano-sunuefi/storage/android-expansion.json ] || exit 2
  current=$("$request" status --device "$bootdev")
  printf '%s' "$current" | grep -Eq '"target"[[:space:]]*:[[:space:]]*0([,}])' || {
   echo '请先恢复 Android 启动选择。' >&2; exit 2;
  }
  piano_android_grow_ready || {
   echo '早期存储维护尚未安装，请更新引导助手模块后继续。' >&2; exit 2;
  }
  /system/bin/sh "$MODPATH/storage-operations.sh" finish-expansion --execute >/dev/null
  (sleep 1; /system/bin/reboot) >/dev/null 2>&1 </dev/null &
  echo '{"status":"rebooting"}' ;;
 storage-status)
  exec /system/bin/sh "$MODPATH/storage.sh" status ;;
 storage-select)
  exec /system/bin/sh "$MODPATH/storage-operations.sh" select "$target" "${3:-}" "${4:-}" ;;
 storage-plan)
  case "$target" in flash|create|resize|delete|delete-return) ;; *) exit 2 ;; esac
  exec /system/bin/sh "$MODPATH/storage-operations.sh" plan "$target" "${3:-}" ;;
 storage-execute)
  exec /system/bin/sh "$MODPATH/storage-operations.sh" execute "$target" --confirm ;;
 storage-job)
  exec /system/bin/sh "$MODPATH/storage-operations.sh" job "$target" ;;
 quick-status)
  piano_quick_status ;;
 summary)
  info=$(piano_quick_status)
  language=${PIANO_LANG:-$(getprop persist.sys.locale)}
  case "$language" in
   en*)
    echo 'SunUEFI Boot Helper'
    if printf '%s\n' "$info" | grep -q '"request_status":"ok"'; then
      case "$info" in
       *'"route":"android"'*) echo 'Boot selection: Android' ;;
       *'"destination":"linux"'*) echo 'Boot selection: UEFI → Linux' ;;
       *'"destination":"setup"'*) echo 'Boot selection: UEFI → Firmware settings' ;;
       *) echo 'Boot selection: UEFI menu' ;;
      esac
    else
      echo 'Boot selection unavailable. Use Check boot in the WebUI.'
    fi
    echo 'Open WebUI on the module card to select a system or reinstall after an update.'
    echo 'This action only reads the current selection and does not restart the tablet.' ;;
   *)
    echo 'SunUEFI 引导助手'
    if printf '%s\n' "$info" | grep -q '"request_status":"ok"'; then
      case "$info" in
       *'"route":"android"'*) echo '启动选择：Android' ;;
       *'"destination":"linux"'*) echo '启动选择：UEFI → Linux' ;;
       *'"destination":"setup"'*) echo '启动选择：UEFI → 固件设置' ;;
       *) echo '启动选择：UEFI 菜单' ;;
      esac
    else
      echo '暂时无法读取启动选择，请在 WebUI 中点击“检查引导”。'
    fi
    echo '请点击模块卡片上的“打开 WebUI”，选择系统或在系统更新后重新安装引导。'
    echo '本次仅查看当前选择，不会重启平板。' ;;
  esac ;;
 status)
  if [ -f "$state" ]; then
   # A complete BOOT check is deliberately requested by Check boot. Do it once.
   if "$repack" status --boot-device "$bootdev" --active-slot "$slot" \
      --policy "$policy" --installed-state "$state"; then
    exit 0
   fi
   # OTA can replace a wrapped BOOT while leaving the old slot metadata. Show
   # the real current slot so the maintenance page can offer reinstall.
   echo '旧槽位的安装记录已失效，当前 BOOT 将按未安装处理。' >&2
  fi
  # Missing installation metadata is reported as-is. Reinstall/adopt owns state
  # creation; checking boot must not silently change installation metadata.
  exec "$repack" status --boot-device "$bootdev" --active-slot "$slot" --policy "$policy" ;;
 switch)
  case "$target" in android|uefi|linux) ;; setup) echo "固件设置的一次性入口尚未启用。" >&2; exit 2 ;; *) exit 2 ;; esac
  [ "$target" != linux ] || piano_installed_linux || {
   echo '未找到已安装的 Linux 启动文件，请先完成 Linux 安装。' >&2; exit 2;
  }
  "$repack" request --target "$target" --boot-device "$bootdev" --active-slot "$slot" \
   --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint" \
   --policy "$policy" --installed-state "$state" --execute
  printf '%s\n' "$target" > "$MODPATH/.preferred-target.new"
  mv "$MODPATH/.preferred-target.new" "$MODPATH/.preferred-target"
  (sleep 1; /system/bin/reboot) >/dev/null 2>&1 </dev/null & ;;
 reinstall)
  mkdir -p "$MODPATH/.work"
  # Every attempt needs fresh exclusive-create outputs. OTA leaves the old
  # slot's state in place, and an interrupted attempt may leave its probe.
  work=$(mktemp -d "$MODPATH/.work/reinstall.XXXXXX")
  next_state="$work/install-state.json"
  current=$($repack status --boot-device "$bootdev" --active-slot "$slot" --policy "$policy")
  if echo "$current" | grep -q '"wrapped":true'; then
   if ! "$repack" adopt --boot-device "$bootdev" --active-slot "$slot" --policy "$policy" \
     --state-output "$next_state" 2>"$work/adopt.log"; then
    # Assistant-only installation may leave an older core in BOOT. Upgrade
    # reconstructs and validates that carrier before replacing its UEFI core.
    "$repack" upgrade --boot-device "$bootdev" --active-slot "$slot" \
     --policy "$policy" --payload-dir "$MODPATH/payload" \
     --state-output "$next_state" --execute
   fi
   mv -f "$next_state" "$state"
  else
   "$repack" probe --boot-device "$bootdev" --active-slot "$slot" \
    --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint" \
    --policy "$policy" --output "$work/current.json" --reject-wrapped >/dev/null
   piano_check_bootloader || exit 1
   "$repack" repack --boot-device "$bootdev" --active-slot "$slot" \
    --source-metadata "$work/current.json" --payload-dir "$MODPATH/payload" \
    --policy "$policy" --state-output "$next_state" --execute >/dev/null
   # Replace the prior slot's metadata only after native write/readback
   # verification. Failed attempts retain their prepared record for diagnosis.
   mv -f "$next_state" "$state"
   previous=$(cat "$MODPATH/.preferred-target" 2>/dev/null || echo android)
   case "$previous" in android|uefi|linux|setup) ;; *) previous=android ;; esac
   "$repack" request --target "$previous" --boot-device "$bootdev" --active-slot "$slot" \
    --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint" \
    --policy "$policy" --installed-state "$state" --execute
  fi
  rm -f "$work/current.json" "$work/adopt.log"
  rmdir "$work"
   ;;
 *) exit 2 ;;
esac
