#!/usr/bin/env bash
# M3-A zone metadata and rollover test on a fresh zns-base target.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m3}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
GC_WORKING_SET_MIB=${GC_WORKING_SET_MIB:-128}
GC_MARKER_MIB=${GC_MARKER_MIB:-4}

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
physical_mib=$((zone_sectors * nr_zones / 2048))
gc_marker_offset_mib=$((GC_WORKING_SET_MIB + 16))
gc_total_mib=${GC_TOTAL_MIB:-$((physical_mib + physical_mib / 2))}

[ "$zone_sectors" -gt 0 ] || fail "$UNDERLYING is not a zoned device"
[ "$nr_zones" -ge 2 ] || fail "rollover test requires at least two zones"
[ $((zone_sectors % SECTORS_PER_BLOCK)) -eq 0 ] ||
	fail "zone capacity is not aligned to 4 KiB blocks"
[ "$gc_total_mib" -gt "$physical_mib" ] ||
	fail "GC workload must exceed physical capacity"
[ $((gc_marker_offset_mib + GC_MARKER_MIB)) -lt "$physical_mib" ] ||
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

sectors=$(blockdev --getsz "$UNDERLYING")
echo "[*] dmsetup create $DM_NAME"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "dmsetup create failed"

block_count=$((zone_sectors / SECTORS_PER_BLOCK + 1))
total_bytes=$((block_count * BLOCK_SIZE))

echo
echo "=== [1/6] underlying zone geometry ==="
echo "zone sectors=$zone_sectors, zones=$nr_zones, rollover write=$total_bytes bytes"
echo "[OK]"

echo
echo "=== [2/6] write/read across the zone 0 -> zone 1 boundary ==="
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
echo "=== [3/6] overwrite maps an old zone 0 block into zone 1 ==="
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
echo "=== [4/6] underlying write pointers show rollover and overwrite ==="
zone0_wptr_hex=$(zone_wptr 0)
zone1_wptr_hex=$(zone_wptr "$zone_sectors")

[ -n "$zone0_wptr_hex" ] || fail "could not parse zone 0 write pointer"
[ -n "$zone1_wptr_hex" ] || fail "could not parse zone 1 write pointer"

zone0_wptr=$((zone0_wptr_hex))
zone1_wptr=$((zone1_wptr_hex))
expected_zone0_wptr=$zone_sectors
# blkzone report prints the write pointer relative to each zone start.
expected_zone1_wptr=$((SECTORS_PER_BLOCK * 2))

echo "zone 0 wp=$zone0_wptr, expected=$expected_zone0_wptr"
echo "zone 1 wp=$zone1_wptr, expected=$expected_zone1_wptr"

[ "$zone0_wptr" -eq "$expected_zone0_wptr" ] ||
	fail "zone 0 is not full after rollover write"
[ "$zone1_wptr" -eq "$expected_zone1_wptr" ] ||
	fail "zone 1 did not receive rollover and overwrite blocks"
echo "[OK]"

echo
echo "=== [5/6] GC reclaims space during capacity-exceeding random overwrite ==="
dd if=/dev/urandom of="$TMP_DIR/gc-marker.bin" bs=1M \
	count="$GC_MARKER_MIB" status=none
dd if="$TMP_DIR/gc-marker.bin" of="$DM_DEV" bs=1M \
	seek="$gc_marker_offset_mib" conv=notrunc oflag=direct status=none ||
	fail "GC marker write failed"

fio --name=gc-overwrite --filename="$DM_DEV" --rw=randwrite \
	--bs=4k --size="${GC_WORKING_SET_MIB}M" --io_size="${gc_total_mib}M" \
	--ioengine=libaio --iodepth=32 --direct=1 --group_reporting ||
	fail "capacity-exceeding random overwrite failed"
echo "logical overwrite=${gc_total_mib}MiB, physical capacity=${physical_mib}MiB"
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
