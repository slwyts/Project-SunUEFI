#!/system/bin/sh
SKIPMOUNT=true
PROPFILE=false
POSTFSDATA=false
LATESTARTSERVICE=false
if [ -f "$MODPATH/installed-core.json" ]; then
  . "$MODPATH/common.sh"
  piano_current_identity install || abort "当前 Android 未正常启动。"
  set_perm "$MODPATH/bin/piano-boot-repack" 0 0 0755
  set_perm "$MODPATH/bin/piano-boot-request" 0 0 0755
  "$MODPATH/bin/piano-boot-repack" adopt --boot-device "$bootdev" --active-slot "$slot" \
    --policy "$MODPATH/policy.json" --state-output "$MODPATH/install-state.json" ||
    abort "没有找到与模块匹配的 SunUEFI 引导。"
  set_perm_recursive "$MODPATH" 0 0 0755 0644
  set_perm "$MODPATH/bin/piano-boot-repack" 0 0 0755
  set_perm "$MODPATH/bin/piano-boot-request" 0 0 0755
  set_perm "$MODPATH/action.sh" 0 0 0755
  set_perm "$MODPATH/manager.sh" 0 0 0755
  ui_print "引导助手已安装。打开模块页面即可选择启动目标。"
  return 0
fi
ui_print "SunUEFI: current-ROM BOOT installation"
[ "$BOOTMODE" = true ] || abort "Install from the module manager in a normally booted Android ROM."
[ "${ARCH:-}" = arm64 ] || abort "This module requires ARM64 Android."
. "$MODPATH/common.sh"
[ -f "$MODPATH/module-policy.sh" ] || abort "Source scaffold: no validated module policy."
. "$MODPATH/module-policy.sh"
[ -f "$MODPATH/bin/piano-boot-repack" ] || abort "The validated repacker is missing."
set_perm "$MODPATH/bin/piano-boot-repack" 0 0 0755
piano_check_payload || abort "Module is not ready; no partition was written."
piano_current_identity install || abort "Android identity checks failed; no partition was written."
state=$(piano_partition_state /dev/block/by-name) || abort "Only one SunUEFI partition exists; review the layout manually."
if [ "$state" = existing ]; then
  ui_print "sunuefi_esp and sunuefi_root exist; skipping all resize/partition operations."
else
  ui_print "No existing Linux ESP/root pair. Android and the UEFI menu remain available."
fi
ui_print "Partitions and userdata are left unchanged. Linux requires an existing installation."
work="$MODPATH/.work"
mkdir -p "$work" || abort "Cannot create temporary metadata directory."
repack="$MODPATH/bin/piano-boot-repack"
# probe stores bounded reconstruction metadata, never a full stock partition copy.
"$repack" probe --boot-device "$bootdev" --active-slot "$slot" \
  --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint" \
  --policy "$MODPATH/policy.json" --output "$work/current.json" --reject-wrapped ||
  abort "Current BOOT is unsupported or already wrapped. No write occurred."
ui_print "Install on the currently running slot $slot? Volume Up confirms; Volume Down cancels."
key=$(piano_read_key) || abort "No confirmation; no write occurred."
[ "$key" = up ] || abort "Cancelled; no write occurred."
# The native tool must re-read slot/fingerprints/source hash immediately before its guarded write.
"$repack" repack --boot-device "$bootdev" --active-slot "$slot" \
  --source-metadata "$work/current.json" --payload-dir "$MODPATH/payload" \
  --policy "$MODPATH/policy.json" --state-output "$MODPATH/install-state.json" --require-ready --execute ||
  abort "Repack/write failed. Read the native tool's result before rebooting."
rm -rf "$work"
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/piano-boot-repack" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
ui_print "Active BOOT was read back by the native tool. Reboot to test the early selector."
ui_print "Normal reboot and reboot recovery retain Android and stock Recovery. UEFI needs a separate verified request."
ui_print "After an OTA, boot the new Android ROM normally and reinstall; this module does not patch inactive slots."
