#!/usr/bin/env bash
# Forces WAL exhaustion, verifies checkpoint/WAL reclaim, then reload recovery.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-checkpoint-$$}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
METADATA_ZONES=6
GC_RESERVE_ZONES=2
# Three WAL capacities force manifest A -> B -> A reuse.
WRITE_MIB=${WRITE_MIB:-396}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
TMP_DIR=$(mktemp -d /tmp/dm-zns-checkpoint.XXXXXX)

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }
fail() { echo "[FAIL] $*" >&2; exit 1; }

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

[ -b "$UNDERLYING" ] || fail "$UNDERLYING is missing. Run scripts/nullblk-up.sh first."

underlying_name=$(basename "$(readlink -f "$UNDERLYING")")
queue_dir="/sys/block/$underlying_name/queue"
zone_sectors=$(cat "$queue_dir/chunk_sectors") || fail "cannot read zone size"
nr_zones=$(cat "$queue_dir/nr_zones") || fail "cannot read zone count"
sectors=$(((nr_zones - METADATA_ZONES - GC_RESERVE_ZONES) * zone_sectors))

echo "[*] Building module"
make -B -C "$SRC_DIR" >/dev/null || fail "build failed"
remove_target 2>/dev/null || true
rmmod "$MOD_NAME" 2>/dev/null || true
blkzone reset -o 0 -c "$nr_zones" "$UNDERLYING" || fail "initial reset failed"

echo "[*] Writing ${WRITE_MIB} MiB to force a WAL checkpoint"
insmod "$KO_PATH" || fail "insmod failed"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "dmsetup create failed"

dd if=/dev/urandom of="$TMP_DIR/payload" bs=1M count="$WRITE_MIB" status=none
dd if="$TMP_DIR/payload" of="$DM_DEV" bs=1M conv=fsync oflag=direct status=none ||
	fail "write failed before checkpoint"

echo "[*] Verifying manifest A/B rotation"
manifest_a_wp=$(zone_wptr 0)
manifest_b_wp=$(zone_wptr "$zone_sectors")
[ -n "$manifest_a_wp" ] || fail "could not parse manifest A write pointer"
[ -n "$manifest_b_wp" ] || fail "could not parse manifest B write pointer"
[ $((manifest_a_wp)) -gt 0 ] || fail "manifest A was not written"
[ $((manifest_b_wp)) -gt 0 ] || fail "manifest B was not written"
echo "manifest A wp=$((manifest_a_wp)), manifest B wp=$((manifest_b_wp))"

remove_target || fail "first dmsetup remove failed"
rmmod "$MOD_NAME" || fail "first rmmod failed"

echo "[*] Recreating target without resetting $UNDERLYING"
insmod "$KO_PATH" || fail "second insmod failed"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	{ dmesg -T | tail -n 80 >&2; fail "recovery dmsetup create failed"; }

dd if="$DM_DEV" of="$TMP_DIR/recovered" bs=1M count="$WRITE_MIB" iflag=direct status=none ||
	fail "recovery read failed"
cmp "$TMP_DIR/payload" "$TMP_DIR/recovered" || fail "checkpoint recovery readback mismatch"

echo "[OK] manifest A/B rotation and checkpoint recovery recovered ${WRITE_MIB} MiB after module reload"
