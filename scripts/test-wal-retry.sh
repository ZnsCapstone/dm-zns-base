#!/usr/bin/env bash
# foreground data append 뒤 WAL 공간 할당이 일시 실패해도 원 bio에 I/O 오류를
# 반환하지 않고 pending queue에서 재시도하는지 검증한다.
#
# wal_batch_alloc_failures=1로 첫 WAL 배치 할당만 강제로 -ENOSPC 처리한다.
# 수정 전 코드는 fio가 I/O error로 실패하고, 수정 후에는 같은 batch가 재큐잉돼
# 다음 시도에 성공해야 한다. 마지막 read hash로 mapping/data 무결성도 확인한다.

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
WRITE_MB=${WRITE_MB:-32}
DATA_FILE=/tmp/wal-retry-test-data
READ_FILE=/tmp/wal-retry-test-read

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
	rm -f "$DATA_FILE" "$READ_FILE"
}
trap cleanup EXIT

[ -b "$UNDERLYING" ] || { echo "[!] $UNDERLYING is missing." >&2; exit 1; }
[ -f "$KO_PATH" ] || make -C "$SRC_DIR" >/dev/null || exit 1

dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod "$MOD_NAME" 2>/dev/null || true
blkzone reset "$UNDERLYING"
dmesg -C

sectors=$(blockdev --getsz "$UNDERLYING")
logical_sectors=$((sectors * 75 / 100))
zone_sectors=$(cat /sys/block/"$(basename "$UNDERLYING")"/queue/chunk_sectors)
logical_sectors=$((logical_sectors / zone_sectors * zone_sectors))

echo "=== [1/4] 첫 WAL 할당 실패를 주입해 target 생성 ==="
insmod "$KO_PATH" wal_batch_alloc_failures=1 || exit 1
echo "0 $logical_sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || exit 1

echo "=== [2/4] ${WRITE_MB}MiB 비동기 쓰기 — 일시 WAL 실패 뒤 재시도되어야 함 ==="
fio --name=wal-retry-write --filename="$DM_DEV" --rw=write --bs=4k \
	--size="${WRITE_MB}m" --ioengine=libaio --iodepth=32 --direct=1 \
	--buffer_pattern=0x5a --output-format=normal

echo "=== [3/4] 기록 내용 읽기 검증 ==="
dd if="$DM_DEV" of="$READ_FILE" bs=1M count="$WRITE_MB" iflag=direct status=none
expected=$(dd if=/dev/zero bs=1M count="$WRITE_MB" status=none | tr '\0' '\132' | sha256sum | awk '{print $1}')
actual=$(sha256sum "$READ_FILE" | awk '{print $1}')
[ "$expected" = "$actual" ] || {
	echo "[FAIL] 재시도 후 데이터 hash 불일치" >&2
	exit 1
}

echo "=== [4/4] 커널/파일시스템 I/O 오류 없음 확인 ==="
if dmesg | grep -qiE 'Buffer I/O error|blk_update_request.*error|EXT[234]-fs.*I/O error'; then
	dmesg | grep -iE 'Buffer I/O error|blk_update_request.*error|EXT[234]-fs.*I/O error'
	echo "[FAIL] WAL 재시도 중 I/O 오류가 노출됨" >&2
	exit 1
fi

echo "=== WAL ALLOCATION RETRY TEST PASSED ==="
