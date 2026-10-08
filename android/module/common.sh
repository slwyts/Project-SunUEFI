#!/system/bin/sh
# These functions also run under host sh with injected key events in tests.
piano_fail() { ui_print "! $*"; return 1; }
piano_read_key() {
  command -v getevent >/dev/null && command -v timeout >/dev/null || return 1
  event=$(timeout 15 getevent -qlc 1 2>/dev/null) || return 1
  case "$event" in
    *KEY_VOLUMEUP*DOWN*|*" 0001 0073 00000001"*) echo up ;;
    *KEY_VOLUMEDOWN*DOWN*|*" 0001 0072 00000001"*) echo down ;;
    *) echo ignored ;;
  esac
}
piano_partition_state() {
  esp=0; root=0
  [ -e "$1/sunuefi_esp" ] && esp=1
  [ -e "$1/sunuefi_root" ] && root=1
  case "$esp:$root" in
    1:1) echo existing ;; 0:0) echo absent ;; *) echo incomplete; return 1 ;;
  esac
}
piano_linux_available() {
  [ "$(piano_partition_state /dev/block/by-name)" = existing ] || {
    piano_fail "Linux requires an already installed sunuefi_esp and sunuefi_root pair."; return 1;
  }
}
piano_current_identity() {
  [ "$(getprop ro.product.device)" = piano ] || piano_fail "Only piano is supported." || return
  [ "$(getprop ro.boot.flash.locked)" = 0 ] || piano_fail "An unlocked bootloader is required." || return
  slot=$(getprop ro.boot.slot_suffix)
  case "$slot" in _a|_b) ;; *) piano_fail "Cannot identify the current Android slot."; return 1 ;; esac
  bootdev="/dev/block/by-name/boot$slot"
  [ -b "$bootdev" ] || { piano_fail "The active BOOT partition is unavailable."; return 1; }
  rom_fingerprint=$(getprop ro.build.fingerprint)
  boot_fingerprint=$(getprop ro.bootimage.build.fingerprint)
  [ -n "$rom_fingerprint" ] && [ -n "$boot_fingerprint" ] || {
    piano_fail "ROM and boot-image fingerprints are required."; return 1;
  }
  if [ "${1:-}" = install ] && [ "$(getprop sys.boot_completed)" != 1 ]; then
    piano_fail "Install only after the current Android ROM has booted normally."; return 1
  fi
}
piano_check_payload() {
  [ "$PIANO_POLICY_SCHEMA" = 1 ] && [ "$PIANO_INTERFACE_VERSION" = 1 ] &&
  [ "$PIANO_DEVICE_PASSTHROUGH_VERIFIED" = true ] &&
  [ "$PIANO_REQUEST_HANDLING_VERIFIED" = true ] &&
  [ "$PIANO_STANDARD_RECOVERY_PRESERVED" = true ] || {
    piano_fail "Android passthrough, persistent request handling or stock Recovery remain unverified."; return 1;
  }
  (cd "$MODPATH" && sha256sum -c payload.sha256 >/dev/null 2>&1) || {
    piano_fail "Module payload hashes differ."; return 1;
  }
  [ -x "$MODPATH/bin/piano-boot-repack" ] || {
    piano_fail "The validated ARM64 repacker is missing."; return 1;
  }
  "$MODPATH/bin/piano-boot-repack" status --interface-version 1 --policy "$MODPATH/policy.json" --require-ready || {
    piano_fail "The native repacker refused readiness."; return 1;
  }
}
