#!/system/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# A bounded, read-only inventory for the module UI. No mount or GPT write.
set -eu

operation=${1:-status}
[ "$operation" = status ] || { echo 'Only storage status is available.' >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo 'Root permission is required.' >&2; exit 1; }
MODDIR=${0%/*}
backend=false; create=false; resize=false; flash=false; delete=false; release=false
[ ! -x "$MODDIR/bin/piano-storage" ] || backend=true
if [ "$backend" = true ] && [ -x "$MODDIR/bin/piano-resize-f2fs" ]; then create=true; fi
if [ "$backend" = true ] &&
   { [ -x "$MODDIR/bin/e2fsck" ] || command -v e2fsck >/dev/null; } &&
   { [ -x "$MODDIR/bin/resize2fs" ] || command -v resize2fs >/dev/null; }; then resize=true; fi

block_signature() {
  if command -v blkid >/dev/null; then blkid "$1"
  elif [ -x /data/adb/ksu/bin/busybox ]; then /data/adb/ksu/bin/busybox blkid "$1"
  elif [ -x /data/adb/magisk/busybox ]; then /data/adb/magisk/busybox blkid "$1"
  else return 1; fi
}

partition_json() {
  name=$1
  path="/dev/block/by-name/$name"
  if [ ! -b "$path" ]; then
    printf '{"name":"%s","present":false}' "$name"
    return
  fi
  resolved=$(readlink -f "$path")
  case "$resolved" in /dev/block/sd[a-z][0-9]*) ;; *)
    echo 'Expected a physical UFS partition.' >&2; return 1 ;;
  esac
  bytes=$(blockdev --getsize64 "$path")
  case "$bytes" in ''|*[!0-9]*) echo 'Cannot read partition capacity.' >&2; return 1 ;; esac
  # blkid reads the filesystem signatures; TYPE is not inferred from the name.
  signature=$(block_signature "$path" 2>/dev/null || true)
  filesystem=$(printf '%s\n' "$signature" | sed -n 's/.* TYPE="\([A-Za-z0-9_-]*\)".*/\1/p')
  [ -n "$filesystem" ] || filesystem=unknown
  mounted=false
  while read -r source mountpoint remaining; do
    case "$source" in "$path"|"$resolved") mounted=true ;; esac
  done < /proc/mounts
  printf '{"name":"%s","present":true,"device":"%s","bytes":%s,"filesystem":"%s","mounted":%s}' \
    "$name" "$resolved" "$bytes" "$filesystem" "$mounted"
}

esp=$(partition_json sunuefi_esp)
root=$(partition_json sunuefi_root)
present_esp=false; present_root=false
[ ! -b /dev/block/by-name/sunuefi_esp ] || present_esp=true
[ ! -b /dev/block/by-name/sunuefi_root ] || present_root=true
case "$present_esp:$present_root" in
  true:true) layout=existing ;;
  false:false) layout=absent ;;
  *) layout=incomplete ;;
esac

userdata_device=$(readlink -f /dev/block/by-name/userdata 2>/dev/null || true)
userdata_source=unknown; userdata_filesystem=unknown
while read -r source mountpoint filesystem remaining; do
  if [ "$mountpoint" = /data ]; then
    userdata_source=$source
    userdata_filesystem=$filesystem
    break
  fi
done < /proc/mounts
case "$userdata_source" in /dev/block/dm-[0-9]*) userdata_mapped=true ;; *) userdata_mapped=false ;; esac
# All string fields below come from fixed paths, kernel mount names or an
# allowlisted filesystem token, never a file name supplied by the WebUI.
case "$userdata_device" in /dev/block/sd[a-z][0-9]*) ;; *) userdata_device=unknown ;; esac
case "$userdata_source" in /dev/block/sd[a-z][0-9]*|/dev/block/dm-[0-9]*) ;; *) userdata_source=unknown ;; esac
case "$userdata_filesystem" in f2fs|ext4) ;; *) userdata_filesystem=unknown ;; esac

active_job=null
pending=false
expansion=null
if [ "$backend" = true ] && [ -f /data/adb/piano-sunuefi/storage/android-expansion.json ]; then
  /system/bin/sh "$MODDIR/storage-operations.sh" verify-expansion >/dev/null 2>/dev/null || true
fi
if [ -f /data/adb/piano-sunuefi/storage/android-expansion.json ]; then
  expansion=$(cat /data/adb/piano-sunuefi/storage/android-expansion.json)
  pending=true
  expansion_job=$(sed -n 's/.*"job_id":"\([0-9a-f]*\)".*/\1/p' /data/adb/piano-sunuefi/storage/android-expansion.json)
  if [ "${#expansion_job}" = 32 ]; then
    case "$expansion_job" in *[!0-9a-f]*) ;; *) active_job="\"$expansion_job\"" ;; esac
  fi
fi
if [ -f /data/adb/piano-sunuefi/storage/lock/job ]; then
  job=$(cat /data/adb/piano-sunuefi/storage/lock/job)
  if [ "${#job}" = 32 ]; then
    case "$job" in *[!0-9a-f]*) ;; *) active_job="\"$job\"" ;; esac
  fi
fi
sources='{"sources":[],"local_file_picker":false}'
if [ "$backend" = true ]; then
  sources=$("$MODDIR/bin/piano-storage" sources)
fi
case "$layout" in
  existing)
    [ "$backend" = false ] || delete=true
    if [ "$backend" = true ] && [ -x "$MODDIR/bin/piano-resize-f2fs" ]; then release=true; fi
    available=$(printf '%s' "$sources" | grep -o '"selected":true,"available":true' | wc -l)
    [ "$backend" = false ] || [ "$available" -ne 2 ] || flash=true
    create=false
    ;;
  absent) resize=false ;;
  incomplete) create=false; resize=false ;;
esac
create_reason=''
resize_reason=''
flash_reason=''
delete_reason=''
release_reason=''
android_grow=false
if [ -x /system/bin/resize.f2fs ] && [ -r /system/lib64/libfs_mgr.so ] &&
   grep -aFq /system/bin/resize.f2fs /system/lib64/libfs_mgr.so; then android_grow=true; fi
if [ "$create" = false ]; then
  if [ "$layout" != absent ]; then create_reason='已有 SunUEFI 分区，无需再次建立。'
  else create_reason='当前模块缺少 Android 数据分区调整组件。'; fi
fi
if [ "$resize" = false ]; then
  if [ "$layout" != existing ]; then resize_reason='需要先建立完整的 ESP 和 root 分区。'
  else resize_reason='当前 Android 缺少 ext4 检查或容量调整工具。'; fi
fi
if [ "$flash" = false ]; then
  if [ "$layout" != existing ]; then flash_reason='需要先建立完整的 ESP 和 root 分区。'
  elif [ "$backend" = false ]; then flash_reason='请更新模块以安装存储组件。'
  else flash_reason='请先选择设备上的 ESP 和 root 镜像文件。'; fi
fi
if [ "$delete" = false ]; then delete_reason='当前没有可删除的完整 SunUEFI 分区。'; fi
if [ "$release" = false ]; then
  if [ "$layout" != existing ]; then release_reason='当前没有可归还的完整 SunUEFI 分区。'
  else release_reason='请更新模块以安装 Android 扩容检查组件。'; fi
elif [ "$android_grow" = false ]; then
  release_reason='当前系统未检测到原厂自动扩容路径；重启后会检查实际容量，未扩大时继续显示待完成。'
fi
printf '{"operation":"storage-status","read_only":true,"layout":"%s","partitions":[%s,%s],"userdata":{"device":"%s","mounted_device":"%s","filesystem":"%s","mapped":%s},"active_job_id":%s,"pending_android_expansion":%s,"android_expansion":%s,"android_grow_detected":%s,"capabilities":{"status":true,"flash_existing":%s,"create":%s,"resize":%s,"delete":%s,"release_to_android":%s,"file_picker":%s,"existing_image_installation":"module","partition_management":"plan-and-confirm"},"reasons":{"flash_existing":"%s","create":"%s","resize":"%s","delete":"%s","release_to_android":"%s"},%s\n' \
  "$layout" "$esp" "$root" "$userdata_device" "$userdata_source" "$userdata_filesystem" "$userdata_mapped" "$active_job" "$pending" "$expansion" "$android_grow" "$flash" "$create" "$resize" "$delete" "$release" "$backend" "$flash_reason" "$create_reason" "$resize_reason" "$delete_reason" "$release_reason" "$(printf '%s' "$sources" | sed 's/^{//')"
