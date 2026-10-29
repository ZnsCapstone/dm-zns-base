#!/usr/bin/env bash
# GC 이주 후 모듈을 재적재해 WAL이 새 물리 위치를 복원하는지 확인한다.
# 큰 flush_threshold로 이주 매핑이 WAL에 의존하는 상황을 유지한다.
# 영역을 먼저 채우고 랜덤 overwrite로 무효 블록을 만든 뒤 실제 이주 발생을 로그로 확인한다.
# zone reset 없이 재적재한 후 전체 영역의 데이터 유실 여부를 검사한다.
# 정상 모듈 제거를 거치므로 전원 차단이나 실제 커널 크래시 시험으로 해석하지 않는다.
#
# 환경변수: UNDERLYING (기본값 /dev/nullb0), DM_NAME (기본값 myzns-base),
#      FILL_MB (기본값 512 — 너무 크면 flush 없이 WAL이 안 줄어 ENOSPC),
#      OVERWRITE_MB (기본값 256 — A 안에서 덮어쓸 총량, 부분-죽은 zone 생성),
#      GC_LOW_WATERMARK (기본값 20 — 높게 잡아 GC가 일찍/자주 돌게).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
FILL_MB=${FILL_MB:-512}
OVERWRITE_MB=${OVERWRITE_MB:-256}
GC_LOW_WATERMARK=${GC_LOW_WATERMARK:-20}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
DATA_FILE=/tmp/gc-crash-test-data

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

[ -f "$KO_PATH" ] || make -C "$SRC_DIR" >/dev/null || { echo "[!] Build failed" >&2; exit 1; }

dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod $MOD_NAME 2>/dev/null || true
blkzone reset "$UNDERLYING"
dmesg -C

sectors=$(blockdev --getsz "$UNDERLYING")
fill_bytes=$((FILL_MB * 1024 * 1024))

echo "=== [1/6] insmod (flush_threshold=기본값, gc_low_watermark=$GC_LOW_WATERMARK) + 영역 A 순차 채우기 (${FILL_MB}MB) ==="
insmod "$KO_PATH" gc_low_watermark="$GC_LOW_WATERMARK" || { echo "[!] insmod failed" >&2; exit 1; }
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || { echo "[!] dmsetup create failed" >&2; exit 1; }

dd if=/dev/urandom of="$DATA_FILE" bs=1M count="$FILL_MB" 2>/dev/null
dd if="$DATA_FILE" of="$DM_DEV" bs=1M count="$FILL_MB" oflag=direct 2>/dev/null || {
	echo "[FAIL] 영역 A 채우기 실패" >&2; exit 3;
}
sync
echo "[OK] A 전체가 non-zero로 채워짐"

echo
echo "=== [2/6] A 안에서 random overwrite (${OVERWRITE_MB}MB) — 부분-죽은 zone 생성 → GC 유발 ==="
fio --name=gc-crash-ow \
	--filename="$DM_DEV" \
	--rw=randwrite --bs=4k \
	--size=$fill_bytes \
	--io_size=$((OVERWRITE_MB * 1024 * 1024)) \
	--ioengine=libaio --iodepth=32 --direct=1 \
	2>&1 | grep -E 'WRITE|err' || true
sync
for _ in $(seq 1 60); do
	dmesg | grep -q "gc: reclaimed zone" && break
	sleep 1
done
echo "[OK]"

echo
echo "=== [3/6] GC가 '살아있는 데이터'를 실제로 재배치했는지 확인 (M>0) ==="
RELOC=$(dmesg | grep -oE "reclaimed zone [0-9]+ \([0-9]+ live entries" | grep -oE "\([0-9]+" | tr -d '(' | awk '{s+=$1} END{print s+0}')
echo "    총 재배치된 live 엔트리 수: ${RELOC}"
dmesg | grep "gc: reclaimed zone" | tail -5
if [ "${RELOC:-0}" -lt 1 ]; then
	echo "[FAIL] GC가 살아있는 데이터를 한 번도 재배치 안 함 — 이 테스트가 취약 경로를 못 탐." >&2
	echo "       FILL_MB/OVERWRITE_MB/GC_LOW_WATERMARK를 조정해보세요(부분-죽은 zone이 생기도록)." >&2
	exit 3
fi
echo "[OK] 살아있는 블록 재배치 발생 — 취약 경로 진입 확인"

echo
echo "=== [4/6] '크래시' — zone reset 없이 모듈만 내렸다 다시 올림 ==="
dmsetup remove "$DM_NAME"
rmmod $MOD_NAME
insmod "$KO_PATH" gc_low_watermark="$GC_LOW_WATERMARK" || { echo "[!] insmod(재적재) failed" >&2; exit 1; }
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || { echo "[!] dmsetup create(재생성) failed" >&2; exit 1; }
echo "[OK] 재적재 완료 (WAL replay가 재배치 매핑까지 복원했어야 함)"

echo
echo "=== [5/6] A 전체를 읽어 all-zero 4K 블록(=유실) 스캔 ==="
ZERO_BLOCKS=$(dd if="$DM_DEV" bs=1M count="$FILL_MB" iflag=direct 2>/dev/null | python3 -c '
import sys
data = sys.stdin.buffer.read()
z = bytes(4096)
n = len(data)//4096
print(sum(1 for i in range(n) if data[i*4096:(i+1)*4096] == z))
')
echo "    all-zero 4K 블록 수: ${ZERO_BLOCKS}"
if [ "${ZERO_BLOCKS:-1}" -ne 0 ]; then
	echo "[FAIL] all-zero 블록 발견 — GC 재배치 매핑이 크래시로 유실됨(A번 버그 재현)" >&2
	exit 3
fi
echo "[OK] 유실 없음 — GC 재배치가 크래시에 안전함"

echo
echo "=== [6/6] 커널 에러 확인 ==="
if dmesg | grep -qi "blk_update_request\|hung_task\|BUG:\|reclaimed while dispatch waiters\|WAL scan: invalid/torn page"; then
	echo "[FAIL] 커널 에러/행 감지됨" >&2
	dmesg | grep -i "blk_update_request\|hung_task\|BUG:\|reclaimed while dispatch waiters\|WAL scan: invalid/torn page"
	exit 3
fi
echo "[OK] 에러 없음"

echo
echo "=== ALL GC-CRASH-SAFETY CHECKS PASSED ==="
