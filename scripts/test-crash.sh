#!/usr/bin/env bash
# Crash-recovery smoke test (implementation plan 5단계 확인용).
#
# 정상적인 dtr() 경로를 거치더라도, memtable/zone_pool의 태그·wp는 메모리에만
# 있어서 재적재하면 사라진다. zone reset 없이 모듈만 내렸다 다시 올려서,
# WAL replay + zone 헤더 스캔이 실제로 매핑을 복원하는지 확인한다.
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
DATA_FILE=/tmp/crash-test-data

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod $MOD_NAME 2>/dev/null || true
	rm -f "$DATA_FILE"
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

# 초기화 — zone reset은 여기서 딱 한 번만. 이후 재insmod 때는 절대 안 함
# (그래야 "크래시 후에도 물리 데이터/하드웨어 wp는 남아있다"는 상황이 재현됨).
dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod $MOD_NAME 2>/dev/null || true
blkzone reset "$UNDERLYING"

sectors=$(blockdev --getsz "$UNDERLYING")

echo "=== [1/4] 첫 insmod + 쓰기 ==="
insmod "$KO_PATH" || { echo "[!] insmod failed" >&2; exit 1; }
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}

dd if=/dev/urandom of="$DATA_FILE" bs=4K count=256 2>/dev/null
dd if="$DATA_FILE" of="$DM_DEV" bs=4K count=256 oflag=direct 2>/dev/null
sync
HASH_A=$(dd if="$DM_DEV" bs=4K count=256 iflag=direct 2>/dev/null | md5sum | awk '{print $1}')
echo "    hash A = $HASH_A"
echo "[OK]"

echo
echo "=== [2/4] '크래시' 시뮬레이션 — zone reset 없이 모듈만 내렸다 다시 올림 ==="
dmsetup remove "$DM_NAME"
rmmod $MOD_NAME
insmod "$KO_PATH" || { echo "[!] insmod(재적재) failed" >&2; exit 1; }
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create(재생성) failed" >&2; exit 1;
}
echo "[OK] 재적재 완료 (WAL replay는 ctr() 안에서 이미 끝났음)"

echo
echo "=== [3/4] 재적재 후 같은 위치 다시 읽기 ==="
HASH_B=$(dd if="$DM_DEV" bs=4K count=256 iflag=direct 2>/dev/null | md5sum | awk '{print $1}')
echo "    hash B = $HASH_B"
echo "[OK]"

echo
echo "=== [4/4] hash 비교 ==="
echo "    hash A = $HASH_A"
echo "    hash B = $HASH_B"
if [ "$HASH_A" = "$HASH_B" ]; then
	echo "[OK] hash 일치 — WAL replay로 매핑이 정확히 복원됨"
else
	echo "[FAIL] hash 불일치 — WAL replay가 매핑을 복원 못함" >&2
	exit 3
fi

echo
echo "=== ALL CRASH-RECOVERY CHECKS PASSED ==="
