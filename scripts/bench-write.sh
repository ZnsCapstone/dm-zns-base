#!/usr/bin/env bash
# Clean-device foreground write benchmark for zns-base.
# WARNING: resets every zone on UNDERLYING before the run.

set -euo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-write-bench}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base

RW=${RW:-randwrite}
BS=${BS:-4k}
IODEPTH=${IODEPTH:-32}
NUMJOBS=${NUMJOBS:-1}
BENCH_MIB=${BENCH_MIB:-1024}
IO_MIB=${IO_MIB:-$BENCH_MIB}
FSYNC_BLOCKS=${FSYNC_BLOCKS:-0}
CONFIRM_RESET=${CONFIRM_RESET:-NO}

METADATA_ZONES=6
GC_RESERVE_ZONES=2

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
SRC_DIR="$REPO_DIR/src"
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR=${RESULT_DIR:-$REPO_DIR/results/write_${RW}_${BS}_qd${IODEPTH}_$TIMESTAMP}

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
		dmsetup remove --retry "$DM_NAME" 2>/dev/null || true
	done
	return 1
}

cleanup() {
	remove_target 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
}
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || fail "run this benchmark with sudo"
[ -b "$UNDERLYING" ] || fail "$UNDERLYING is not a block device"
command -v fio >/dev/null || fail "fio is not installed"
command -v blkzone >/dev/null || fail "blkzone is not installed"

case "$RW" in
write|randwrite) ;;
*) fail "RW must be write or randwrite" ;;
esac
case "$BENCH_MIB:$IO_MIB:$IODEPTH:$NUMJOBS:$FSYNC_BLOCKS" in
*[!0-9:]*) fail "size, depth, job, and fsync values must be integers" ;;
esac
[ "$BENCH_MIB" -gt 0 ] || fail "BENCH_MIB must be positive"
[ "$IO_MIB" -gt 0 ] || fail "IO_MIB must be positive"
[ "$IODEPTH" -gt 0 ] || fail "IODEPTH must be positive"
[ "$NUMJOBS" -gt 0 ] || fail "NUMJOBS must be positive"

underlying_name=$(basename "$(readlink -f "$UNDERLYING")")
queue_dir="/sys/block/$underlying_name/queue"
zone_sectors=$(cat "$queue_dir/chunk_sectors" 2>/dev/null) ||
	fail "cannot read zone size for $UNDERLYING"
nr_zones=$(cat "$queue_dir/nr_zones" 2>/dev/null) ||
	fail "cannot read zone count for $UNDERLYING"
[ "$zone_sectors" -gt 0 ] || fail "$UNDERLYING is not zoned"
[ "$nr_zones" -gt $((METADATA_ZONES + GC_RESERVE_ZONES)) ] ||
	fail "not enough zones for metadata, data, and GC reserve"

logical_data_zones=$((nr_zones - METADATA_ZONES - GC_RESERVE_ZONES))
target_sectors=$((logical_data_zones * zone_sectors))
bench_sectors=$((BENCH_MIB * 2048))
[ "$bench_sectors" -le "$target_sectors" ] ||
	fail "BENCH_MIB exceeds the exposed target capacity"

if [ "$UNDERLYING" != "/dev/nullb0" ] && [ "$CONFIRM_RESET" != "YES" ]; then
	fail "this erases $UNDERLYING; rerun with CONFIRM_RESET=YES"
fi

mkdir -p "$RESULT_DIR"

echo "[*] Building module"
make -C "$SRC_DIR" >/dev/null || fail "build failed"

remove_target 2>/dev/null || true
if lsmod | grep -q '^dm_zns_base '; then
	rmmod "$MOD_NAME" 2>/dev/null ||
		fail "$MOD_NAME is loaded and in use"
fi

echo "[*] Resetting $UNDERLYING ($nr_zones zones)"
blkzone reset -o 0 -c "$nr_zones" "$UNDERLYING" ||
	fail "underlying reset failed"

echo "[*] Loading zns-base with full hardware-zone capacity"
insmod "$KO_PATH" data_zone_capacity_mib=0 || fail "insmod failed"
echo "0 $target_sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" ||
	fail "dmsetup create failed"

dmsetup status "$DM_NAME" > "$RESULT_DIR/status-before.txt"
blkzone report "$UNDERLYING" > "$RESULT_DIR/zones-before.txt"

cat > "$RESULT_DIR/config.txt" <<EOF
underlying=$UNDERLYING
hardware_zone_mib=$((zone_sectors / 2048))
nr_zones=$nr_zones
target_mib=$((target_sectors / 2048))
rw=$RW
bs=$BS
iodepth=$IODEPTH
numjobs=$NUMJOBS
working_set_mib=$BENCH_MIB
io_mib=$IO_MIB
fsync_blocks=$FSYNC_BLOCKS
EOF

fio_args=(
	--name="zns-base-$RW"
	--filename="$DM_DEV"
	--rw="$RW"
	--bs="$BS"
	--size="${BENCH_MIB}M"
	--io_size="${IO_MIB}M"
	--ioengine=libaio
	--iodepth="$IODEPTH"
	--numjobs="$NUMJOBS"
	--direct=1
	--randrepeat=1
	--group_reporting
	--end_fsync=1
	--percentile_list=50:90:95:99:99.9
)
if [ "$FSYNC_BLOCKS" -gt 0 ]; then
	fio_args+=(--fsync="$FSYNC_BLOCKS")
fi

echo "[*] Running: rw=$RW bs=$BS qd=$IODEPTH jobs=$NUMJOBS io=${IO_MIB}MiB"
set +e
fio "${fio_args[@]}" 2>&1 | tee "$RESULT_DIR/fio.txt"
fio_ret=${PIPESTATUS[0]}
set -e

dmsetup status "$DM_NAME" > "$RESULT_DIR/status-after.txt" || true
blkzone report "$UNDERLYING" > "$RESULT_DIR/zones-after.txt" || true

[ "$fio_ret" -eq 0 ] || fail "fio failed; see $RESULT_DIR/fio.txt"

echo
echo "[OK] Write benchmark completed"
echo "results=$RESULT_DIR"
echo "mode note: FSYNC_BLOCKS=0 measures writeback throughput with one final durability flush"
