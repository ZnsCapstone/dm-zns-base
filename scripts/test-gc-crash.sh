#!/usr/bin/env bash
# GC 재배치의 크래시 안전성 검증 (A번 이슈 수정 확인용).
#
# 버그: gc_relocate_one이 데이터를 새 위치에 durable하게 쓴 뒤 mapping_put으로
# memtable만 갱신하고 WAL에는 안 남겼다. 그 매핑이 SSTable로 flush되기 전에
# 크래시가 나면 WAL replay가 옛 위치(곧 reset될 victim)로 복원 → victim이
# 이미 지워져 그 블록이 0으로 읽힌다(데이터 유실). 수정: 재배치도 WAL PUT을
# 남겨(gc_wal_log_put) replay가 새 위치로 복원하게 함.
#
# 재현 전략:
#   - flush_threshold를 기본값(매우 큼)으로 둬 flush를 아예 안 나게 한다 →
#     재배치 매핑이 memtable(+WAL)에만 있는 "취약한 창"을 크래시까지 유지.
#   - 영역 A 전체를 먼저 순차로 채워(모든 블록 non-zero) 전부 살아있게 한다.
#   - A 안에서 random overwrite를 해 각 zone을 부분적으로 죽인다(uniform) →
#     free zone이 watermark 이하로 떨어지면 GC가 "부분만 죽은" zone을 victim으로
#     골라 살아있는 블록을 재배치할 수밖에 없다(이게 이 테스트의 핵심 —
#     "M memtable entries relocated"의 M>0을 확인).
#   - zone reset 없이 재적재(크래시) 후, A 전체를 읽어 all-zero 4K 블록이
#     하나라도 있으면 유실(FAIL). A는 전부 urandom으로 썼으므로 정상이면
#     0 블록이 없어야 한다.
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-base),
#      FILL_MB (default 512 — 너무 크면 flush 없이 WAL이 안 줄어 ENOSPC),
#      OVERWRITE_MB (default 256 — A 안에서 덮어쓸 총량, 부분-죽은 zone 생성),
#      GC_LOW_WATERMARK (default 20 — 높게 잡아 GC가 일찍/자주 돌게).

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
sleep 5   # GC(재배치+reset)가 끝날 시간
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
