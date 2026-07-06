#!/usr/bin/env bash
# End-to-end smoke test for the M1 in-memory mapping path.
# build -> insmod -> dmsetup create -> verify conventional top device
# -> 4 KiB write/read -> overwrite/read -> several mapped reads.
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-m1).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m1}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
BLOCK_SIZE=4096

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
TMP_DIR=$(mktemp -d /tmp/dm-zns-m1.XXXXXX)

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod $MOD_NAME 2>/dev/null || true
	rm -rf "$TMP_DIR"
}
trap cleanup EXIT

fail() {
	echo "[FAIL] $*" >&2
	exit 1
}

make_block() {
	local label=$1
	local out=$2

	printf "%s\n" "$label" > "$out"
	dd if=/dev/zero bs=$BLOCK_SIZE count=1 2>/dev/null |
		dd of="$out" bs=$BLOCK_SIZE count=1 conv=notrunc 2>/dev/null
	printf "%s\n" "$label" | dd of="$out" bs=1 conv=notrunc 2>/dev/null
}

write_block() {
	local file=$1
	local lblock=$2

	dd if="$file" of="$DM_DEV" bs=$BLOCK_SIZE count=1 seek="$lblock" oflag=direct status=none
}

read_block() {
	local file=$1
	local lblock=$2

	dd if="$DM_DEV" of="$file" bs=$BLOCK_SIZE count=1 skip="$lblock" iflag=direct status=none
}

[ -b "$UNDERLYING" ] || fail "$UNDERLYING is missing. Run scripts/nullblk-up.sh first."

echo "[*] Building module"
make -C "$SRC_DIR" >/dev/null || fail "build failed"

dmsetup remove "$DM_NAME" 2>/dev/null || true
if lsmod | grep -q '^dm_zns_base '; then
	rmmod $MOD_NAME 2>/dev/null || fail "$MOD_NAME is already loaded and in use. Remove existing zns-base dm devices first."
fi

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" || fail "insmod failed"

sectors=$(blockdev --getsz "$UNDERLYING")
echo "[*] dmsetup create $DM_NAME (zns-base on $UNDERLYING, $sectors sectors)"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || fail "dmsetup create failed"

dm_block=$(basename "$(readlink -f "$DM_DEV")")
zoned=$(cat "/sys/block/$dm_block/queue/zoned")

echo
echo "=== [1/5] top device is conventional ==="
echo "$DM_DEV -> /dev/$dm_block, zoned=$zoned"
[ "$zoned" = "none" ] || fail "expected /sys/block/$dm_block/queue/zoned to be none"
echo "[OK]"

echo
echo "=== [2/5] first 4 KiB write/read ==="
make_block "first-write-logical-block-10" "$TMP_DIR/a.bin"
write_block "$TMP_DIR/a.bin" 10 || fail "write logical block 10 failed"
read_block "$TMP_DIR/a.read" 10 || fail "read logical block 10 failed"
cmp "$TMP_DIR/a.bin" "$TMP_DIR/a.read" || fail "first readback mismatch"
echo "[OK]"

echo
echo "=== [3/5] overwrite same logical block ==="
make_block "second-write-logical-block-10" "$TMP_DIR/b.bin"
write_block "$TMP_DIR/b.bin" 10 || fail "overwrite logical block 10 failed"
read_block "$TMP_DIR/b.read" 10 || fail "read overwritten logical block 10 failed"
cmp "$TMP_DIR/b.bin" "$TMP_DIR/b.read" || fail "overwrite readback mismatch"
echo "[OK]"

echo
echo "=== [4/5] independent logical blocks ==="
for lblock in 0 1 31 32 127; do
	make_block "logical-block-$lblock" "$TMP_DIR/block-$lblock.bin"
	write_block "$TMP_DIR/block-$lblock.bin" "$lblock" || fail "write logical block $lblock failed"
done
for lblock in 0 1 31 32 127; do
	read_block "$TMP_DIR/block-$lblock.read" "$lblock" || fail "read logical block $lblock failed"
	cmp "$TMP_DIR/block-$lblock.bin" "$TMP_DIR/block-$lblock.read" || fail "readback mismatch at logical block $lblock"
done
echo "[OK]"

echo
echo "=== [5/5] unmapped read fails for current M1-a policy ==="
if read_block "$TMP_DIR/unmapped.read" 9999 2>/dev/null; then
	fail "unmapped read unexpectedly succeeded"
else
	echo "[OK] unmapped read failed as expected"
fi

echo
echo "=== ALL M1 CHECKS PASSED ==="
