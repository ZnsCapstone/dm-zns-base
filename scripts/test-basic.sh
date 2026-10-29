#!/usr/bin/env bash
# 초기 M0의 zone 전달 경로를 확인하는 참고용 smoke 테스트다.
# 빌드·모듈 적재·DM 생성 뒤 zone report와 첫 zone reset을 확인한다.
# 4KiB 순차 쓰기 후 wp를 확인하고 정리한다.
#
# 환경변수: UNDERLYING (기본값 /dev/nullb0), DM_NAME (기본값 myzns-base).

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

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" || { echo "[!] insmod failed" >&2; exit 1; }

sectors=$(blockdev --getsz "$UNDERLYING")
echo "[*] dmsetup create $DM_NAME (zns-base on $UNDERLYING, $sectors sectors)"
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}

echo
echo "=== [1/4] blkzone report ==="
blkzone report "$DM_DEV" | head -n 3 || {
	echo "[FAIL] blkzone report failed — check .report_zones / .iterate_devices" >&2
	exit 2
}
echo "[OK]"

echo
echo "=== [2/4] zone 0 reset ==="
blkzone reset -o 0 -c 1 "$DM_DEV" && echo "[OK]"

echo
echo "=== [3/4] 4 KiB sequential write at sector 0 ==="
dd if=/dev/zero of="$DM_DEV" bs=4096 count=1 seek=0 oflag=direct 2>&1 | tail -n 2

echo
echo "=== [4/4] write pointer advanced ==="
wp_line=$(blkzone report "$DM_DEV" | head -n 1)
echo "$wp_line"
if echo "$wp_line" | grep -q 'wptr 0x000008'; then
	echo "[OK] wp = sector 8"
else
	echo "[FAIL] wp is not at the expected 0x8 — check bio remap" >&2
	exit 3
fi

echo
echo "=== ALL CHECKS PASSED ==="
