#!/usr/bin/env bash
# M2 ext4 round-trip test on a fresh zns-base target.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m2}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
DATA_SIZE_MB=${DATA_SIZE_MB:-10}
BLOCK_SIZE=4096
SECTOR_SIZE=512
PARTIAL_READ_LBLOCK=16
PARTIAL_OVERWRITE_LBLOCK=17
CROSS_BLOCK_LBLOCK=20
METADATA_ZONES=10
GC_RESERVE_ZONES=2

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

make_filled_file() {
	local output=$1
	local bytes=$2
	local fill=$3

	dd if=/dev/zero bs=1 count="$bytes" status=none |
		tr '\000' "$fill" > "$output"
}

[ -b "$UNDERLYING" ] || fail "$UNDERLYING is missing. Run scripts/nullblk-up.sh first."

underlying_name=$(basename "$(readlink -f "$UNDERLYING")")
queue_dir="/sys/block/$underlying_name/queue"
zone_sectors=$(cat "$queue_dir/chunk_sectors" 2>/dev/null) ||
	fail "could not read zone size for $UNDERLYING"
nr_zones=$(cat "$queue_dir/nr_zones" 2>/dev/null) ||
	fail "could not read zone count for $UNDERLYING"
[ "$nr_zones" -gt $((METADATA_ZONES + GC_RESERVE_ZONES)) ] ||
	fail "not enough zones for metadata and GC reserve"

sectors=$(((nr_zones - METADATA_ZONES - GC_RESERVE_ZONES) * zone_sectors))

echo "[*] Building module"
make -C "$SRC_DIR" >/dev/null || fail "build failed"

dmsetup remove "$DM_NAME" 2>/dev/null || true
if lsmod | grep -q '^dm_zns_base '; then
	rmmod "$MOD_NAME" 2>/dev/null ||
		fail "$MOD_NAME is already loaded and in use. Remove existing zns-base dm devices first."
fi

if [ "$UNDERLYING" = "/dev/nullb0" ]; then
	echo "[*] Resetting $UNDERLYING ($nr_zones zones)"
	blkzone reset -o 0 -c "$nr_zones" "$UNDERLYING" ||
		fail "failed to reset $UNDERLYING"
fi

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" || fail "insmod failed"

echo "[*] dmsetup create $DM_NAME"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "dmsetup create failed"

mkdir -p "$MOUNT_DIR"

echo
echo "=== [1/7] mapped 1024B read ==="
make_filled_file "$TMP_DIR/partial-read-base.bin" "$BLOCK_SIZE" A
dd if="$TMP_DIR/partial-read-base.bin" of="$DM_DEV" bs="$BLOCK_SIZE" count=1 \
	seek="$PARTIAL_READ_LBLOCK" oflag=direct conv=notrunc status=none ||
	fail "4 KiB setup write for 1024B read failed"
dd if="$DM_DEV" of="$TMP_DIR/partial-read.actual" bs=1024 count=1 \
	skip=$((PARTIAL_READ_LBLOCK * 4 + 1)) iflag=direct status=none ||
	fail "mapped 1024B read failed"
dd if="$TMP_DIR/partial-read-base.bin" of="$TMP_DIR/partial-read.expected" \
	bs=1024 count=1 skip=1 status=none
cmp "$TMP_DIR/partial-read.expected" "$TMP_DIR/partial-read.actual" ||
	fail "mapped 1024B read returned incorrect data"
echo "[OK]"

echo
echo "=== [2/7] 512B overwrite preserves the rest of a 4 KiB block ==="
make_filled_file "$TMP_DIR/overwrite-base.bin" "$BLOCK_SIZE" B
make_filled_file "$TMP_DIR/overwrite-patch.bin" "$SECTOR_SIZE" Z
cp "$TMP_DIR/overwrite-base.bin" "$TMP_DIR/overwrite.expected"
dd if="$TMP_DIR/overwrite-base.bin" of="$DM_DEV" bs="$BLOCK_SIZE" count=1 \
	seek="$PARTIAL_OVERWRITE_LBLOCK" oflag=direct conv=notrunc status=none ||
	fail "4 KiB setup write for 512B overwrite failed"
dd if="$TMP_DIR/overwrite-patch.bin" of="$DM_DEV" bs="$SECTOR_SIZE" count=1 \
	seek=$((PARTIAL_OVERWRITE_LBLOCK * 8 + 4)) oflag=direct conv=notrunc status=none ||
	fail "512B overwrite failed"
dd if="$TMP_DIR/overwrite-patch.bin" of="$TMP_DIR/overwrite.expected" \
	bs="$SECTOR_SIZE" count=1 seek=4 conv=notrunc status=none
dd if="$DM_DEV" of="$TMP_DIR/overwrite.actual" bs="$BLOCK_SIZE" count=1 \
	skip="$PARTIAL_OVERWRITE_LBLOCK" iflag=direct status=none ||
	fail "full read after 512B overwrite failed"
cmp "$TMP_DIR/overwrite.expected" "$TMP_DIR/overwrite.actual" ||
	fail "512B overwrite did not preserve unchanged bytes"
echo "[OK]"

echo
echo "=== [3/7] 7 KiB write/read across a 4 KiB boundary ==="
dd if=/dev/urandom of="$TMP_DIR/cross-block.bin" bs=1024 count=7 status=none
dd if="$TMP_DIR/cross-block.bin" of="$DM_DEV" bs=1024 count=7 \
	seek=$((CROSS_BLOCK_LBLOCK * 4)) oflag=direct conv=notrunc status=none ||
	fail "7 KiB cross-block write failed"
dd if="$DM_DEV" of="$TMP_DIR/cross-block.actual" bs=1024 count=7 \
	skip=$((CROSS_BLOCK_LBLOCK * 4)) iflag=direct status=none ||
	fail "7 KiB cross-block read failed"
cmp "$TMP_DIR/cross-block.bin" "$TMP_DIR/cross-block.actual" ||
	fail "7 KiB cross-block readback mismatch"
echo "[OK]"

echo
echo "=== [4/7] mkfs.ext4 without discard ==="
mkfs.ext4 -F -E nodiscard "$DM_DEV" >/dev/null || fail "mkfs.ext4 failed"
echo "[OK]"

echo
echo "=== [5/7] mount and write random data ==="
mount "$DM_DEV" "$MOUNT_DIR" || fail "mount failed"
dd if=/dev/urandom of="$MOUNT_DIR/data" bs=1M count="$DATA_SIZE_MB" status=none ||
	fail "file write failed"
hash_a=$(md5sum "$MOUNT_DIR/data" | awk '{print $1}') || fail "first md5sum failed"
echo "hash A: $hash_a"
echo "[OK]"

echo
echo "=== [6/7] sync and unmount ==="
sync
umount "$MOUNT_DIR" || fail "umount failed"
echo "[OK]"

echo
echo "=== [7/7] remount and verify data ==="
mount "$DM_DEV" "$MOUNT_DIR" || fail "remount failed"
hash_b=$(md5sum "$MOUNT_DIR/data" | awk '{print $1}') || fail "second md5sum failed"
echo "hash B: $hash_b"
[ "$hash_a" = "$hash_b" ] || fail "hash mismatch after remount"
umount "$MOUNT_DIR" || fail "final umount failed"
echo "[OK]"

echo
echo "=== M2 EXT4 ROUND-TRIP PASSED ==="
