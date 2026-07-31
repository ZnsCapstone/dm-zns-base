#!/usr/bin/env bash
# WAL-only recovery smoke test. The underlying device is deliberately not
# reset between the two target creations.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-recovery}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
BLOCK_SIZE=4096
METADATA_ZONES=10
GC_RESERVE_ZONES=2

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
TMP_DIR=$(mktemp -d /tmp/dm-zns-recovery.XXXXXX)

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

fail() { echo "[FAIL] $*" >&2; exit 1; }

remove_target() {
	dmsetup remove --retry "$DM_NAME"
}

cleanup() {
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

echo "[*] First target creation and write"
insmod "$KO_PATH" || fail "first insmod failed"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "first dmsetup create failed"

dd if=/dev/urandom of="$TMP_DIR/payload" bs=1M count=4 status=none
dd if="$TMP_DIR/payload" of="$DM_DEV" bs=1M conv=fsync oflag=direct status=none ||
	fail "initial write failed"

remove_target || fail "first dmsetup remove failed"
rmmod "$MOD_NAME" || fail "first rmmod failed"

echo "[*] Recreating target without resetting $UNDERLYING"
insmod "$KO_PATH" || fail "second insmod failed"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "recovery dmsetup create failed"

dd if="$DM_DEV" of="$TMP_DIR/recovered" bs=1M count=4 iflag=direct status=none ||
	fail "recovery read failed"
cmp "$TMP_DIR/payload" "$TMP_DIR/recovered" || fail "WAL replay readback mismatch"

echo "[OK] WAL replay recovered 4 MiB after module reload"
