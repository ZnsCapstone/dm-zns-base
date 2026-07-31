#!/usr/bin/env bash
# M3-A zone metadata and rollover test on a fresh zns-base target.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m3}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
GC_WORKING_SET_MIB=${GC_WORKING_SET_MIB:-0}
GC_MARKER_MIB=${GC_MARKER_MIB:-4}
MANIFEST_ZONES=2
WAL_ZONES=2
SSTABLE_ZONES=6
METADATA_ZONES=$((MANIFEST_ZONES + WAL_ZONES + SSTABLE_ZONES))
GC_RESERVE_ZONES=2

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
TMP_DIR=$(mktemp -d /tmp/dm-zns-m3.XXXXXX)

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	dmsetup remove "$DM_NAME" 2>/dev/null || true
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
data_zones=$((nr_zones - METADATA_ZONES))
logical_data_zones=$((data_zones - GC_RESERVE_ZONES))
sectors=$((logical_data_zones * zone_sectors))
logical_mib=$((logical_data_zones * zone_sectors / 2048))
if [ "$GC_WORKING_SET_MIB" -eq 0 ]; then
	GC_WORKING_SET_MIB=$((logical_mib * 80 / 100))
fi
GC_OVERWRITE_MIB=${GC_OVERWRITE_MIB:-$((GC_WORKING_SET_MIB * 12 / 10))}
gc_marker_offset_mib=$((GC_WORKING_SET_MIB + 16))

[ "$GC_WORKING_SET_MIB" -gt 0 ] || fail "GC working set must be positive"
[ "$GC_WORKING_SET_MIB" -le "$logical_mib" ] ||
	fail "GC working set must fit in top logical capacity"
[ "$GC_OVERWRITE_MIB" -gt "$GC_WORKING_SET_MIB" ] ||
	fail "GC overwrite amount must exceed the working set"
[ $((gc_marker_offset_mib + GC_MARKER_MIB)) -lt "$logical_mib" ] ||
	fail "GC marker range must fit outside the random-write working set"

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
else
	echo "[*] $UNDERLYING must be reset before this in-memory-mapping test"
fi

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" || fail "insmod failed"

echo "[*] dmsetup create $DM_NAME"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "dmsetup create failed"

block_count=$((zone_sectors / SECTORS_PER_BLOCK + 1))
total_bytes=$((block_count * BLOCK_SIZE))

echo
echo "=== [1/6] underlying zone geometry ==="
echo "zone sectors=$zone_sectors, zones=$nr_zones, data start=$METADATA_ZONES, rollover write=$total_bytes bytes"
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
echo "[OK]"

echo
echo "=== [4/6] metadata is role-isolated; data zones roll over ==="

# A 64 MiB rollover write fills multiple MemTables. Persistent LSM flushes
# therefore append SSTables and publish a Manifest; this is metadata, not a
# user-data write leaking into metadata zones.
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
expected_data0_wptr=$zone_sectors
# blkzone report prints the write pointer relative to each zone start.
expected_data1_wptr=$((SECTORS_PER_BLOCK * 2))

echo "data zone $METADATA_ZONES wp=$data0_wptr, expected=$expected_data0_wptr"
echo "data zone $((METADATA_ZONES + 1)) wp=$data1_wptr, expected=$expected_data1_wptr"

[ "$data0_wptr" -eq "$expected_data0_wptr" ] ||
	fail "first data zone is not full after rollover write"
[ "$data1_wptr" -eq "$expected_data1_wptr" ] ||
	fail "second data zone did not receive rollover and overwrite blocks"
echo "[OK]"

echo
echo "=== [5/6] GC reclaims space during 80% fill and 1.2x random overwrite ==="
dd if=/dev/urandom of="$TMP_DIR/gc-marker.bin" bs=1M \
	count="$GC_MARKER_MIB" status=none
dd if="$TMP_DIR/gc-marker.bin" of="$DM_DEV" bs=1M \
	seek="$gc_marker_offset_mib" conv=notrunc oflag=direct status=none ||
	fail "GC marker write failed"

fio --name=gc-fill --filename="$DM_DEV" --rw=randwrite \
	--bs=4k --size="${GC_WORKING_SET_MIB}M" --io_size="${GC_WORKING_SET_MIB}M" \
	--ioengine=libaio --iodepth=32 --direct=1 --group_reporting ||
	fail "80% random fill failed"

fio --name=gc-overwrite --filename="$DM_DEV" --rw=randwrite \
	--bs=4k --size="${GC_WORKING_SET_MIB}M" --io_size="${GC_OVERWRITE_MIB}M" \
	--ioengine=libaio --iodepth=32 --direct=1 --group_reporting ||
	fail "1.2x random overwrite failed"
echo "working set=${GC_WORKING_SET_MIB}MiB (80% of ${logical_mib}MiB), overwrite=${GC_OVERWRITE_MIB}MiB"
echo "[OK]"

echo
echo "=== [6/6] valid data survives GC migration ==="
dd if="$DM_DEV" of="$TMP_DIR/gc-marker.read" bs=1M \
	skip="$gc_marker_offset_mib" count="$GC_MARKER_MIB" \
	iflag=direct status=none || fail "GC marker read failed"
cmp "$TMP_DIR/gc-marker.bin" "$TMP_DIR/gc-marker.read" ||
	fail "valid marker data was corrupted during GC"
echo "[OK]"

echo
echo "=== M3 GC AND ZONE REUSE CHECK PASSED ==="
