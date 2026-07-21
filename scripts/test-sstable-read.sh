#!/usr/bin/env bash
# SSTable read-path verification (implementation plan 8단계 확인용).
#
# flush_threshold를 낮춰 같은 LBA 범위를 두 번(서로 다른 데이터로) 덮어써서
# flush가 여러 번 일어나게 만든 뒤 — 즉 같은 lba가 여러 SSTable 세대에 걸쳐
# 있는 상태를 만든 뒤 — 다시 읽었을 때 가장 최근에 쓴 데이터가 나오는지
# 확인한다(오래된 SSTable/세대가 아니라 seq_no 최댓값이 이겨야 함).
#
# test-checkpoint.sh(7단계)와 달리 이번엔 실제로 디바이스를 통해 읽어서
# 정확성을 검증한다 — 8단계부터는 flush된 데이터도 읽을 수 있어야 하므로.
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-base),
#      FLUSH_THRESHOLD (default 50), WRITE_COUNT (default 120, 4K블록 수).

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
