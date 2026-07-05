#!/usr/bin/env bash
# M1 smoke test: verify random writes are translated into sequential writes.
#
# Visual proof:
#   - blkzone report on /dev/nullb0 BEFORE fio: all zones wp=0x0
#   - blkzone report on /dev/nullb0 AFTER  fio: zone(s) wp advanced sequentially
#   - dmsetup status shows per-zone write pointer table
#
# Success criterion: fio 4K randwrite 100MB → blk_update_request delta=0
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-base).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod $MOD_NAME 2>/dev/null || true
}
trap cleanup EXIT

[ -b "$UNDERLYING" ] || {
	echo "[!] $UNDERLYING is missing. Run scripts/nullblk-up.sh first." >&2
	exit 1
}

if [ ! -f "$KO_PATH" ]; then
	echo "[*] Building module"
	make -C "$SRC_DIR" >/dev/null || { echo "[!] Build failed" >&2; exit 1; }
fi

dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod $MOD_NAME 2>/dev/null || true

# Reset all zones for a clean starting point
blkzone reset "$UNDERLYING"

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" || { echo "[!] insmod failed" >&2; exit 1; }

sectors=$(blockdev --getsz "$UNDERLYING")
echo "[*] dmsetup create $DM_NAME (zns-base on $UNDERLYING, $sectors sectors)"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}

# ── [1] conventional 노출 확인 ────────────────────────────────────────────────
echo
echo "=== [1/5] DM device: conventional (not zoned)? ==="
zoned=$(cat /sys/block/$(basename "$(readlink -f "$DM_DEV")")/queue/zoned 2>/dev/null || echo "none")
echo "    /dev/mapper/$DM_NAME  queue/zoned = $zoned"
if [ "$zoned" = "none" ] || [ "$zoned" = "" ]; then
	echo "[OK] conventional (zoned=none) — ext4 가능"
else
	echo "[WARN] zoned='$zoned' (expected 'none')"
fi

# ── [2] 쓰기 전 underlying zone 상태 ─────────────────────────────────────────
echo
echo "=== [2/5] /dev/nullb0 zone write pointers BEFORE fio ==="
echo "    (모든 wp = 0x0 이어야 함 — 아무것도 안 썼으므로)"
blkzone report "$UNDERLYING" | head -6
echo "    ..."

# ── [3] fio randwrite ─────────────────────────────────────────────────────────
echo
echo "=== [3/5] fio 4K randwrite 100MB (iodepth=32, direct=1) ==="
before=$(dmesg 2>/dev/null | grep -c 'blk_update_request.*I/O error' || true)

fio --name=m1-randw \
    --filename="$DM_DEV" \
    --rw=randwrite \
    --bs=4k \
    --size=100M \
    --ioengine=libaio \
    --iodepth=32 \
    --direct=1 \
    --output-format=normal \
    2>&1 | grep -E 'WRITE|iops|err'

after=$(dmesg 2>/dev/null | grep -c 'blk_update_request.*I/O error' || true)

# ── [4] 쓰기 후 underlying zone 상태 ─────────────────────────────────────────
echo
echo "=== [4/5] /dev/nullb0 zone write pointers AFTER fio ==="
echo "    (랜덤 LBA가 들어왔지만 zone 0부터 wp가 순차 전진해야 함)"
blkzone report "$UNDERLYING" | head -6
echo "    ..."

echo
echo "--- dmsetup status (per-zone wp in our mapping table) ---"
dmsetup status "$DM_NAME"

# ── [5] 에러 카운트 확인 ──────────────────────────────────────────────────────
echo
echo "=== [5/5] I/O error count delta ==="
delta=$((after - before))
echo "    blk_update_request errors before : $before"
echo "    blk_update_request errors after  : $after"
echo "    delta                            : $delta"

if [ "$delta" -eq 0 ]; then
	echo "[OK] delta = 0 — SEQ_WRITE_REQUIRED 위반 없음"
else
	echo "[FAIL] delta = $delta — underlying이 쓰기를 거절함" >&2
	dmesg | grep 'blk_update_request' | tail -5 >&2
	exit 3
fi

echo
echo "=== ALL M1 CHECKS PASSED ==="
