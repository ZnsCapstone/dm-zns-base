#!/usr/bin/env bash
# WIP: discard tombstone와 WAL replay 영속성을 검증한다.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
DATA_FILE=/tmp/discard-test-data
DISCARD_OFFSET=$((64 * 4096))
DISCARD_LENGTH=$((128 * 1024 * 1024))

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
	rm -f "$DATA_FILE"
}
trap cleanup EXIT

[ -b "$UNDERLYING" ] || { echo "[!] $UNDERLYING is missing." >&2; exit 1; }
[ -f "$KO_PATH" ] || make -C "$SRC_DIR" >/dev/null || exit 1

dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod "$MOD_NAME" 2>/dev/null || true
blkzone reset "$UNDERLYING"
sectors=$(blockdev --getsz "$UNDERLYING")

insmod "$KO_PATH" || exit 1
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || exit 1

dd if=/dev/urandom of="$DATA_FILE" bs=4K count=256 status=none
dd if="$DATA_FILE" of="$DM_DEV" bs=4K count=256 oflag=direct status=none
sync
# 범위 discard가 4KiB bio/WAL 수만 개로 다시 쪼개지면 제한 시간에 걸린다.
timeout 30s blkdiscard -o "$DISCARD_OFFSET" -l "$DISCARD_LENGTH" "$DM_DEV" || exit 1
sync

if dd if="$DM_DEV" bs=4K skip=64 count=128 iflag=direct status=none | cmp -n $((128 * 4096)) - /dev/zero; then
	echo "[OK] discard 직후 zero-read"
else
	echo "[FAIL] discard 직후 삭제 범위가 zero가 아님" >&2
	exit 3
fi

dmsetup remove "$DM_NAME"
rmmod "$MOD_NAME"
insmod "$KO_PATH" || exit 1
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || exit 1

if dd if="$DM_DEV" bs=4K skip=64 count=128 iflag=direct status=none | cmp -n $((128 * 4096)) - /dev/zero; then
	echo "[OK] WAL replay 후에도 discard 유지"
else
	echo "[FAIL] 재적재 후 삭제된 매핑이 부활함" >&2
	exit 3
fi

echo "=== ALL DISCARD CHECKS PASSED ==="
