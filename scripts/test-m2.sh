#!/usr/bin/env bash
# M2 ext4 round-trip test on a fresh zns-base target.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m2}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
DATA_SIZE_MB=${DATA_SIZE_MB:-10}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
TMP_DIR=$(mktemp -d /tmp/dm-zns-m2.XXXXXX)
MOUNT_DIR="$TMP_DIR/mnt"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	if mountpoint -q "$MOUNT_DIR"; then
		umount "$MOUNT_DIR" 2>/dev/null || true
	fi
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
	rm -rf "$TMP_DIR"
}
trap cleanup EXIT

fail() {
	echo "[FAIL] $*" >&2
	exit 1
}

[ -b "$UNDERLYING" ] || fail "$UNDERLYING is missing. Run scripts/nullblk-up.sh first."

echo "[*] Building module"
make -C "$SRC_DIR" >/dev/null || fail "build failed"

dmsetup remove "$DM_NAME" 2>/dev/null || true
if lsmod | grep -q '^dm_zns_base '; then
	rmmod "$MOD_NAME" 2>/dev/null ||
		fail "$MOD_NAME is already loaded and in use. Remove existing zns-base dm devices first."
fi

if [ "$UNDERLYING" = "/dev/nullb0" ]; then
	nr_zones=$(cat /sys/block/nullb0/queue/nr_zones)
	echo "[*] Resetting $UNDERLYING ($nr_zones zones)"
	blkzone reset -o 0 -c "$nr_zones" "$UNDERLYING" ||
		fail "failed to reset $UNDERLYING"
fi

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" || fail "insmod failed"

sectors=$(blockdev --getsz "$UNDERLYING")
echo "[*] dmsetup create $DM_NAME"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "dmsetup create failed"

mkdir -p "$MOUNT_DIR"

echo
echo "=== [1/4] mkfs.ext4 without discard ==="
mkfs.ext4 -F -E nodiscard "$DM_DEV" >/dev/null || fail "mkfs.ext4 failed"
echo "[OK]"

echo
echo "=== [2/4] mount and write random data ==="
mount "$DM_DEV" "$MOUNT_DIR" || fail "mount failed"
dd if=/dev/urandom of="$MOUNT_DIR/data" bs=1M count="$DATA_SIZE_MB" status=none ||
	fail "file write failed"
hash_a=$(md5sum "$MOUNT_DIR/data" | awk '{print $1}') || fail "first md5sum failed"
echo "hash A: $hash_a"
echo "[OK]"

echo
echo "=== [3/4] sync and unmount ==="
sync
umount "$MOUNT_DIR" || fail "umount failed"
echo "[OK]"

echo
echo "=== [4/4] remount and verify data ==="
mount "$DM_DEV" "$MOUNT_DIR" || fail "remount failed"
hash_b=$(md5sum "$MOUNT_DIR/data" | awk '{print $1}') || fail "second md5sum failed"
echo "hash B: $hash_b"
[ "$hash_a" = "$hash_b" ] || fail "hash mismatch after remount"
umount "$MOUNT_DIR" || fail "final umount failed"
echo "[OK]"

echo
echo "=== M2 EXT4 ROUND-TRIP PASSED ==="
