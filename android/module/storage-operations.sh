#!/system/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# WebUI supplies operation enums, GiB choices, opaque ids, or a selected file
# name encoded as UTF-8 hex plus its length. It never supplies shell paths.
set -eu
MODDIR=${0%/*}
STATE=/data/adb/piano-sunuefi/storage
NATIVE="$MODDIR/bin/piano-storage"
RESIZER="$MODDIR/bin/piano-resize-f2fs"
REQUEST="$MODDIR/bin/piano-boot-request"
RECEIPT="$STATE/android-expansion.json"
mkdir -p "$STATE"
chmod 700 "$STATE"
[ "$(id -u)" = 0 ] || { echo '需要 Root 权限。' >&2; exit 1; }
[ "$(getprop ro.product.device)" = piano ] || { echo '此工具只支持小米平板 8 Pro。' >&2; exit 1; }
[ "$(getprop ro.boot.flash.locked)" = 0 ] || { echo '需要先解锁引导加载程序。' >&2; exit 1; }
[ -x "$NATIVE" ] || { echo '存储组件未安装，请更新模块。' >&2; exit 1; }

valid_id() {
  [ "${#1}" = 32 ] || return 1
  case "$1" in *[!0-9a-f]*) return 1 ;; esac
}
new_id() { od -An -N16 -tx1 /dev/urandom | tr -d ' \n'; }
lock() {
  if ! mkdir "$STATE/lock" 2>/dev/null; then
    current_boot=$(cat /proc/sys/kernel/random/boot_id)
    previous_boot=$(cat "$STATE/lock/boot" 2>/dev/null || true)
    if [ -n "$previous_boot" ] && [ "$previous_boot" != "$current_boot" ]; then
      old_job=$(cat "$STATE/lock/job" 2>/dev/null || true)
      if valid_id "$old_job"; then
        printf '{"job_id":"%s","status":"failed","percent":0,"message":"操作期间设备已重启。请先查看分区状态，再决定是否重试。","restart_required":false}\n' "$old_job" > "$STATE/$old_job.progress.tmp"
        mv "$STATE/$old_job.progress.tmp" "$STATE/$old_job.progress.json"
      fi
      unlock
      mkdir "$STATE/lock" 2>/dev/null || { echo '另一项存储操作正在运行。' >&2; exit 1; }
    else
      echo '另一项存储操作正在运行。' >&2; exit 1
    fi
  fi
  cat /proc/sys/kernel/random/boot_id > "$STATE/lock/boot"
}
unlock() { rm -f "$STATE/lock/job" "$STATE/lock/boot"; rmdir "$STATE/lock" 2>/dev/null || true; }
progress() {
  # Message is an internal literal, never raw file names or shell output.
  printf '{"job_id":"%s","status":"%s","percent":%s,"message":"%s","restart_required":%s,"pending_android_expansion":%s}\n' \
    "$job" "$1" "$2" "$3" "${4:-false}" "${5:-false}" > "$STATE/$job.progress.tmp"
  mv "$STATE/$job.progress.tmp" "$STATE/$job.progress.json"
}
android_grow_support() {
  android_grow=false
  libfs_mgr_sha=unknown
  if [ -r /system/lib64/libfs_mgr.so ]; then
    libfs_mgr_sha=$(sha256sum /system/lib64/libfs_mgr.so | cut -d ' ' -f 1)
    # Record the library hash as evidence, not an OTA/version allowlist.
    if [ -x /system/bin/resize.f2fs ] &&
       grep -aFq /system/bin/resize.f2fs /system/lib64/libfs_mgr.so; then android_grow=true; fi
  fi
}
verify_expansion() {
  if [ ! -f "$RECEIPT" ]; then
    printf '{"pending_android_expansion":false}\n'
    return 0
  fi
  job=$(sed -n 's/.*"job_id":"\([0-9a-f]*\)".*/\1/p' "$RECEIPT")
  expected=$(sed -n 's/.*"target_userdata_bytes":\([0-9][0-9]*\).*/\1/p' "$RECEIPT")
  created_boot=$(sed -n 's/.*"created_boot_id":"\([0-9a-f-]*\)".*/\1/p' "$RECEIPT")
  valid_id "$job" || { echo 'Android 扩容记录无效。' >&2; return 1; }
  case "$expected" in ''|*[!0-9]*) echo 'Android 扩容目标容量无效。' >&2; return 1 ;; esac
  [ -n "$created_boot" ] || { echo 'Android 扩容记录缺少启动信息。' >&2; return 1; }
  if [ "$created_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ]; then
    progress pending_android_expansion 95 '分区空间已归还。请正常重启 Android，完成文件系统扩大。' true true
    cat "$STATE/$job.progress.json"
    return 0
  fi
  [ -x "$RESIZER" ] || { echo '缺少 Android 扩容检查组件。' >&2; return 1; }
  verification_rc=0
  "$RESIZER" status --expect-bytes "$expected" > "$STATE/$job.android-expansion-verification.json" \
    2> "$STATE/$job.android-expansion-verification.log" || verification_rc=$?
  if [ "$verification_rc" = 0 ] &&
     grep -q '"expansion_verified":true' "$STATE/$job.android-expansion-verification.json"; then
    progress completed 100 'SunUEFI 分区已删除，释放的空间已归还 Android。' false false
    # Keep the result, then clear only the pending receipt after raw partition,
    # plaintext mapper and both F2FS superblocks report the expected capacity.
    mv "$RECEIPT" "$STATE/android-expansion-last.json"
    sync
    if [ -f "$STATE/lock/job" ] && [ "$(cat "$STATE/lock/job")" = "$job" ] &&
       [ -f "$STATE/lock/boot" ] && [ "$(cat "$STATE/lock/boot")" != "$(cat /proc/sys/kernel/random/boot_id)" ]; then unlock; fi
  else
    progress pending_android_expansion 95 '分区空间已归还，Android 文件系统尚未完成扩大。请查看详情后再继续。' false true
  fi
  cat "$STATE/$job.progress.json"
}
check_android() {
  slot=$(getprop ro.boot.slot_suffix)
  case "$slot" in _a|_b) ;; *) echo '无法识别当前 Android 启动槽。' >&2; return 1 ;; esac
  "$REQUEST" status --device "/dev/block/by-name/boot$slot" > "$STATE/$job.boot.json"
  # NEXTv1 target zero is Android. A default Linux route must be changed in the
  # Boot tab before removing its disk system; this operation never edits BOOT.
  grep -Eq '"target"[[:space:]]*:[[:space:]]*0([,}])' "$STATE/$job.boot.json" || {
    echo '请先在启动页选择 Android，再删除 Linux 分区。' >&2; return 1;
  }
}
run_job() {
  writer=
  interrupted() {
    # A native flash already in progress owns the storage lock until its
    # write, fsync, and readback finish. Do not terminate or orphan it.
    trap '' HUP INT TERM
    if [ -n "$writer" ]; then
      wait "$writer" || true
      writer=
    fi
    progress failed 0 '操作已中断，请查看存储状态后再继续。'
    exit 1
  }
  trap 'interrupted' HUP INT TERM
  plan="$STATE/$job.plan"
  progress running 0 '正在检查分区和镜像。'
  "$NATIVE" validate "$plan" > "$STATE/$job.result.json"
  facts=$("$NATIVE" facts "$plan")
  # All eight fields are integers generated by the native parser. No eval.
  set -- $facts
  [ "$#" = 8 ] || { echo '存储计划字段不完整。' >&2; return 1; }
  op=$1; esp_number=$2; root_number=$3; userdata_number=$4
  userdata_bytes=$5; root_bytes=$6; old_root_bytes=$7
  restart=false
  case "$op" in
    1)
      progress running 5 '正在刷写 ESP 和 Linux 系统。'
      "$NATIVE" flash "$plan" --execute > "$STATE/$job.result.json" 2> "$STATE/$job.transfer.log" &
      writer=$!
      while kill -0 "$writer" 2>/dev/null; do
        line=$(tail -n 1 "$STATE/$job.transfer.log" 2>/dev/null || true)
        case "$line" in
          progress:esp:*|progress:root:*)
            IFS=: read -r prefix kind done total <<EOF
$line
EOF
            case "$done:$total" in *[!0-9:]*) ;; *)
              if [ "$total" -gt 0 ]; then
                if [ "$kind" = esp ]; then pct=$((5 + done * 10 / total)); msg='正在写入 ESP 引导文件。'
                else pct=$((15 + done * 80 / total)); msg='正在写入 Linux 系统。'; fi
                progress running "$pct" "$msg"
              fi ;; esac ;;
        esac
        sleep 1
      done
      wait "$writer"
      writer=
      ;;
    2)
      if [ "$userdata_bytes" -gt 0 ]; then
        [ -x "$RESIZER" ] || { echo '缺少 Android 数据分区调整组件。' >&2; return 1; }
        progress running 10 '正在为 Linux 腾出空间，Android 数据会保留。'
        "$RESIZER" shrink --target-bytes "$userdata_bytes" --execute > "$STATE/$job.f2fs.json"
      fi
      progress running 70 '正在建立 ESP 和 Linux 分区。'
      "$NATIVE" apply "$plan" "$STATE/$job.gpt-backup" --execute > "$STATE/$job.result.json"
      restart=true
      ;;
    3)
      fsck="$MODDIR/bin/e2fsck"; resize="$MODDIR/bin/resize2fs"
      [ -x "$fsck" ] || fsck=$(command -v e2fsck || true)
      [ -x "$resize" ] || resize=$(command -v resize2fs || true)
      [ -n "$fsck" ] && [ -n "$resize" ] || {
        echo '当前系统缺少 ext4 调整工具。' >&2; return 1;
      }
      "$NATIVE" check-project "$plan" > /dev/null
      "$NATIVE" check-root "$plan" > /dev/null
      rootdev=/dev/block/by-name/sunuefi_root
      progress running 10 '正在检查 Linux 文件系统。'
      check_rc=0; "$fsck" -pf "$rootdev" > "$STATE/$job.ext4.log" 2>&1 || check_rc=$?
      [ "$check_rc" -le 1 ] || { echo 'Linux 文件系统检查未通过，分区表保持不变。' >&2; return 1; }
      gib=$((root_bytes / 1073741824))
      if [ "$root_bytes" -lt "$old_root_bytes" ]; then
        progress running 30 '正在缩小 Linux 文件系统。'
        "$resize" "$rootdev" "${gib}G" >> "$STATE/$job.ext4.log" 2>&1
        sync
        progress running 80 '正在更新 root 分区容量。'
        "$NATIVE" apply "$plan" "$STATE/$job.gpt-backup" --execute > "$STATE/$job.result.json"
      else
        progress running 30 '正在扩大 root 分区容量。'
        "$NATIVE" apply "$plan" "$STATE/$job.gpt-backup" --execute > "$STATE/$job.result.json"
        if grep -q '"reboot_required":true' "$STATE/$job.result.json"; then
          # ext4 expands through the rootfs fstab x-systemd.growfs setting on
          # Linux startup. Never use stale kernel partition geometry here.
          restart=true
        else
          progress running 70 '正在扩大 Linux 文件系统。'
          "$resize" "$rootdev" >> "$STATE/$job.ext4.log" 2>&1
        fi
      fi
      grep -q '"reboot_required":true' "$STATE/$job.result.json" && restart=true || true
      ;;
    4)
      check_android
      "$NATIVE" check-project "$plan" > /dev/null
      progress running 70 '正在删除 SunUEFI 分区，腾出的空间保持空闲。'
      "$NATIVE" apply "$plan" "$STATE/$job.gpt-backup" --execute > "$STATE/$job.result.json"
      restart=true
      ;;
    5)
      check_android
      "$NATIVE" check-project "$plan" > /dev/null
      [ -x "$RESIZER" ] || { echo '缺少 Android 扩容检查组件。' >&2; return 1; }
      "$RESIZER" status > "$STATE/$job.f2fs-before-return.json"
      android_grow_support
      progress running 70 '正在删除 SunUEFI 分区，将空间归还 Android。'
      "$NATIVE" apply "$plan" "$STATE/$job.gpt-backup" --execute > "$STATE/$job.result.json"
      printf '{"schema_version":1,"job_id":"%s","target_userdata_bytes":%s,"created_boot_id":"%s","gpt_written":true,"pending_android_expansion":true,"android_grow_detected":%s,"libfs_mgr_sha256":"%s"}\n' \
        "$job" "$userdata_bytes" "$(cat /proc/sys/kernel/random/boot_id)" "$android_grow" "$libfs_mgr_sha" > "$RECEIPT.tmp"
      chmod 600 "$RECEIPT.tmp"
      mv "$RECEIPT.tmp" "$RECEIPT"
      sync
      progress pending_android_expansion 95 '分区空间已归还。请正常重启 Android，完成文件系统扩大。' true true
      return 0
      ;;
    *) echo '不支持此存储操作。' >&2; return 1 ;;
  esac
  case "$op" in
    1) message='ESP 和 Linux 已写入，读回检查完成。' ;;
    2) message='分区已建立。请正常重启，再刷写 ESP 和 Linux 镜像。' ;;
    3) message='root 容量已调整。需要重启时，请正常重启后继续。' ;;
    4) message='SunUEFI 分区已删除，Android 数据分区保持原大小。' ;;
  esac
  progress completed 100 "$message" "$restart"
}

case "${1:-}" in
  verify-expansion)
    verify_expansion
    ;;
  select)
    kind=${2:-}; name_hex=${3:-}; bytes=${4:-}
    case "$kind" in esp|root) ;; *) echo '请选择 ESP 或 root 镜像。' >&2; exit 2 ;; esac
    case "$name_hex" in ''|*[!0-9a-f]*) echo '所选文件名无效。' >&2; exit 2 ;; esac
    case "$bytes" in ''|*[!0-9]*) echo '所选镜像容量无效。' >&2; exit 2 ;; esac
    [ "${#name_hex}" -le 510 ] || { echo '所选文件名过长。' >&2; exit 2; }
    lock; trap 'unlock' EXIT
    "$NATIVE" select "$kind" "$name_hex" "$bytes"
    ;;
  plan)
    operation=${2:-}; value=${3:-}
    case "$operation" in
      create|resize) case "$value" in 32|64|128) ;; *) echo '请选择 32、64 或 128 GiB。' >&2; exit 2 ;; esac ;;
      flash|delete|delete-return) value=0 ;;
      *) echo '不支持此存储操作。' >&2; exit 2 ;;
    esac
    lock; trap 'unlock' EXIT
    job=$(new_id); valid_id "$job"
    section_bytes=
    if [ "$operation" = create ] && [ -x "$RESIZER" ] &&
       "$RESIZER" status > "$STATE/$job.userdata.json" 2> "$STATE/$job.userdata-status.log"; then
      section_blocks=$(sed -n 's/.*"section_blocks":\([0-9][0-9]*\).*/\1/p' "$STATE/$job.userdata.json")
      case "$section_blocks" in
        ''|*[!0-9]*) echo '无法读取 F2FS section 大小。' >&2; exit 1 ;;
      esac
      # The native F2FS parser reports 512 blocks per segment and a uint32
      # segments-per-section count, so this multiplication fits signed 64 bit.
      [ "$section_blocks" -ge 512 ] && [ "$section_blocks" -le 2199023255040 ] || {
        echo 'F2FS section 大小超出范围。' >&2; exit 1;
      }
      section_bytes=$((section_blocks * 4096))
    fi
    if [ -n "$section_bytes" ]; then
      "$NATIVE" plan "$operation" "$value" "$STATE/$job.plan" --userdata-section-bytes "$section_bytes" > "$STATE/$job.description.json"
    else
      "$NATIVE" plan "$operation" "$value" "$STATE/$job.plan" > "$STATE/$job.description.json"
    fi
    if [ "$operation" = create ] && grep -q '"requires_userdata_shrink":true' "$STATE/$job.description.json"; then
      [ -x "$RESIZER" ] || { echo '当前模块缺少 Android 数据分区调整组件。' >&2; exit 1; }
      "$RESIZER" status > "$STATE/$job.userdata.json"
    fi
    case "$operation" in delete|delete-return) check_android ;; esac
    if [ "$operation" = delete-return ]; then
      [ -x "$RESIZER" ] || { echo '当前模块缺少 Android 扩容检查组件。' >&2; exit 1; }
      "$RESIZER" status > "$STATE/$job.f2fs-before-return.json"
      android_grow_support
      printf '{"plan_id":"%s","android_grow_detected":%s,%s\n' "$job" "$android_grow" "$(sed 's/^{//' "$STATE/$job.description.json")"
    else
      printf '{"plan_id":"%s",%s\n' "$job" "$(sed 's/^{//' "$STATE/$job.description.json")"
    fi
    ;;
  execute)
    job=${2:-}; [ "${3:-}" = --confirm ] && valid_id "$job" || { echo '请先预览操作，再明确确认。' >&2; exit 2; }
    [ -f "$STATE/$job.plan" ] && [ ! -f "$STATE/$job.progress.json" ] || { echo '计划不存在或已经执行。' >&2; exit 1; }
    lock
    printf '%s\n' "$job" > "$STATE/lock/job"
    progress running 0 '操作已开始，请保持平板开机。'
    # An explicit confirmation starts one child; the WebUI polls job status.
    nohup /system/bin/sh "$0" run "$job" > "$STATE/$job.log" 2>&1 < /dev/null &
    printf '{"job_id":"%s","status":"running"}\n' "$job"
    ;;
  run)
    job=${2:-}; valid_id "$job" && [ -f "$STATE/lock/job" ] && [ "$(cat "$STATE/lock/job")" = "$job" ] || exit 2
    # A failed command must update progress. Run in a separate process with -e
    # intact; calling a shell function as an if condition would disable -e.
    if /system/bin/sh "$0" work "$job"; then :
    else
      # These logs contain only this tool's fixed device/operation messages.
      # Escape text as JSON instead of exposing a command or accepting HTML.
      reason=$(tail -n 1 "$STATE/$job.transfer.log" 2>/dev/null || true)
      case "$reason" in piano-storage:*) ;; *) reason=$(tail -n 1 "$STATE/$job.log" 2>/dev/null || true) ;; esac
      reason=$(printf '%s' "$reason" | sed 's/^piano-storage: //' | tr -d '\000-\037' | sed 's/\\/\\\\/g; s/"/\\"/g')
      [ -n "$reason" ] || reason='操作未完成，请查看当前分区状态；不会自动重试写入。'
      progress failed 0 "$reason"
    fi
    unlock
    ;;
  work)
    job=${2:-}; valid_id "$job" && [ -f "$STATE/lock/job" ] && [ "$(cat "$STATE/lock/job")" = "$job" ] || exit 2
    run_job
    ;;
  job)
    job=${2:-}; valid_id "$job" && [ -f "$STATE/$job.progress.json" ] || { echo '找不到此操作。' >&2; exit 2; }
    if [ -f "$RECEIPT" ]; then (verify_expansion >/dev/null) || true; fi
    if [ -f "$RECEIPT" ] &&
       [ "$(sed -n 's/.*"job_id":"\([0-9a-f]*\)".*/\1/p' "$RECEIPT")" = "$job" ]; then
      cat "$STATE/$job.progress.json"
      exit 0
    fi
    if [ -f "$STATE/lock/boot" ] && [ "$(cat "$STATE/lock/boot")" != "$(cat /proc/sys/kernel/random/boot_id)" ]; then
      printf '{"job_id":"%s","status":"failed","percent":0,"message":"操作期间设备已重启。请先查看分区状态，再决定是否重试。","restart_required":false}\n' "$job"
      exit 0
    fi
    cat "$STATE/$job.progress.json"
    ;;
  *) echo 'Usage: storage-operations.sh select esp|root NAME_HEX BYTES | plan OP [GiB] | execute PLAN_ID --confirm | job JOB_ID' >&2; exit 2 ;;
esac
