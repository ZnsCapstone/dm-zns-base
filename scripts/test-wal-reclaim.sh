#!/usr/bin/env bash
# WAL zone 회수 + 회수 후 크래시 복구 검증 (implementation plan 13단계 확인용).
#
# 13단계 전까지 WAL zone은 아무도 회수하지 않아 단조 증가했다. 이제 체크포인트가
# durable해지면 온전히 flush된 WAL zone을 reset한다. 이 테스트의 핵심은 회수
# 자체가 아니라 **회수 후에도 크래시 복구가 정확한가**다 — WAL zone을 회수하면
# zone_id 순서 ≠ 기록 순서가 되어, replay가 generation 순서로 정확히 재생해야만
# 데이터가 복원된다(그래서 13단계에서 zone 헤더에 generation을 넣었다).
#
# 시나리오:
#   1) flush_threshold를 낮게 잡아 flush/체크포인트가 자주 일어나게 한다.
#   2) WAL zone 하나(≈512MB, 131072 레코드)를 넘겨 쓰도록 넉넉히 distinct
#      오프셋에 기록한다 — 그래야 첫 WAL zone이 회수 대상이 된다. GC와 섞이지
#      않도록 겹쳐쓰기 없이 device 앞쪽 영역에만 쓴다(free zone 여유 유지).
#   3) "wal reclaim: reclaimed zone" 로그로 실제 회수를 확인한다.
#   4) zone reset 없이 모듈만 내렸다 다시 올려(크래시 시뮬레이션) 같은 영역을
#      다시 읽어 hash가 일치하는지 본다 — 회수로 뒤섞인 WAL을 generation 순서로
#      제대로 재생했는지 검증.
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-base),
#      FLUSH_THRESHOLD (default 8000 — 낮게 잡아 flush 자주),
#      WRITE_MB (default 768 — WAL zone 하나(≈512MB)를 확실히 넘기려고).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
FLUSH_THRESHOLD=${FLUSH_THRESHOLD:-8000}
WRITE_MB=${WRITE_MB:-768}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
DATA_FILE=/tmp/wal-reclaim-test-data

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

echo "=== [1/5] insmod (flush_threshold=$FLUSH_THRESHOLD, batching off for rollover coverage) + ${WRITE_MB}MB 기록 ==="
insmod "$KO_PATH" flush_threshold="$FLUSH_THRESHOLD" wal_batch_max_records=1 || { echo "[!] insmod failed" >&2; exit 1; }
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}

# 결정적 데이터를 device 앞쪽 WRITE_MB 영역에 통째로 기록(겹쳐쓰기 없음).
dd if=/dev/urandom of="$DATA_FILE" bs=1M count="$WRITE_MB" 2>/dev/null
dd if="$DATA_FILE" of="$DM_DEV" bs=1M count="$WRITE_MB" oflag=direct 2>/dev/null || {
	echo "[FAIL] 데이터 기록 중 실패" >&2; exit 3;
}
sync
sleep 3   # 진행 중인 flush/체크포인트/WAL 회수가 정리될 시간
HASH_A=$(dd if="$DM_DEV" bs=1M count="$WRITE_MB" iflag=direct 2>/dev/null | md5sum | awk '{print $1}')
echo "    hash A = $HASH_A"
echo "[OK]"

echo
echo "=== [2/5] WAL zone 회수 로그 확인 ==="
RECLAIM_COUNT=$(dmesg | grep -c "wal reclaim: reclaimed zone" || true)
echo "    wal reclaim: reclaimed zone 로그: ${RECLAIM_COUNT}건"
if [ "$RECLAIM_COUNT" -lt 1 ]; then
	echo "[FAIL] WAL zone이 한 번도 회수되지 않음 — WRITE_MB를 늘리거나 FLUSH_THRESHOLD를 낮춰보세요" >&2
	dmesg | grep -E "checkpoint written|wal reclaim" | tail -10
	exit 3
fi
dmesg | grep "wal reclaim" | tail -5
echo "[OK]"

echo
echo "=== [3/5] '크래시' 시뮬레이션 — zone reset 없이 모듈만 내렸다 다시 올림 ==="
dmsetup remove "$DM_NAME"
rmmod $MOD_NAME
insmod "$KO_PATH" flush_threshold="$FLUSH_THRESHOLD" wal_batch_max_records=1 || { echo "[!] insmod(재적재) failed" >&2; exit 1; }
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create(재생성) failed" >&2; exit 1;
}
echo "[OK] 재적재 완료 (generation 순서 replay + SSTable 스캔은 ctr()에서 이미 끝남)"

echo
echo "=== [4/5] 재적재 후 같은 영역 다시 읽어 hash 비교 ==="
HASH_B=$(dd if="$DM_DEV" bs=1M count="$WRITE_MB" iflag=direct 2>/dev/null | md5sum | awk '{print $1}')
echo "    hash A = $HASH_A"
echo "    hash B = $HASH_B"
if [ "$HASH_A" = "$HASH_B" ]; then
	echo "[OK] hash 일치 — WAL zone 회수로 순서가 뒤섞인 뒤에도 복구 정확함"
else
	echo "[FAIL] hash 불일치 — 회수 후 generation 순서 replay가 매핑을 복원 못함" >&2
	exit 3
fi

echo
echo "=== [5/5] 커널 에러 확인 ==="
if dmesg | grep -qi "blk_update_request\|hung_task\|BUG:\|reclaimed while dispatch waiters"; then
	echo "[FAIL] 커널 에러/행/회수 가드 위반 감지됨" >&2
	dmesg | grep -i "blk_update_request\|hung_task\|BUG:\|reclaimed while dispatch waiters"
	exit 3
fi
echo "[OK] 에러 없음"

echo
echo "=== ALL WAL-RECLAIM CHECKS PASSED ==="
