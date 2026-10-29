#!/usr/bin/env bash
# flush된 SSTable을 장치에서 읽어 최신 seq_no의 매핑이 선택되는지 확인한다.
# checkpoint 로그만 확인하는 테스트와 구분해 실제 읽기 정합성을 검사한다.
#
# 환경변수: UNDERLYING (기본값 /dev/nullb0), DM_NAME (기본값 myzns-base),
#      FLUSH_THRESHOLD (기본값 50), WRITE_COUNT (기본값 120, 4K블록 수).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
FLUSH_THRESHOLD=${FLUSH_THRESHOLD:-50}
WRITE_COUNT=${WRITE_COUNT:-120}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
DATA_A=/tmp/sstable-read-test-A
DATA_B=/tmp/sstable-read-test-B

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod $MOD_NAME 2>/dev/null || true
	rm -f "$DATA_A" "$DATA_B"
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
blkzone reset "$UNDERLYING"
dmesg -C

sectors=$(blockdev --getsz "$UNDERLYING")

echo "=== [1/4] insmod (flush_threshold=$FLUSH_THRESHOLD) ==="
insmod "$KO_PATH" flush_threshold="$FLUSH_THRESHOLD" || { echo "[!] insmod failed" >&2; exit 1; }
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}
echo "[OK]"

echo
echo "=== [2/4] 같은 LBA 범위를 두 번(A -> B) 덮어써서 여러 SSTable 세대에 걸치게 함 ==="
dd if=/dev/urandom of="$DATA_A" bs=4K count="$WRITE_COUNT" 2>/dev/null
dd if="$DATA_A" of="$DM_DEV" bs=4K count="$WRITE_COUNT" oflag=direct 2>/dev/null
sync
sleep 2   # 1차 flush들이 끝나길 기다림

dd if=/dev/urandom of="$DATA_B" bs=4K count="$WRITE_COUNT" 2>/dev/null
dd if="$DATA_B" of="$DM_DEV" bs=4K count="$WRITE_COUNT" oflag=direct 2>/dev/null
sync
sleep 2   # 2차 flush들이 끝나길 기다림

CKPT_WRITTEN=$(dmesg | grep -c "checkpoint written" || true)
echo "    checkpoint written 로그: ${CKPT_WRITTEN}건 (여러 SSTable 세대가 쌓였다는 뜻)"
if [ "$CKPT_WRITTEN" -lt 2 ]; then
	echo "[!] flush가 충분히 안 일어났습니다 (WRITE_COUNT/FLUSH_THRESHOLD를 조정해보세요)" >&2
	exit 3
fi
echo "[OK]"

echo
echo "=== [3/4] 페이지 캐시 우회해서 다시 읽고 최신값(B)과 비교 ==="
HASH_B_EXPECTED=$(md5sum "$DATA_B" | awk '{print $1}')
HASH_READ=$(dd if="$DM_DEV" bs=4K count="$WRITE_COUNT" iflag=direct 2>/dev/null | md5sum | awk '{print $1}')
echo "    expected (B) = $HASH_B_EXPECTED"
echo "    read back    = $HASH_READ"
if [ "$HASH_READ" = "$HASH_B_EXPECTED" ]; then
	echo "[OK] 최신 SSTable/memtable 값이 정확히 읽힘 — 오래된 세대(A)로 새지 않음"
else
	echo "[FAIL] 읽은 값이 최신 값(B)과 다름 — SSTable 조회의 seq_no 우선순위가 잘못됐을 수 있음" >&2
	exit 3
fi

echo
echo "=== [4/4] 커널 에러 확인 ==="
if dmesg | grep -qi "blk_update_request\|hung_task\|BUG:"; then
	echo "[FAIL] 커널 에러/행 감지됨" >&2
	dmesg | grep -i "blk_update_request\|hung_task\|BUG:"
	exit 3
fi
echo "[OK] 에러 없음"

echo
echo "=== ALL SSTABLE READ-PATH CHECKS PASSED ==="
