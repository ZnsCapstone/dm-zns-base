#!/usr/bin/env bash
# WAL checkpoint verification (implementation plan 7단계 확인용).
#
# flush_threshold를 낮춰서 SSTable flush + WAL checkpoint가 실제로 여러 번
# 일어나게 만든 뒤, 크래시 시뮬레이션(zone reset 없이 재insmod)을 하고
# dmesg에서 "마지막 체크포인트를 찾아 그 이전 WAL은 건너뛰었는지"를 확인한다.
#
# 주의: SSTable 읽기 경로(8단계)가 아직 없어서, flush된 세대의 LBA는 재부팅
# 후 못 읽는다 — 이 스크립트는 읽기 정확성이 아니라 체크포인트 메커니즘
# 자체(로그)만 확인한다. test.sh의 0~3 회귀 스위트와는 별개.
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-base),
#      FLUSH_THRESHOLD (default 1000), WRITE_COUNT (default 6000, 4K블록 수).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
FLUSH_THRESHOLD=${FLUSH_THRESHOLD:-1000}
WRITE_COUNT=${WRITE_COUNT:-6000}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
DATA_FILE=/tmp/checkpoint-test-data

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

echo "=== [1/4] insmod (flush_threshold=$FLUSH_THRESHOLD) + 여러 flush를 유발하는 쓰기 ==="
insmod "$KO_PATH" flush_threshold="$FLUSH_THRESHOLD" || { echo "[!] insmod failed" >&2; exit 1; }
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}

dd if=/dev/urandom of="$DATA_FILE" bs=4K count="$WRITE_COUNT" 2>/dev/null
dd if="$DATA_FILE" of="$DM_DEV" bs=4K count="$WRITE_COUNT" oflag=direct 2>/dev/null
sync
sleep 2   # flush/checkpoint 체인은 fire-and-forget이라 완료를 잠깐 기다림

CKPT_WRITTEN=$(dmesg | grep -c "checkpoint written" || true)
echo "    checkpoint written 로그: ${CKPT_WRITTEN}건"
if [ "$CKPT_WRITTEN" -lt 2 ]; then
	echo "[!] 체크포인트가 충분히 안 생겼습니다 (WRITE_COUNT/FLUSH_THRESHOLD를 조정해보세요)" >&2
	exit 3
fi
echo "[OK]"

echo
echo "=== [2/4] '크래시' 시뮬레이션 — zone reset 없이 모듈만 내렸다 다시 올림 ==="
dmsetup remove "$DM_NAME"
rmmod $MOD_NAME
dmesg -C
insmod "$KO_PATH" flush_threshold="$FLUSH_THRESHOLD" || { echo "[!] insmod(재적재) failed" >&2; exit 1; }
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create(재생성) failed" >&2; exit 1;
}
echo "[OK] 재적재 완료"

echo
echo "=== [3/4] dmesg에서 체크포인트 기반 replay 확인 ==="
dmesg | grep "WAL replay:" || true
echo

if dmesg | grep -q "WAL replay: last checkpoint seq="; then
	echo "[OK] 마지막 체크포인트를 찾아 그 이전 WAL은 건너뛰었습니다"
else
	echo "[FAIL] 체크포인트를 못 찾음 — 전체 재생으로 폴백된 것으로 보임" >&2
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
echo "=== ALL CHECKPOINT CHECKS PASSED ==="
