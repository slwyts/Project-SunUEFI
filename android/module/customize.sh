#!/system/bin/sh
SKIPMOUNT=true
PROPFILE=false
POSTFSDATA=false
LATESTARTSERVICE=true
if [ -f "$MODPATH/installed-core.json" ]; then
  [ "$BOOTMODE" = true ] || abort "请在正常启动的 Android 模块管理器中安装。"
  [ "${ARCH:-}" = arm64 ] || abort "此模块需要 ARM64 Android。"
  . "$MODPATH/common.sh"
  piano_current_identity install || abort "当前 Android 未正常启动。"
  set_perm "$MODPATH/bin/piano-boot-repack" 0 0 0755
  set_perm "$MODPATH/bin/piano-boot-request" 0 0 0755
  repack="$MODPATH/bin/piano-boot-repack"
  work="$MODPATH/.work"
  mkdir -p "$work"
  ui_print "正在识别当前 Android 引导。"
  if grep -Eq '"generic_core"[[:space:]]*:[[:space:]]*true' "$MODPATH/installed-core.json"; then
    "$repack" status --boot-device "$bootdev" --active-slot "$slot" --policy "$MODPATH/policy.json" > "$work/status.json" ||
      abort "无法识别当前 BOOT，尚未写入。"
    if grep -q '"wrapped":true' "$work/status.json"; then
      if "$repack" adopt --boot-device "$bootdev" --active-slot "$slot" --policy "$MODPATH/policy.json"         --state-output "$MODPATH/install-state.json" > /dev/null 2> "$work/adopt.log"; then
        ui_print "当前引导已是相同核心，保留现有启动选择。"
      else
        ui_print "正在更新 SunUEFI，保留当前 Android 内核和启动选择。"
        # Manual BOOT installations may not have an old module record. The
        # upgrade command validates and reconstructs the actual current carrier
        # and rechecks its full source and device identity before writing.
        "$repack" upgrade --boot-device "$bootdev" --active-slot "$slot" --policy "$MODPATH/policy.json"           --payload-dir "$MODPATH/payload" --state-output "$MODPATH/install-state.json" --execute > /dev/null ||
          abort "引导更新未完成，请查看上面的原因。"
      fi
    else
      ui_print "正在安装开机选择器，保留当前 Android 内核，默认进入 Android。"
      "$repack" probe --boot-device "$bootdev" --active-slot "$slot"         --rom-fingerprint "$rom_fingerprint" --boot-fingerprint "$boot_fingerprint"         --policy "$MODPATH/policy.json" --output "$work/current.json" --reject-wrapped > /dev/null ||
        abort "当前 BOOT 格式不受支持，尚未写入。"
      "$repack" repack --boot-device "$bootdev" --active-slot "$slot"         --source-metadata "$work/current.json" --payload-dir "$MODPATH/payload"         --policy "$MODPATH/policy.json" --state-output "$MODPATH/install-state.json" --execute > /dev/null ||
        abort "引导安装未完成，请查看上面的原因。"
    fi
  else
    "$repack" adopt --boot-device "$bootdev" --active-slot "$slot"       --policy "$MODPATH/policy.json" --state-output "$MODPATH/install-state.json" > /dev/null ||
      abort "没有找到与模块匹配的 SunUEFI 引导。"
  fi
  # Preserve the actual boot preference across module upgrades and later OTA
  # stock-BOOT replacement; the installer never infers it from a stale UI.
  choice=$("$MODPATH/bin/piano-boot-request" status --device "$bootdev") || abort "无法读取已安装引导的启动选择。"
  selected=$(printf '%s' "$choice" | sed -n 's/.*"target":\([0-3]\).*/\1/p')
  case "$selected" in 0) preferred=android ;; 1) preferred=uefi ;; 2) preferred=linux ;; 3) preferred=setup ;; *) abort "启动选择格式无效。" ;; esac
  printf '%s\n' "$preferred" > "$MODPATH/.preferred-target"
  rm -f "$work/current.json" "$work/status.json" "$work/adopt.log"
  set_perm_recursive "$MODPATH" 0 0 0755 0644
  set_perm "$MODPATH/bin/piano-boot-repack" 0 0 0755
  set_perm "$MODPATH/bin/piano-boot-request" 0 0 0755
  for binary in "$MODPATH"/bin/*; do set_perm "$binary" 0 0 0755; done
  set_perm "$MODPATH/service.sh" 0 0 0755
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
