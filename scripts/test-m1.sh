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
FLUSH_OFFSET_MB=${FLUSH_OFFSET_MB:-64}
FLUSH_SIZE_MB=${FLUSH_SIZE_MB:-80}
FLUSH_TIMEOUT=${FLUSH_TIMEOUT:-60s}
RUN_TEST_OFFSET_MB=${RUN_TEST_OFFSET_MB:-192}
RUN_FILL_SIZE_MB=${RUN_FILL_SIZE_MB:-17}
RUN_FLUSH_WAIT_SECONDS=${RUN_FLUSH_WAIT_SECONDS:-1}
RANDWRITE_SIZE_MB=${RANDWRITE_SIZE_MB:-100}
RANDWRITE_OFFSET_MB=${RANDWRITE_OFFSET_MB:-256}
RANDWRITE_IODEPTH=${RANDWRITE_IODEPTH:-32}
RANDWRITE_TIMEOUT=${RANDWRITE_TIMEOUT:-90s}
FSYNC_TEST_OFFSET_MB=${FSYNC_TEST_OFFSET_MB:-384}
FSYNC_TEST_SIZE_MB=${FSYNC_TEST_SIZE_MB:-16}
METADATA_ZONES=6
GC_RESERVE_ZONES=2

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
TMP_DIR=$(mktemp -d /tmp/dm-zns-m1.XXXXXX)

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

remove_target() {
	local i

	dmsetup remove "$DM_NAME" 2>/dev/null || true
	for ((i = 0; i < 50; i++)); do
		if ! dmsetup info "$DM_NAME" >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.1
		dmsetup remove --retry "$DM_NAME" 2>/dev/null || true
	done
	return 1
}

cleanup() {
	remove_target 2>/dev/null || true
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

make_pattern_file() {
	local out=$1
	local size_mb=$2

	dd if=/dev/zero bs=1M count="$size_mb" status=none |
		tr '\000' '\132' > "$out"
}

read_range() {
	local file=$1
	local offset_mb=$2
	local size_mb=$3
	local offset_blocks=$((offset_mb * 1024 * 1024 / BLOCK_SIZE))
	local block_count=$((size_mb * 1024 * 1024 / BLOCK_SIZE))

	dd if="$DM_DEV" of="$file" bs=$BLOCK_SIZE skip="$offset_blocks" count="$block_count" \
		iflag=direct status=none
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

remove_target 2>/dev/null || true
if lsmod | grep -q '^dm_zns_base '; then
	rmmod $MOD_NAME 2>/dev/null || fail "$MOD_NAME is already loaded and in use. Remove existing zns-base dm devices first."
fi

if [ "$UNDERLYING" = "/dev/nullb0" ]; then
	echo "[*] Resetting $UNDERLYING ($nr_zones zones)"
	blkzone reset -o 0 -c "$nr_zones" "$UNDERLYING" ||
		fail "failed to reset $UNDERLYING"
fi

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" || fail "insmod failed"

echo "[*] dmsetup create $DM_NAME (zns-base on $UNDERLYING, $sectors sectors)"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || fail "dmsetup create failed"

dm_block=$(dmsetup info -C --noheadings -o blkdevname "$DM_NAME" | xargs)
zoned=$(cat "/sys/block/$dm_block/queue/zoned")

echo
echo "=== [1/10] top device is conventional ==="
echo "$DM_DEV -> /dev/$dm_block, zoned=$zoned"
[ "$zoned" = "none" ] || fail "expected /sys/block/$dm_block/queue/zoned to be none"
echo "[OK]"

echo
echo "=== [2/10] first 4 KiB write/read ==="
make_block "first-write-logical-block-10" "$TMP_DIR/a.bin"
write_block "$TMP_DIR/a.bin" 10 || fail "write logical block 10 failed"
read_block "$TMP_DIR/a.read" 10 || fail "read logical block 10 failed"
cmp "$TMP_DIR/a.bin" "$TMP_DIR/a.read" || fail "first readback mismatch"
echo "[OK]"

echo
echo "=== [3/10] overwrite same logical block ==="
make_block "second-write-logical-block-10" "$TMP_DIR/b.bin"
write_block "$TMP_DIR/b.bin" 10 || fail "overwrite logical block 10 failed"
read_block "$TMP_DIR/b.read" 10 || fail "read overwritten logical block 10 failed"
cmp "$TMP_DIR/b.bin" "$TMP_DIR/b.read" || fail "overwrite readback mismatch"
echo "[OK]"

echo
echo "=== [4/10] independent logical blocks ==="
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
echo "=== [5/10] MemTable flush, persistent SSTable readback, and spare reuse ==="
flush_lblock=$((FLUSH_OFFSET_MB * 1024 * 1024 / BLOCK_SIZE))
make_pattern_file "$TMP_DIR/flush-pattern.bin" "$FLUSH_SIZE_MB"

timeout "$FLUSH_TIMEOUT" fio --name=memtable-flush \
	--filename="$DM_DEV" --rw=write --bs=$BLOCK_SIZE \
	--offset="${FLUSH_OFFSET_MB}M" --size="${FLUSH_SIZE_MB}M" \
	--ioengine=libaio --iodepth=32 --direct=1 \
	--buffer_pattern=0x5a --group_reporting \
	>/dev/null || fail "MemTable flush stress write failed or timed out"

read_range "$TMP_DIR/flush-pattern.read" "$FLUSH_OFFSET_MB" "$FLUSH_SIZE_MB" ||
	fail "full read after MemTable flush stress failed"
cmp "$TMP_DIR/flush-pattern.bin" "$TMP_DIR/flush-pattern.read" ||
	fail "persistent SSTable lookup readback mismatch after MemTable flush stress"

make_block "post-flush-overwrite" "$TMP_DIR/flush-overwrite.bin"
write_block "$TMP_DIR/flush-overwrite.bin" "$((flush_lblock + 123))" ||
	fail "post-flush overwrite failed"
read_block "$TMP_DIR/flush-overwrite.read" "$((flush_lblock + 123))" ||
	fail "post-flush overwrite read failed"
cmp "$TMP_DIR/flush-overwrite.bin" "$TMP_DIR/flush-overwrite.read" ||
	fail "post-flush overwrite readback mismatch"
echo "[OK]"

echo
echo "=== [6/10] newest persistent SSTable wins over an older SSTable ==="
run_test_lblock=$((RUN_TEST_OFFSET_MB * 1024 * 1024 / BLOCK_SIZE))

make_block "run-old-version" "$TMP_DIR/run-old.bin"
write_block "$TMP_DIR/run-old.bin" "$run_test_lblock" ||
	fail "write old SSTable version failed"
timeout "$FLUSH_TIMEOUT" fio --name=flush-old-run \
	--filename="$DM_DEV" --rw=write --bs=$BLOCK_SIZE \
	--offset=320M --size="${RUN_FILL_SIZE_MB}M" \
	--ioengine=libaio --iodepth=1 --direct=1 --group_reporting \
	>/dev/null || fail "failed to flush old SSTable version"
sleep "$RUN_FLUSH_WAIT_SECONDS"

make_block "run-new-version" "$TMP_DIR/run-new.bin"
write_block "$TMP_DIR/run-new.bin" "$run_test_lblock" ||
	fail "write new SSTable version failed"
timeout "$FLUSH_TIMEOUT" fio --name=flush-new-run \
	--filename="$DM_DEV" --rw=write --bs=$BLOCK_SIZE \
	--offset=352M --size="${RUN_FILL_SIZE_MB}M" \
	--ioengine=libaio --iodepth=1 --direct=1 --group_reporting \
	>/dev/null || fail "failed to flush new SSTable version"
sleep "$RUN_FLUSH_WAIT_SECONDS"

read_block "$TMP_DIR/run-new.read" "$run_test_lblock" ||
	fail "read newest SSTable version failed"
cmp "$TMP_DIR/run-new.bin" "$TMP_DIR/run-new.read" ||
	fail "older SSTable mapping won over newest SSTable mapping"
echo "[OK]"

echo
echo "=== [7/10] random 4 KiB writes at iodepth 32 ==="
kernel_errors_before=$(dmesg | grep -c 'blk_update_request: I/O error' || true)
randwrite_count=$((RANDWRITE_SIZE_MB * 1024 * 1024 / BLOCK_SIZE))

timeout "$RANDWRITE_TIMEOUT" fio --name=randw \
	--filename="$DM_DEV" --rw=randwrite --bs=$BLOCK_SIZE \
	--offset="${RANDWRITE_OFFSET_MB}M" --size="${RANDWRITE_SIZE_MB}M" \
	--ioengine=libaio \
	--iodepth="$RANDWRITE_IODEPTH" --direct=1 --group_reporting \
	>/dev/null || fail "random write workload failed or timed out"

kernel_errors_after=$(dmesg | grep -c 'blk_update_request: I/O error' || true)
kernel_error_delta=$((kernel_errors_after - kernel_errors_before))
[ "$kernel_error_delta" -eq 0 ] ||
	fail "underlying device rejected $kernel_error_delta write request(s)"
echo "[OK] $randwrite_count random writes completed without new underlying I/O errors"

echo
echo "=== [8/10] flush bio pass-through ==="
timeout "$FLUSH_TIMEOUT" fio --name=flush-test \
	--filename="$DM_DEV" --rw=write --bs=$BLOCK_SIZE \
	--offset="${FSYNC_TEST_OFFSET_MB}M" --size="${FSYNC_TEST_SIZE_MB}M" \
	--ioengine=libaio --iodepth=1 --direct=1 --fsync=1 --group_reporting \
	>/dev/null || fail "write plus fsync workload failed"
echo "[OK] fsync completed after every 4 KiB write"

echo
echo "=== [9/10] unmapped read returns zeros ==="
dd if=/dev/zero of="$TMP_DIR/unmapped.zero" bs=$BLOCK_SIZE count=1 status=none
read_block "$TMP_DIR/unmapped.read" 9999 || fail "unmapped read failed"
cmp "$TMP_DIR/unmapped.zero" "$TMP_DIR/unmapped.read" ||
	fail "unmapped read was not zero-filled"
echo "[OK]"

echo
echo "=== [10/10] target status exposes zone, GC, and metadata state ==="
status=$(dmsetup status "$DM_NAME") || fail "dmsetup status failed"
printf '%s\n' "$status" | grep -q 'data_active=' ||
	fail "status is missing data zone counts"
printf '%s\n' "$status" | grep -q 'gc_runs=' ||
	fail "status is missing GC counters"
printf '%s\n' "$status" | grep -q 'wal_zone=' ||
	fail "status is missing WAL state"
printf '%s\n' "$status" | grep -q 'checkpoint_generation=' ||
	fail "status is missing checkpoint state"
sstable_count=$(printf '%s\n' "$status" | sed -n 's/.*persistent_sstables=\([0-9][0-9]*\).*/\1/p')
[ -n "$sstable_count" ] || fail "status is missing persistent SSTable count"
[ "$sstable_count" -lt 4 ] || fail "SSTable compaction did not reduce the table count: $sstable_count"
echo "[OK] $status"

echo
echo "=== ALL M1 CHECKS PASSED ==="
