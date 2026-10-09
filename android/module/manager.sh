#!/system/bin/sh
set -eu
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

# This checks the installed entry, not a partition name or an arbitrary EFI file.
# The current core's Linux request selects this path; other ESP entries are not
# selectable by the request protocol yet. The mount never writes to the ESP.
piano_installed_linux() (
  [ -b /dev/block/by-name/sunuefi_esp ] &&
    [ -b /dev/block/by-name/sunuefi_root ] || exit 1
  dir=$(mktemp -d "$MODPATH/.esp-read.XXXXXX") || exit 2
  mounted=false
  trap '[ "$mounted" = false ] || umount "$dir"; rmdir "$dir"' EXIT
  mount -t vfat -o ro,nosuid,nodev,noexec /dev/block/by-name/sunuefi_esp "$dir" 2>/dev/null || exit 2
  mounted=true
  [ -s "$dir/EFI/Piano/stable/boot.img" ]
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
  if piano_installed_linux; then
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
  printf '{"operation":"quick-status","route":"%s","destination":"%s","wrapped":%s,"state_available":%s,"linux_available":%s,"linux_checked":%s,"slot":"%s","request_status":"%s","request_target":%s,"request_sequence":%s,"setup_available":false,"read_only":true,"entries":%s}\n' \
    "$route" "$destination" "$wrapped" "$state_available" "$linux_available" "$linux_checked" "$slot" "$request_status" "$chosen" "$sequence" "$entries"
}

case "$operation" in
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
   exec "$repack" status --boot-device "$bootdev" --active-slot "$slot" --policy "$policy" --installed-state "$state"
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
  work="$MODPATH/.work"
  mkdir -p "$work"
  current=$($repack status --boot-device "$bootdev" --active-slot "$slot" --policy "$policy")
  if echo "$current" | grep -q '"wrapped":true'; then
   "$repack" adopt --boot-device "$bootdev" --active-slot "$slot" --policy "$policy" --state-output "$state"
  else
   "$repack" probe --boot-device "$bootdev" --active-slot "$slot" \
    --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint" \
    --policy "$policy" --output "$work/current.json" --reject-wrapped >/dev/null
   "$repack" repack --boot-device "$bootdev" --active-slot "$slot" \
    --source-metadata "$work/current.json" --payload-dir "$MODPATH/payload" \
    --policy "$policy" --state-output "$state" --execute >/dev/null
   previous=$(cat "$MODPATH/.preferred-target" 2>/dev/null || echo android)
   case "$previous" in android|uefi|linux|setup) ;; *) previous=android ;; esac
   "$repack" request --target "$previous" --boot-device "$bootdev" --active-slot "$slot" \
    --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint" \
    --policy "$policy" --installed-state "$state" --execute
  fi
  rm -f "$work/current.json" ;;
 *) exit 2 ;;
esac
