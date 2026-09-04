#!/usr/bin/env bash
# Compaction verification (implementation plan 9단계 확인용).
#
# flush_threshold/compaction_k를 낮춰서 SSTable이 여러 개(K 이상) 쌓이며
# compaction이 자동으로(백그라운드 workqueue) 돌게 만든다. 전부 겹치지 않는
# 서로 다른 LBA 범위로 한 번에 쓰기 때문에(overwrite 없음), compaction이
# 데이터를 잃어버리지 않고 정확히 병합했는지 최종 재읽기 hash로 검증한다 —
# 겹치는 LBA에 대한 "최신값 우선" 자체는 test-sstable-read.sh가 이미 확인함.
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-base),
#      FLUSH_THRESHOLD (default 50), COMPACTION_K (default 4),
#      WRITE_COUNT (default 500, 4K블록 수 — FLUSH_THRESHOLD의 몇 배는 되어야
#      SSTable이 COMPACTION_K개 이상 쌓임).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
FLUSH_THRESHOLD=${FLUSH_THRESHOLD:-50}
COMPACTION_K=${COMPACTION_K:-4}
WRITE_COUNT=${WRITE_COUNT:-500}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
DATA_FILE=/tmp/compaction-test-data

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

dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod $MOD_NAME 2>/dev/null || true
blkzone reset "$UNDERLYING"
dmesg -C

sectors=$(blockdev --getsz "$UNDERLYING")

echo "=== [1/5] insmod (flush_threshold=$FLUSH_THRESHOLD, compaction_k=$COMPACTION_K) ==="
insmod "$KO_PATH" flush_threshold="$FLUSH_THRESHOLD" compaction_k="$COMPACTION_K" || {
	echo "[!] insmod failed" >&2; exit 1;
}
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}
echo "[OK]"

echo
echo "=== [2/5] 겹치지 않는 $WRITE_COUNT개 4K 블록 쓰기 (여러 flush → SSTable 여러 개 → compaction 트리거) ==="
dd if=/dev/urandom of="$DATA_FILE" bs=4K count="$WRITE_COUNT" 2>/dev/null
dd if="$DATA_FILE" of="$DM_DEV" bs=4K count="$WRITE_COUNT" oflag=direct 2>/dev/null
sync
sleep 3   # flush + compaction(비동기 workqueue) 완료를 기다림

FLUSH_COUNT=$(dmesg | grep -c "checkpoint written" || true)
COMPACT_COUNT=$(dmesg | grep -c "compaction: merged" || true)
echo "    flush(checkpoint written) 로그: ${FLUSH_COUNT}건"
echo "    compaction 로그: ${COMPACT_COUNT}건"
if [ "$COMPACT_COUNT" -lt 1 ]; then
	echo "[!] compaction이 한 번도 안 돌았습니다 (WRITE_COUNT/FLUSH_THRESHOLD/COMPACTION_K를 조정해보세요)" >&2
	exit 3
fi
# Compaction은 새 논리 쓰기가 아니므로 결과 seq가 병합 대상의
# 최대 seq보다 커지면 안 된다. 그러면 대상 밖의 더 최신
# SSTable을 예전 데이터가 덮어쓰는 세대 역전이 생긴다.
SEQ_VIOLATIONS=$(dmesg |
	sed -nE 's/.*seq [0-9]+\.\.([0-9]+)\) into seq=([0-9]+).*/\1 \2/p' |
	awk '$2 > $1 { bad++ } END { print bad + 0 }')
if [ "$SEQ_VIOLATIONS" -ne 0 ]; then
	echo "[FAIL] compaction 결과 seq가 victim 최대 seq보다 큼 — 최신 mapping 역전 가능" >&2
	exit 3
fi
echo "    compaction sequence 역전: 0건"
echo "[OK]"

echo
echo "=== [3/5] compaction 로그 내용 ==="
dmesg | grep "compaction:" || true
echo "[OK]"

echo
echo "=== [4/5] 페이지 캐시 우회해서 다시 읽고 원본과 비교 ==="
HASH_EXPECTED=$(md5sum "$DATA_FILE" | awk '{print $1}')
HASH_READ=$(dd if="$DM_DEV" bs=4K count="$WRITE_COUNT" iflag=direct 2>/dev/null | md5sum | awk '{print $1}')
echo "    expected = $HASH_EXPECTED"
echo "    read back = $HASH_READ"
if [ "$HASH_READ" = "$HASH_EXPECTED" ]; then
	echo "[OK] compaction 전후로 데이터가 정확히 보존됨"
else
	echo "[FAIL] 읽은 값이 원본과 다름 — compaction이 데이터를 잃어버렸을 수 있음" >&2
	exit 3
fi

echo
echo "=== [5/5] 커널 에러 확인 ==="
if dmesg | grep -qi "blk_update_request\|hung_task\|BUG:"; then
	echo "[FAIL] 커널 에러/행 감지됨" >&2
	dmesg | grep -i "blk_update_request\|hung_task\|BUG:"
	exit 3
fi
echo "[OK] 에러 없음"

echo
echo "=== ALL COMPACTION CHECKS PASSED ==="
