#!/usr/bin/env bash
# M2 smoke test: ext4 round-trip data integrity check.
#
# Success criterion: md5sum of a file matches before and after umount/remount.
# The umount/remount cycle flushes the page cache, forcing reads to go through
# the DM mapping layer instead of being served from RAM.
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-base).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
MNT=/mnt/zns-m2

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	umount "$MNT" 2>/dev/null || true
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod $MOD_NAME 2>/dev/null || true
}
trap cleanup EXIT

[ -b "$UNDERLYING" ] || {
	echo "[!] $UNDERLYING is missing. Run scripts/nullblk-up.sh first." >&2
	exit 1
}

# 빌드
if [ ! -f "$KO_PATH" ]; then
	echo "[*] Building module"
	make -C "$SRC_DIR" >/dev/null || { echo "[!] Build failed" >&2; exit 1; }
fi

# 초기화
dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod $MOD_NAME 2>/dev/null || true
blkzone reset "$UNDERLYING"

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" || { echo "[!] insmod failed" >&2; exit 1; }

sectors=$(blockdev --getsz "$UNDERLYING")
echo "[*] dmsetup create $DM_NAME"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}

mkdir -p "$MNT"

# ── [1/4] mkfs.ext4 + mount ───────────────────────────────────────────────────
echo
echo "=== [1/4] mkfs.ext4 + mount ==="
mkfs.ext4 -F "$DM_DEV" >/dev/null 2>&1 || {
	echo "[FAIL] mkfs.ext4 failed — DM device not conventional?" >&2; exit 2;
}
mount "$DM_DEV" "$MNT" || { echo "[FAIL] mount failed" >&2; exit 2; }
echo "[OK]"

# ── [2/4] 랜덤 데이터 쓰기 + hash A ──────────────────────────────────────────
echo
echo "=== [2/4] 10MB 랜덤 데이터 쓰기 ==="
dd if=/dev/urandom of="$MNT/data" bs=1M count=10 2>/dev/null
HASH_A=$(md5sum "$MNT/data" | awk '{print $1}')
echo "    hash A = $HASH_A"
echo "[OK]"

# ── [3/4] umount (페이지 캐시 비우기) + remount ───────────────────────────────
echo
echo "=== [3/4] sync + umount + remount ==="
sync
umount "$MNT"
mount "$DM_DEV" "$MNT" || { echo "[FAIL] remount failed" >&2; exit 2; }
echo "[OK]"

# ── [4/4] hash B 비교 ─────────────────────────────────────────────────────────
echo
echo "=== [4/4] md5 비교 ==="
HASH_B=$(md5sum "$MNT/data" | awk '{print $1}')
echo "    hash A = $HASH_A"
echo "    hash B = $HASH_B"

if [ "$HASH_A" = "$HASH_B" ]; then
	echo "[OK] hash 일치 — 읽기 정확성 검증 통과"
else
	echo "[FAIL] hash 불일치 — 매핑 테이블 읽기 경로에 버그 있음" >&2
	exit 3
fi

echo
echo "=== ALL M2 CHECKS PASSED ==="
