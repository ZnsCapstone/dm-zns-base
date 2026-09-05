#!/usr/bin/env bash
# Validates recovery after one-shot write/metadata-persistence failpoints.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-failpoint-$$}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm_zns_base
KO_NAME=dm-zns-base
METADATA_ZONES=6
GC_RESERVE_ZONES=2
CHECKPOINT_WRITE_MIB=${CHECKPOINT_WRITE_MIB:-132}
CHECK_PREFIX_MIB=${CHECK_PREFIX_MIB:-120}
# A 4 KiB WAL page contains 126 records.  The extra record attempts to stage
# behind the full page and therefore observes the injected flush error.
WAL_PAGE_RECORDS=126
WAL_TRIGGER_RECORDS=$((WAL_PAGE_RECORDS + 1))
MEMTABLE_ENTRIES=4096
MEMTABLE_MIB=16

FAIL_AFTER_DATA_WRITE=1
FAIL_BEFORE_WAL_WRITE=2
FAIL_AFTER_SSTABLE_WRITE=3
FAIL_AFTER_MANIFEST_WRITE=4
FAIL_CORRUPT_WAL_PAGE_CRC=6

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
SRC_DIR="$REPO_DIR/src"
KO_PATH="$SRC_DIR/$KO_NAME.ko"
TMP_DIR=$(mktemp -d /tmp/dm-zns-failpoint.XXXXXX)
FAIL_PARAM="/sys/module/$MOD_NAME/parameters/failpoint"

[ "$(id -u)" -eq 0 ] || {
	echo "Run with sudo." >&2
	exit 1
}

fail() {
	echo "[FAIL] $*" >&2
	exit 1
}

remove_target() {
	local i

	dmsetup remove "$DM_NAME" 2>/dev/null || true
	for ((i = 0; i < 50; i++)); do
		if ! dmsetup info "$DM_NAME" >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.1
		dmsetup remove "$DM_NAME" 2>/dev/null || true
	done

	return 1
}

cleanup() {
	remove_target 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
	rm -rf "$TMP_DIR"
}
trap cleanup EXIT

load_target() {
	insmod "$KO_PATH" memtable_capacity_entries="$MEMTABLE_ENTRIES" ||
		fail "insmod failed"
	echo "0 $SECTORS zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
		fail "dmsetup create failed"
}

reload_target() {
	remove_target || fail "dmsetup remove failed before reload"
	rmmod "$MOD_NAME" || fail "rmmod failed before reload"
	load_target
}

prepare_empty_target() {
	remove_target 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
	blkzone reset -o 0 -c "$NR_ZONES" "$UNDERLYING" ||
		fail "underlying reset failed"
	load_target
}

set_failpoint() {
	local point=$1

	[ -w "$FAIL_PARAM" ] || fail "failpoint parameter is unavailable"
	echo "$point" > "$FAIL_PARAM"
}

expect_failpoint_consumed() {
	local point=$1
	local current

	current=$(cat "$FAIL_PARAM") || fail "cannot read failpoint parameter"
	[ "$current" -eq 0 ] ||
		fail "failpoint $point was not reached (current=$current)"
}

wait_for_checkpoint_seq() {
	local expected=$1
	local i status current

	for ((i = 0; i < 200; i++)); do
		status=$(dmsetup status "$DM_NAME") ||
			fail "cannot read target status"
		current=$(sed -n \
			's/.*checkpoint_seq=\([0-9][0-9]*\).*/\1/p' <<<"$status")
		if [ -n "$current" ] && [ "$current" -ge "$expected" ]; then
			return 0
		fi
		sleep 0.05
	done

	fail "persistent MemTable flush did not reach checkpoint_seq=$expected"
}

expect_zero_block_after_reload() {
	dd if="$DM_DEV" of="$TMP_DIR/recovered-zero" bs=4K count=1 \
		iflag=direct status=none || fail "recovery read failed"
	cmp "$TMP_DIR/zero" "$TMP_DIR/recovered-zero" ||
		fail "failed write became visible after recovery"
}

write_failure_case() {
	local name=$1
	local point=$2
	local input=$3
	local count=$4

	echo
	echo "=== $name ==="
	prepare_empty_target
	set_failpoint "$point"

	if dd if="$input" of="$DM_DEV" bs=4K count="$count" \
		oflag=direct status=none; then
		fail "$name write unexpectedly succeeded"
	fi
	expect_failpoint_consumed "$point"

	reload_target
	expect_zero_block_after_reload
	echo "[OK] failed write was not recovered"
}

wal_crc_case() {
	echo
	echo "=== [3/5] WAL page CRC corruption ==="
	prepare_empty_target
	set_failpoint "$FAIL_CORRUPT_WAL_PAGE_CRC"

	# Fill one WAL page so CRC corruption is injected into an actual metadata
	# write rather than remaining armed on a partial in-memory page.  Record
	# 127 starts the next page; teardown flushes that valid partial page.
	dd if="$TMP_DIR/wal-payload" of="$DM_DEV" bs=4K \
		count="$WAL_TRIGGER_RECORDS" oflag=direct status=none ||
		fail "corrupted WAL write unexpectedly failed before recovery"
	expect_failpoint_consumed "$FAIL_CORRUPT_WAL_PAGE_CRC"

	reload_target
	expect_zero_block_after_reload
	echo "[OK] CRC-invalid WAL record was ignored during replay"
}

metadata_failure_case() {
	local name=$1
	local point=$2
	local tail_mib=$((CHECKPOINT_WRITE_MIB - CHECK_PREFIX_MIB))
	local prefix_checkpoint_seq=$((CHECK_PREFIX_MIB / MEMTABLE_MIB * MEMTABLE_ENTRIES))

	echo
	echo "=== $name ==="
	prepare_empty_target

	# Make the prefix durable before arming the failpoint.  Persistent
	# MemTables are emitted every 4096 mappings (16 MiB), so arming the
	# failpoint before this write would stop at the first flush and the old
	# test would incorrectly demand recovery of bytes that were never written.
	dd if="$TMP_DIR/payload" of="$DM_DEV" bs=1M \
		count="$CHECK_PREFIX_MIB" oflag=direct conv=fsync status=none ||
		fail "$name failed while writing the durable prefix"
	# fsync is a WAL durability boundary; the SSTable worker is independent.
	# Wait until every full MemTable in the prefix has published its Manifest so
	# the newly armed failpoint can only affect the following tail.
	wait_for_checkpoint_seq "$prefix_checkpoint_seq"

	set_failpoint "$point"

	if dd if="$TMP_DIR/payload" of="$DM_DEV" bs=1M \
		skip="$CHECK_PREFIX_MIB" seek="$CHECK_PREFIX_MIB" \
		count="$tail_mib" oflag=direct conv=fsync status=none; then
		fail "$name write unexpectedly succeeded"
	fi
	expect_failpoint_consumed "$point"

	reload_target
	dd if="$DM_DEV" of="$TMP_DIR/recovered-prefix" bs=1M \
		count="$CHECK_PREFIX_MIB" iflag=direct status=none ||
		fail "recovery prefix read failed"
	dd if="$TMP_DIR/payload" of="$TMP_DIR/expected-prefix" bs=1M \
		count="$CHECK_PREFIX_MIB" status=none
	cmp "$TMP_DIR/expected-prefix" "$TMP_DIR/recovered-prefix" ||
		fail "$name lost the durable prefix after recovery"
	echo "[OK] metadata interruption recovered the durable prefix"
}

[ -b "$UNDERLYING" ] ||
	fail "$UNDERLYING is missing. Run scripts/nullblk-up.sh first."

underlying_name=$(basename "$(readlink -f "$UNDERLYING")")
queue_dir="/sys/block/$underlying_name/queue"
ZONE_SECTORS=$(cat "$queue_dir/chunk_sectors") || fail "cannot read zone size"
NR_ZONES=$(cat "$queue_dir/nr_zones") || fail "cannot read zone count"
SECTORS=$(((NR_ZONES - METADATA_ZONES - GC_RESERVE_ZONES) * ZONE_SECTORS))

[ "$CHECKPOINT_WRITE_MIB" -gt "$CHECK_PREFIX_MIB" ] ||
	fail "checkpoint write size must exceed verified prefix"

echo "[*] Building module"
make -B -C "$SRC_DIR" >/dev/null || fail "build failed"

dd if=/dev/zero of="$TMP_DIR/zero" bs=4K count=1 status=none
dd if=/dev/urandom of="$TMP_DIR/marker" bs=4K count=1 status=none
dd if=/dev/urandom of="$TMP_DIR/wal-payload" bs=4K \
	count="$WAL_TRIGGER_RECORDS" status=none
dd if=/dev/urandom of="$TMP_DIR/payload" bs=1M \
	count="$CHECKPOINT_WRITE_MIB" status=none

write_failure_case "[1/5] data write succeeds, WAL is never staged" \
	"$FAIL_AFTER_DATA_WRITE" "$TMP_DIR/marker" 1
write_failure_case "[2/5] data write succeeds, WAL page write fails" \
	"$FAIL_BEFORE_WAL_WRITE" "$TMP_DIR/wal-payload" \
	"$WAL_TRIGGER_RECORDS"
wal_crc_case
metadata_failure_case "[4/5] MemTable persistence fails after SSTable write" \
	"$FAIL_AFTER_SSTABLE_WRITE"
metadata_failure_case "[5/5] MemTable persistence fails after Manifest write" \
	"$FAIL_AFTER_MANIFEST_WRITE"

echo
echo "=== FAILURE-INJECTION RECOVERY CHECKS PASSED ==="
