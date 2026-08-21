#!/usr/bin/env bash
# M3-A zone metadata and rollover test on a fresh zns-base target.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m3}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
WAL_PAGE_RECORDS=126
# auto caps every larger zone to 16 MiB for a bounded functional GC test.
# Use "full" for a device-scale endurance run, or supply an explicit MiB value.
M3_ZONE_CAPACITY_MIB=${M3_ZONE_CAPACITY_MIB:-auto}
MANIFEST_ZONES=2
WAL_ZONES=2
SSTABLE_ZONES=2
METADATA_ZONES=$((MANIFEST_ZONES + WAL_ZONES + SSTABLE_ZONES))
GC_RESERVE_ZONES=2

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
TMP_DIR=$(mktemp -d /tmp/dm-zns-m3.XXXXXX)

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
	rmmod "$MOD_NAME" 2>/dev/null || true
	rm -rf "$TMP_DIR"
}
trap cleanup EXIT

fail() {
	echo "[FAIL] $*" >&2
	exit 1
}

zone_wptr() {
	local zone_start_sector=$1

	blkzone report -o "$zone_start_sector" -c 1 "$UNDERLYING" |
		awk 'match($0, /wptr[[:space:]:]+0x[[:xdigit:]]+/) {
			value = substr($0, RSTART, RLENGTH);
			sub(/^wptr[[:space:]:]+/, "", value);
			print value;
			exit;
		}'
}

wait_for_persistent_memtable() {
	local expected_seq=$1
	local i status="" checkpoint_seq persistent_sstables

	for ((i = 0; i < 400; i++)); do
		status=$(dmsetup status "$DM_NAME") ||
			fail "could not read target status while waiting for MemTable persistence"
		checkpoint_seq=$(sed -n \
			's/.*checkpoint_seq=\([0-9][0-9]*\).*/\1/p' <<<"$status")
		persistent_sstables=$(sed -n \
			's/.*persistent_sstables=\([0-9][0-9]*\).*/\1/p' <<<"$status")
		if [ -n "$checkpoint_seq" ] && [ -n "$persistent_sstables" ] &&
		   [ "$checkpoint_seq" -ge "$expected_seq" ] &&
		   [ "$persistent_sstables" -gt 0 ]; then
			return 0
		fi
		sleep 0.05
	done

	fail "MemTable persistence did not reach checkpoint_seq=$expected_seq: ${status:-status unavailable}"
}

wait_for_gc_reclaim() {
	local i status="" resets moved error

	for ((i = 0; i < 1200; i++)); do
		status=$(dmsetup status "$DM_NAME") ||
			fail "could not read target status while waiting for GC"
		resets=$(sed -n 's/.*gc_resets=\([0-9][0-9]*\).*/\1/p' <<<"$status")
		moved=$(sed -n 's/.*gc_moved_blocks=\([0-9][0-9]*\).*/\1/p' <<<"$status")
		error=$(sed -n 's/.*gc_error=\(-\{0,1\}[0-9][0-9]*\).*/\1/p' <<<"$status")
		[ -n "$error" ] && [ "$error" -ne 0 ] &&
			fail "GC entered a fatal error state: $status"
		if [ -n "$resets" ] && [ -n "$moved" ] &&
		   [ "$resets" -gt 0 ] && [ "$moved" -gt 0 ]; then
			return 0
		fi
		sleep 0.05
	done

	fail "GC did not reclaim and migrate a live block within 60 seconds: ${status:-status unavailable}"
}

[ -b "$UNDERLYING" ] || fail "$UNDERLYING is missing. Run scripts/nullblk-up.sh first."

underlying_name=$(basename "$(readlink -f "$UNDERLYING")")
queue_dir="/sys/block/$underlying_name/queue"
zone_sectors=$(cat "$queue_dir/chunk_sectors" 2>/dev/null) ||
	fail "could not read zone size for $UNDERLYING"
nr_zones=$(cat "$queue_dir/nr_zones" 2>/dev/null) ||
	fail "could not read zone count for $UNDERLYING"
[ "$zone_sectors" -gt 0 ] || fail "$UNDERLYING is not a zoned device"
[ "$nr_zones" -gt $((METADATA_ZONES + GC_RESERVE_ZONES + 1)) ] ||
	fail "rollover test requires metadata, GC reserve, and two data zones"
[ $((zone_sectors % SECTORS_PER_BLOCK)) -eq 0 ] ||
	fail "zone capacity is not aligned to 4 KiB blocks"
[ $((zone_sectors % 2048)) -eq 0 ] ||
	fail "zone size must be an integral MiB for this test"

hardware_zone_mib=$((zone_sectors / 2048))
case "$M3_ZONE_CAPACITY_MIB" in
auto)
	if [ "$hardware_zone_mib" -gt 16 ]; then
		effective_zone_mib=16
	else
		effective_zone_mib=$hardware_zone_mib
	fi
	;;
full)
	effective_zone_mib=$hardware_zone_mib
	;;
*[!0-9]*|'')
	fail "M3_ZONE_CAPACITY_MIB must be auto, full, or a positive integer"
	;;
*)
	effective_zone_mib=$M3_ZONE_CAPACITY_MIB
	;;
esac
[ "$effective_zone_mib" -gt 0 ] || fail "effective data-zone capacity must be positive"
[ "$effective_zone_mib" -le "$hardware_zone_mib" ] ||
	fail "requested data-zone capacity exceeds the hardware zone size"
effective_zone_sectors=$((effective_zone_mib * 2048))
if [ "$effective_zone_sectors" -eq "$zone_sectors" ]; then
	module_zone_capacity_mib=0
else
	module_zone_capacity_mib=$effective_zone_mib
fi

zone_block_count=$((effective_zone_sectors / SECTORS_PER_BLOCK))
# block_count is the first WAL-page-aligned count beyond one data zone.  Leave
# 64 slots in the first MemTable, then stage WAL_PAGE_RECORDS-1 new mappings in
# stage 3.  Together with the block-0 overwrite they form one complete WAL
# page, cross the MemTable exactly once, and leave only 61 entries in the new
# active generation.
block_count=$((((zone_block_count + 1 + WAL_PAGE_RECORDS - 1) / WAL_PAGE_RECORDS) * WAL_PAGE_RECORDS))
m3_memtable_capacity=$((block_count + 64))
memtable_flush_blocks=$((WAL_PAGE_RECORDS - 1))
rollover_second_zone_blocks=$((block_count - zone_block_count))
total_bytes=$((block_count * BLOCK_SIZE))

data_zones=$((nr_zones - METADATA_ZONES))
logical_data_zones=$((data_zones - GC_RESERVE_ZONES))
sectors=$((logical_data_zones * effective_zone_sectors))
# Stage 2 leaves two data zones in use.  When deterministic victim rewrites
# activate a third one, free data zones become data_zones-3.  Trigger and stop
# above that point instead of filling 80% of a multi-GiB FEMU namespace.
gc_test_watermark=$((data_zones - 3))
gc_test_target=$((gc_test_watermark + 1))
[ "$gc_test_watermark" -gt 0 ] || fail "deterministic GC test needs four data zones"

echo "[*] Building module"
make -C "$SRC_DIR" >/dev/null || fail "build failed"

remove_target 2>/dev/null || true
if lsmod | grep -q '^dm_zns_base '; then
	rmmod "$MOD_NAME" 2>/dev/null ||
		fail "$MOD_NAME is already loaded and in use. Remove existing zns-base dm devices first."
fi

if [ "$UNDERLYING" = "/dev/nullb0" ]; then
	echo "[*] Resetting $UNDERLYING ($nr_zones zones)"
	blkzone reset -o 0 -c "$nr_zones" "$UNDERLYING" ||
		fail "failed to reset $UNDERLYING"
else
	echo "[*] $UNDERLYING must be reset before this in-memory-mapping test"
fi

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" data_zone_capacity_mib="$module_zone_capacity_mib" \
	gc_low_watermark="$gc_test_watermark" \
	gc_target_free_zones="$gc_test_target" \
	memtable_capacity_entries="$m3_memtable_capacity" ||
	fail "insmod failed"

echo "[*] dmsetup create $DM_NAME"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "dmsetup create failed"

# Cross the first data-zone boundary and continue through the next complete
# WAL page.  Merely writing zone_blocks+1 leaves up to 125 mappings in the
# partial in-memory WAL page; stage 3 deliberately completes the enlarged
# MemTable after this rollover write.

echo
echo "=== [1/6] underlying zone geometry ==="
echo "hardware zone=${hardware_zone_mib}MiB, effective data zone=${effective_zone_mib}MiB, zones=$nr_zones"
echo "data start=$METADATA_ZONES, rollover write=$total_bytes bytes"
echo "[OK]"

echo
echo "=== [2/6] write/read across the first two data zones ==="
dd if=/dev/zero bs="$BLOCK_SIZE" count="$block_count" status=none |
	tr '\000' '\132' > "$TMP_DIR/rollover.bin"
dd if="$TMP_DIR/rollover.bin" of="$DM_DEV" bs="$BLOCK_SIZE" \
	iflag=fullblock oflag=direct status=none ||
	fail "rollover write failed"
dd if="$DM_DEV" of="$TMP_DIR/rollover.read" bs="$BLOCK_SIZE" \
	count="$block_count" iflag=direct status=none ||
	fail "rollover read failed"
cmp "$TMP_DIR/rollover.bin" "$TMP_DIR/rollover.read" ||
	fail "rollover readback mismatch"
echo "[OK]"

echo
echo "=== [3/6] overwrite maps an old block into the next data zone ==="
dd if=/dev/zero bs="$BLOCK_SIZE" count=1 status=none |
	tr '\000' '\245' > "$TMP_DIR/overwrite.bin"
dd if="$TMP_DIR/overwrite.bin" of="$DM_DEV" bs="$BLOCK_SIZE" count=1 \
	seek=0 oflag=direct conv=notrunc status=none ||
	fail "overwrite of logical block 0 failed"
dd if="$DM_DEV" of="$TMP_DIR/overwrite.read" bs="$BLOCK_SIZE" count=1 \
	skip=0 iflag=direct status=none ||
	fail "read of overwritten logical block failed"
dd if="$DM_DEV" of="$TMP_DIR/neighbor.read" bs="$BLOCK_SIZE" count=1 \
	skip=1 iflag=direct status=none ||
	fail "read of neighboring logical block failed"
dd if="$TMP_DIR/rollover.bin" of="$TMP_DIR/neighbor.expected" \
	bs="$BLOCK_SIZE" count=1 skip=1 status=none
cmp "$TMP_DIR/overwrite.bin" "$TMP_DIR/overwrite.read" ||
	fail "overwrite did not become the newest mapping"
cmp "$TMP_DIR/neighbor.expected" "$TMP_DIR/neighbor.read" ||
	fail "overwrite corrupted neighboring logical block"

# Fill the deliberately enlarged first MemTable and cross it once.  These 125
# records plus the preceding block-0 overwrite make a complete 126-record WAL
# page, so persistence does not depend on userspace fsync behavior.  The next
# victim-overwrite generation remains below the new MemTable's capacity.
dd if=/dev/zero bs="$BLOCK_SIZE" count="$memtable_flush_blocks" status=none |
	tr '\000' '\307' > "$TMP_DIR/memtable-flush.bin"
dd if="$TMP_DIR/memtable-flush.bin" of="$DM_DEV" bs="$BLOCK_SIZE" \
	count="$memtable_flush_blocks" seek="$block_count" iflag=fullblock \
	oflag=direct conv=notrunc,fsync status=none ||
	fail "MemTable flush trigger write failed"
echo "[OK]"

echo
echo "=== [4/6] metadata is role-isolated; data zones roll over ==="

# WAL publication and the persistent MemTable worker are asynchronous.  Wait
# until the first full MemTable is represented by an SSTable and Manifest
# before inspecting their physical write pointers.
wait_for_persistent_memtable "$zone_block_count"

# The rollover write fills a MemTable. Persistent LSM flushes therefore append
# SSTables and publish a Manifest; this is metadata, not a user-data write
# leaking into metadata zones.
manifest_advanced=0
sstable_advanced=0
for ((i = 0; i < MANIFEST_ZONES; i++)); do
	metadata_wptr_hex=$(zone_wptr $((i * zone_sectors)))
	[ -n "$metadata_wptr_hex" ] || fail "could not parse manifest zone $i write pointer"
	[ $((metadata_wptr_hex)) -gt 0 ] && manifest_advanced=1
done
for ((i = MANIFEST_ZONES + WAL_ZONES; i < METADATA_ZONES; i++)); do
	metadata_wptr_hex=$(zone_wptr $((i * zone_sectors)))
	[ -n "$metadata_wptr_hex" ] || fail "could not parse SSTable zone $i write pointer"
	[ $((metadata_wptr_hex)) -gt 0 ] && sstable_advanced=1
done
[ "$manifest_advanced" -eq 1 ] || fail "MemTable flush did not publish a Manifest"
[ "$sstable_advanced" -eq 1 ] || fail "MemTable flush did not write an SSTable"

# Foreground writes must produce durable WAL pages in the first WAL zone.
wal_wptr_hex=$(zone_wptr $((MANIFEST_ZONES * zone_sectors)))
[ -n "$wal_wptr_hex" ] || fail "could not parse WAL zone write pointer"
[ $((wal_wptr_hex)) -gt 0 ] || fail "WAL zone did not receive WAL records"

data_start_sector=$((METADATA_ZONES * zone_sectors))
next_data_sector=$((data_start_sector + zone_sectors))
data0_wptr_hex=$(zone_wptr "$data_start_sector")
data1_wptr_hex=$(zone_wptr "$next_data_sector")

[ -n "$data0_wptr_hex" ] || fail "could not parse first data zone write pointer"
[ -n "$data1_wptr_hex" ] || fail "could not parse second data zone write pointer"

data0_wptr=$((data0_wptr_hex))
data1_wptr=$((data1_wptr_hex))
expected_data0_wptr=$effective_zone_sectors
# blkzone report prints the write pointer relative to each zone start.
expected_data1_wptr=$((SECTORS_PER_BLOCK *
	(rollover_second_zone_blocks + 1 + memtable_flush_blocks)))

echo "data zone $METADATA_ZONES wp=$data0_wptr, expected=$expected_data0_wptr"
echo "data zone $((METADATA_ZONES + 1)) wp=$data1_wptr, expected=$expected_data1_wptr"

[ "$data0_wptr" -eq "$expected_data0_wptr" ] ||
	fail "first data zone did not reach the effective rollover boundary"
[ "$data1_wptr" -eq "$expected_data1_wptr" ] ||
	fail "second data zone did not receive rollover and overwrite blocks"
echo "[OK]"

echo
echo "=== [5/6] GC reclaims one deterministic victim zone ==="
# Stage 2 placed logical blocks 0..zone_block_count-1 in the first physical
# data zone.  Block 0 was already overwritten in stage 3.  Overwrite every
# block except block 1 once more: the old first zone is then an unambiguous
# victim with one live marker, while only one additional effective zone worth
# of foreground data is written.  This keeps FEMU's 2 GiB hardware-zone test
# bounded to about 16 MiB without weakening the reset/migration assertion.
victim_overwrite_blocks=$((zone_block_count - 2))
dd if=/dev/zero bs="$BLOCK_SIZE" count="$victim_overwrite_blocks" status=none |
	tr '\000' '\074' > "$TMP_DIR/victim-overwrite.bin"
dd if="$TMP_DIR/victim-overwrite.bin" of="$DM_DEV" bs="$BLOCK_SIZE" \
	count="$victim_overwrite_blocks" seek=2 iflag=fullblock \
	oflag=direct conv=notrunc,fsync status=none ||
	fail "deterministic victim overwrite failed"

# The rollover leaves fewer than WAL_PAGE_RECORDS records after the new data
# zone is activated.  fsync above is therefore essential: it publishes that
# partial WAL page, after which the free-zone watermark can schedule GC.
wait_for_gc_reclaim

target_status=$(dmsetup status "$DM_NAME") || fail "could not read target status after GC"
gc_resets=$(sed -n 's/.*gc_resets=\([0-9][0-9]*\).*/\1/p' <<<"$target_status")
gc_moved_blocks=$(sed -n 's/.*gc_moved_blocks=\([0-9][0-9]*\).*/\1/p' <<<"$target_status")
[ -n "$gc_resets" ] || fail "target status is missing gc_resets"
[ -n "$gc_moved_blocks" ] || fail "target status is missing gc_moved_blocks"
[ "$gc_resets" -gt 0 ] || fail "workload did not reset/reuse any data zone"
[ "$gc_moved_blocks" -gt 0 ] || fail "workload did not exercise live-block migration"

echo "deterministic overwrite=$((victim_overwrite_blocks * BLOCK_SIZE / 1024 / 1024))MiB"
echo "gc resets=$gc_resets, moved blocks=$gc_moved_blocks"
echo "[OK]"

echo
echo "=== [6/6] valid data survives GC migration ==="
dd if="$DM_DEV" of="$TMP_DIR/gc-marker.read" bs="$BLOCK_SIZE" \
	skip=1 count=1 iflag=direct status=none || fail "GC marker read failed"
cmp "$TMP_DIR/neighbor.expected" "$TMP_DIR/gc-marker.read" ||
	fail "live marker data was corrupted during GC"

# Also make sure GC did not let its older copy win over the foreground update.
dd if="$TMP_DIR/victim-overwrite.bin" of="$TMP_DIR/overwrite.expected" \
	bs="$BLOCK_SIZE" count=1 status=none
dd if="$DM_DEV" of="$TMP_DIR/post-gc-overwrite.read" bs="$BLOCK_SIZE" \
	skip=2 count=1 iflag=direct status=none ||
	fail "post-GC overwrite read failed"
cmp "$TMP_DIR/overwrite.expected" "$TMP_DIR/post-gc-overwrite.read" ||
	fail "GC replaced a newer foreground mapping with stale data"
echo "[OK]"

echo
echo "=== M3 GC AND ZONE REUSE CHECK PASSED ==="
