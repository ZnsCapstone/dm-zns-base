#!/usr/bin/env bash
# ext4 round-trip across dm-zns-base module reload without resetting media.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-recovery-ext4}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
METADATA_ZONES=6
GC_RESERVE_ZONES=2
DATA_MIB=${DATA_MIB:-10}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
TMP_DIR=$(mktemp -d /tmp/dm-zns-recovery-ext4.XXXXXX)
MOUNT_DIR="$TMP_DIR/mnt"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

fail() { echo "[FAIL] $*" >&2; exit 1; }

remove_target() {
	dmsetup remove --retry "$DM_NAME"
}

cleanup() {
	mountpoint -q "$MOUNT_DIR" && umount "$MOUNT_DIR" || true
	remove_target 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
	rm -rf "$TMP_DIR"
}
trap cleanup EXIT

[ -b "$UNDERLYING" ] || fail "$UNDERLYING is missing. Run scripts/nullblk-up.sh first."

underlying_name=$(basename "$(readlink -f "$UNDERLYING")")
queue_dir="/sys/block/$underlying_name/queue"
zone_sectors=$(cat "$queue_dir/chunk_sectors") || fail "cannot read zone size"
nr_zones=$(cat "$queue_dir/nr_zones") || fail "cannot read zone count"
sectors=$(((nr_zones - METADATA_ZONES - GC_RESERVE_ZONES) * zone_sectors))

echo "[*] Building module"
make -C "$SRC_DIR" >/dev/null || fail "build failed"

remove_target 2>/dev/null || true
rmmod "$MOD_NAME" 2>/dev/null || true
blkzone reset -o 0 -c "$nr_zones" "$UNDERLYING" || fail "initial reset failed"
mkdir -p "$MOUNT_DIR"

echo "[*] First target creation and ext4 write"
insmod "$KO_PATH" || fail "first insmod failed"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "first dmsetup create failed"

mkfs.ext4 -F -E nodiscard "$DM_DEV" >/dev/null || fail "mkfs.ext4 failed"
mount "$DM_DEV" "$MOUNT_DIR" || fail "first mount failed"
dd if=/dev/urandom of="$MOUNT_DIR/data" bs=1M count="$DATA_MIB" conv=fsync status=none ||
	fail "file write failed"
hash_a=$(md5sum "$MOUNT_DIR/data" | awk '{print $1}')
sync
umount "$MOUNT_DIR" || fail "first umount failed"

remove_target || fail "first dmsetup remove failed"
rmmod "$MOD_NAME" || fail "first rmmod failed"

echo "[*] Recreating target without resetting $UNDERLYING"
insmod "$KO_PATH" || fail "second insmod failed"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "recovery dmsetup create failed"
mount "$DM_DEV" "$MOUNT_DIR" || fail "recovery mount failed"
hash_b=$(md5sum "$MOUNT_DIR/data" | awk '{print $1}')
umount "$MOUNT_DIR" || fail "recovery umount failed"

[ "$hash_a" = "$hash_b" ] || fail "ext4 hash mismatch after WAL recovery"
echo "[OK] ext4 WAL recovery hash: $hash_b"
