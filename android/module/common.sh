#!/system/bin/sh
# These functions also run under host sh with injected key events in tests.
piano_fail() { ui_print "! $*"; return 1; }
piano_read_key() {
  command -v getevent >/dev/null && command -v timeout >/dev/null || return 1
  deadline=$(($(date +%s) + 30))
  while :; do
    remaining=$((deadline - $(date +%s)))
    [ "$remaining" -gt 0 ] || return 1
    event=$(timeout "$remaining" getevent -qlc 1 2>/dev/null) || return 1
    case "$event" in
      *KEY_VOLUMEUP*DOWN*|*" 0001 0073 00000001"*) echo up; return ;;
      *KEY_VOLUMEDOWN*DOWN*|*" 0001 0072 00000001"*) echo down; return ;;
    esac
  done
}
piano_choose_boot_install() {
  ui_print "音量＋：安装或更新 SunUEFI 引导。"
  ui_print "音量－：只安装引导助手，不修改 BOOT。"
  ui_print "30 秒未选择时，只安装引导助手。"
  key=$(piano_read_key) || key=down
  if [ "$key" = up ]; then
    ui_print "已选择安装或更新引导。"
    return 0
  fi
  ui_print "已选择只安装引导助手，可稍后在维护页安装引导。"
  return 1
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
piano_android_grow_ready() (
  tool=${MODDIR:-${MODPATH:-}}/bin/piano-resize-f2fs
  [ -x "$tool" ] && "$tool" early-status >/dev/null 2>&1 || return 1
  for rc in /metadata/vold/ksu/modules.rc /metadata/watchdog/ksu/modules.rc /metadata/ksu/modules.rc; do
    if [ -s "$rc" ]; then
      grep -qx 'import /metadata/sunuefi/storage-early.rc' "$rc"
      return
    fi
  done
  return 1
)
piano_esp_arm64_application() (
  # Identify PE32+ ARM64 EFI applications; firmware validates the full image.
  kind=${3:-loader}
  file=$(readlink -f "$2") || exit 2
  case "$file" in "$1"/*) ;; *) exit 1 ;; esac
  size=$(stat -c %s "$file") || exit 2
  [ "$size" -ge 64 ] || exit 1
  bytes=$(od -An -v -tx1 -N64 "$file") || exit 2
  set -- $bytes
  [ "$#" -eq 64 ] && [ "$1$2" = 4d5a ] || exit 1
  shift 60
  pe=$((0x$1 | (0x$2 << 8) | (0x$3 << 16) | (0x$4 << 24)))
  [ "$pe" -ge 64 ] && [ "$((pe + 94))" -le "$size" ] || exit 1
  bytes=$(od -An -v -tx1 -j "$pe" -N94 "$file") || exit 2
  set -- $bytes
  [ "$#" -eq 94 ] && [ "$1$2$3$4$5$6" = 5045000064aa ] || exit 1
  sections=$((0x$7 | (0x$8 << 8)))
  shift 20
  optional=$((0x$1 | (0x$2 << 8)))
  shift 4
  [ "$1$2" = 0b02 ] || exit 1
  shift 68
  [ "$1$2" = 0a00 ] || exit 1
  [ "$kind" = uki ] || exit 0

  # A UKI contains a Linux payload section. An EFI tool can share its suffix.
  section=$((pe + 24 + optional))
  [ "$optional" -ge 70 ] && [ "$sections" -le 96 ] &&
    [ "$((section + 40 * sections))" -le "$size" ] || exit 1
  while [ "$sections" -gt 0 ]; do
    bytes=$(od -An -v -tx1 -j "$section" -N24 "$file") || exit 2
    set -- $bytes
    [ "$#" -eq 24 ] || exit 1
    if [ "$1$2$3$4$5$6$7$8" = 2e6c696e75780000 ]; then
      shift 16
      length=$((0x$1 | (0x$2 << 8) | (0x$3 << 16) | (0x$4 << 24)))
      shift 4
      offset=$((0x$1 | (0x$2 << 8) | (0x$3 << 16) | (0x$4 << 24)))
      [ "$length" -gt 0 ] && [ "$offset" -ge "$((section + 40 * sections))" ] &&
        [ "$offset" -le "$size" ] && [ "$length" -le "$((size - offset))" ]
      exit "$?"
    fi
    section=$((section + 40))
    sections=$((sections - 1))
  done
  exit 1
)

# Check the ARM64 fallback, conventional Linux loaders and versioned UKIs.
# Boot order and loader configuration belong to firmware and the boot manager.
# Return 1 for no candidate and 2 if inspection could not be completed.
piano_esp_linux_present() (
  root=$(readlink -f "$1") || exit 2
  [ -d "$root" ] && [ -r "$root" ] && [ -x "$root" ] || exit 2
  efi=$root/EFI
  [ -d "$efi" ] || exit 1
  [ -r "$efi" ] && [ -x "$efi" ] || exit 2
  result=1
  for directory in "$efi"/*; do
    [ -d "$directory" ] || continue
    if [ ! -r "$directory" ] || [ ! -x "$directory" ]; then
      result=2
      continue
    fi
    for entry in "$directory"/*.[Ee][Ff][Ii]; do
      [ -f "$entry" ] || continue
      kind=loader
      case "${directory##*/}/${entry##*/}" in
        [Ll][Ii][Nn][Uu][Xx]/*) kind=uki ;;
        [Bb][Oo][Oo][Tt]/[Bb][Oo][Oo][Tt][Aa][Aa]64.[Ee][Ff][Ii] | \
        [Ss][Yy][Ss][Tt][Ee][Mm][Dd]/[Ss][Yy][Ss][Tt][Ee][Mm][Dd]-[Bb][Oo][Oo][Tt][Aa][Aa]64.[Ee][Ff][Ii] | \
        */[Gg][Rr][Uu][Bb][Aa][Aa]64.[Ee][Ff][Ii] | */[Ss][Hh][Ii][Mm][Aa][Aa]64.[Ee][Ff][Ii]) ;;
        *) continue ;;
      esac
      if piano_esp_arm64_application "$root" "$entry" "$kind"; then
        exit 0
      elif [ "$?" -eq 2 ]; then
        result=2
      fi
    done
  done
  exit "$result"
)
piano_check_bootloader() {
  [ "$(getprop ro.boot.flash.locked)" = 0 ] && return 0
  [ "${PIANO_CONFIRMED_UNVERIFIED_BOOTLOADER:-}" = 1 ] && return 0
  # Root managers do not all display a child process's stderr. Publish the
  # confirmation through their installer UI before the native key reader waits.
  ui_print "无法确认 BL 已解锁，可能启用了状态伪装模块。"
  ui_print "若实际未解锁、只有临时 Root，写入引导可能导致设备无法启动。"
  ui_print "确认实际已解锁后，松开音量键，在 30 秒内按下并松开三次音量下。"
  ui_print "按音量上取消；超时不写入 BOOT。"
  "$MODPATH/bin/piano-boot-repack" confirm-bootloader || return 1
  # Private to this operation and its child tools; never saved across boots or
  # later WebUI requests, and never used to bypass any other identity check.
  PIANO_CONFIRMED_UNVERIFIED_BOOTLOADER=1
  export PIANO_CONFIRMED_UNVERIFIED_BOOTLOADER
}
piano_boot_source_warning() {
  if grep -Eq '"boot_source_confirm_required"[[:space:]]*:[[:space:]]*true' "$1"; then
    ui_print "当前 BOOT 的 AVB 摘要缺失或不一致，可能装有自定义 GKI 或被 Root 工具修改。"
    ui_print "安装会保留当前内核，不换回原厂；完整还原检查通过后会请求实体音量键确认。"
    ui_print "确认这是目前正常启动 Android 所用的 BOOT 后，按下并松开三次音量下继续。"
    ui_print "按音量上取消；30 秒未完成则退出，尚未写入 BOOT。"
  fi
}
piano_current_identity() {
  [ "$(getprop ro.product.device)" = piano ] || piano_fail "Only piano is supported." || return
  slot=$(getprop ro.boot.slot_suffix)
  case "$slot" in _a|_b) ;; *) piano_fail "Cannot identify the current Android slot."; return 1 ;; esac
  bootdev="/dev/block/by-name/boot$slot"
  [ -b "$bootdev" ] || { piano_fail "The active BOOT partition is unavailable."; return 1; }
  rom_fingerprint=$(getprop ro.build.fingerprint)
  boot_fingerprint=$(getprop ro.bootimage.build.fingerprint)
  if [ "${1:-}" = install ] && [ "$(getprop sys.boot_completed)" != 1 ]; then
    piano_fail "Install only after the current Android ROM has booted normally."; return 1
  fi
  return 0
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
