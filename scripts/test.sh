#!/usr/bin/env bash
# 지정한 milestone의 테스트를 실행한다. 인자가 없으면 아래 기본 순서로 실행한다.
# 사용법: sudo bash scripts/test.sh [<milestone>]
# milestone 0 → test-basic.sh: 초기 zone 전달 경로의 참고 테스트
# 1 → test-m1.sh: 랜덤 쓰기의 순차 변환
# 2 → test-m2.sh: 파일시스템 생성·마운트·재마운트
# 3 → test-crash.sh: 재적재 후 WAL replay
# 4 → test-checkpoint.sh: checkpoint 이전 WAL 생략
# 5 → test-sstable-read.sh: 최신 세대의 SSTable 읽기
# 6 → test-compaction.sh: 병합 후 table 수와 데이터 유지
# 7 → test-gc.sh: 작은 장치의 데이터 GC와 overwrite 진행
# 8 → test-wal-reclaim.sh: WAL 회수와 재적재 후 데이터 복원
# 9 → test-gc-crash.sh: GC 이주 후 재적재 복원
# 10 → test-wal-retry.sh: WAL 공간 부족 재시도와 데이터 정합성
# 11 → test-discard.sh: discard tombstone과 재생
# 인자 없음 → 1부터 11까지 순서대로 실행한다.
#
# M0는 초기 zone 전달 설계의 참고 테스트이므로 기본 순서에서 제외한다.
# 현재 타깃은 상위에 conventional 장치를 노출한다. M0는 test.sh 0으로 직접 지정할 수 있다.
#
# /dev/nullb0이 없으면 자동으로 생성한다.
# 환경변수: UNDERLYING 기본값은 /dev/nullb0이다. nullblk-up.sh의 환경변수도 전달한다.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
UNDERLYING=${UNDERLYING:-/dev/nullb0}

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

# ── 인자 확인 ────────────────────────────────────────────────────────────
if [ $# -gt 1 ]; then
	echo "Usage: sudo bash scripts/test.sh [<milestone>]" >&2
	echo "  milestone: 0 (M0), 1 (M1), 2 (M2), 3 (crash-recovery), 4 (checkpoint), 5 (sstable-read), 6 (compaction), 7 (gc), 8 (wal-reclaim), 9 (gc-crash), 10 (wal-retry), 11 (discard)" >&2
	echo "  omit milestone to run 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 in order" >&2
	exit 1
fi

script_for() {
	case "$1" in
		0) echo "$SCRIPT_DIR/test-basic.sh" ;;
		1) echo "$SCRIPT_DIR/test-m1.sh" ;;
		2) echo "$SCRIPT_DIR/test-m2.sh" ;;
		3) echo "$SCRIPT_DIR/test-crash.sh" ;;
		4) echo "$SCRIPT_DIR/test-checkpoint.sh" ;;
		5) echo "$SCRIPT_DIR/test-sstable-read.sh" ;;
		6) echo "$SCRIPT_DIR/test-compaction.sh" ;;
		7) echo "$SCRIPT_DIR/test-gc.sh" ;;
		8) echo "$SCRIPT_DIR/test-wal-reclaim.sh" ;;
		9) echo "$SCRIPT_DIR/test-gc-crash.sh" ;;
		10) echo "$SCRIPT_DIR/test-wal-retry.sh" ;;
		11) echo "$SCRIPT_DIR/test-discard.sh" ;;
		*) return 1 ;;
	esac
}

if [ $# -eq 1 ]; then
	script_for "$1" >/dev/null || {
		echo "[!] Unknown milestone '$1'. Supported: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11" >&2
		exit 1
	}
	MILESTONES=("$1")
else
	MILESTONES=(1 2 3 4 5 6 7 8 9 10 11)
fi

# ── null_blk 장치 확인 ───────────────────────────────────────────────────────
if ! [ -b "$UNDERLYING" ]; then
	echo "[*] $UNDERLYING not found — running nullblk-up.sh"
	bash "$SCRIPT_DIR/nullblk-up.sh" || { echo "[!] nullblk-up.sh failed" >&2; exit 1; }
else
	zoned=$(cat /sys/block/"$(basename "$UNDERLYING")"/queue/zoned 2>/dev/null || echo "")
	if [ "$zoned" != "host-managed" ]; then
		echo "[!] $UNDERLYING exists but is not host-managed zoned (zoned=$zoned)" >&2
		exit 1
	fi
	echo "[*] $UNDERLYING already up (host-managed zoned)"
fi

# ── 테스트 실행 ───────────────────────────────────────────────────────────
for m in "${MILESTONES[@]}"; do
	TEST_SCRIPT=$(script_for "$m")

	if [ ! -f "$TEST_SCRIPT" ]; then
		echo "[!] $TEST_SCRIPT not found." >&2
		exit 1
	fi

	echo "[*] Running M$m test: $TEST_SCRIPT"
	echo
	bash "$TEST_SCRIPT" || { echo "[!] M$m test failed" >&2; exit 1; }
	echo
done

echo "=== ALL REQUESTED TESTS PASSED ==="
