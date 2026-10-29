#!/usr/bin/env bash
# 작은 null_blk 장치에서 데이터 GC와 반복 overwrite의 진행 여부를 확인한다.
# FILL_PERCENT 영역을 채운 뒤 같은 영역에 1.2배 쓰기를 수행한다.
# --size는 접근 범위, --io_size는 총 쓰기량이며 두 단계의 접근 범위는 동일하다.
# 기본 점유율 50%는 이주 공간을 남기기 위한 기능 검증 조건이다.
# 이 설정을 FEMU 80% endurance 기준 통과로 해석하지 않는다.
# 큰 flush_threshold로 SSTable 작업을 줄이며 GC 동작을 관찰한다.
#
# 환경변수: UNDERLYING (기본값 /dev/nullb0), DM_NAME (기본값 myzns-base),
#      FILL_PERCENT (기본값 50),
#      GC_LOW_WATERMARK (기본값 8 — null_blk 기본 32 zone 기준).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
GC_LOW_WATERMARK=${GC_LOW_WATERMARK:-8}
FILL_PERCENT=${FILL_PERCENT:-50}
DATA_FILE=/tmp/gc-test-sanity-data

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"

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
dev_bytes=$((sectors * 512))
fill_bytes=$((dev_bytes * FILL_PERCENT / 100))
over_bytes=$((fill_bytes * 12 / 10))

echo "=== [1/6] insmod (gc_low_watermark=$GC_LOW_WATERMARK, fill=${FILL_PERCENT}%) ==="
insmod "$KO_PATH" gc_low_watermark="$GC_LOW_WATERMARK" || {
	echo "[!] insmod failed" >&2; exit 1;
}
echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}
echo "[OK] device=${dev_bytes} bytes, ${FILL_PERCENT}%=${fill_bytes} bytes, 120% of that=${over_bytes} bytes"

echo
echo "=== [2/6] 용량의 ${FILL_PERCENT}%까지 randwrite로 채우기 ==="
fio --name=gc-fill \
	--filename="$DM_DEV" \
	--rw=randwrite \
	--bs=4k \
	--size=$fill_bytes \
	--ioengine=libaio \
	--iodepth=32 \
	--direct=1 \
	--output-format=normal \
	2>&1 | grep -E 'WRITE|iops|err'
FIO1_STATUS=${PIPESTATUS[0]}
sync
sleep 3   # GC(백그라운드 workqueue)가 이번 fill 도중 트리거된 만큼 정리될 시간

if [ "$FIO1_STATUS" -ne 0 ]; then
	echo "[FAIL] 1단계(${FILL_PERCENT}% 채우기) 중 실패 (fio exit=$FIO1_STATUS)" >&2
	exit 3
fi
echo "[OK]"

echo
echo "=== [3/6] 같은 영역(${FILL_PERCENT}%)에 1.2배 분량 random overwrite — GC를 여러 번 유발 ==="
fio --name=gc-overwrite \
	--filename="$DM_DEV" \
	--rw=randwrite \
	--bs=4k \
	--size=$fill_bytes \
	--io_size=$over_bytes \
	--ioengine=libaio \
	--iodepth=32 \
	--direct=1 \
	--output-format=normal \
	2>&1 | grep -E 'WRITE|iops|err'
FIO2_STATUS=${PIPESTATUS[0]}
sync
sleep 5   # GC/zone reset이 다 끝날 시간을 넉넉히

if [ "$FIO2_STATUS" -ne 0 ]; then
	echo "[FAIL] 2단계(1.2배 overwrite) 중 실패 (fio exit=$FIO2_STATUS) — ENOSPC 등" >&2
	exit 3
fi
echo "[OK] no space 에러 없이 완료 (M3 성공 기준 통과)"

echo
echo "=== [4/6] GC 로그 확인 ==="
GC_COUNT=$(dmesg | grep -c "gc: reclaimed zone" || true)
echo "    gc: reclaimed zone 로그: ${GC_COUNT}건"
if [ "$GC_COUNT" -lt 1 ]; then
	echo "[!] GC가 한 번도 안 돌았습니다 (GC_LOW_WATERMARK를 더 높여보세요)" >&2
	exit 3
fi
dmesg | grep "gc:" | tail -10
if dmesg | grep -q "gc: failed to build a complete latest-map"; then
	echo "[FAIL] GC latest-map 구축이 완전하지 않아 회수가 중단됨" >&2
	exit 3
fi
echo "[OK]"

echo
echo "=== [5/6] GC/overwrite 이후에도 정상적으로 읽고 쓰는지 최종 sanity 체크 ==="
dd if=/dev/urandom of="$DATA_FILE" bs=4K count=256 2>/dev/null
dd if="$DATA_FILE" of="$DM_DEV" bs=4K count=256 oflag=direct 2>/dev/null
sync
HASH_EXPECTED=$(md5sum "$DATA_FILE" | awk '{print $1}')
HASH_READ=$(dd if="$DM_DEV" bs=4K count=256 iflag=direct 2>/dev/null | md5sum | awk '{print $1}')
echo "    expected = $HASH_EXPECTED"
echo "    read back = $HASH_READ"
if [ "$HASH_READ" = "$HASH_EXPECTED" ]; then
	echo "[OK] GC로 zone을 재활용한 뒤에도 읽기/쓰기가 정확함"
else
	echo "[FAIL] 읽은 값이 원본과 다름" >&2
	exit 3
fi

echo
echo "=== [6/6] 커널 에러 확인 ==="
if dmesg | grep -qi "blk_update_request\|I/O error\|hung_task\|BUG:"; then
	echo "[FAIL] 커널 에러/행 감지됨" >&2
	dmesg | grep -i "blk_update_request\|I/O error\|hung_task\|BUG:"
	exit 3
fi
echo "[OK] 에러 없음"

echo
echo "=== ALL GC CHECKS PASSED ==="
