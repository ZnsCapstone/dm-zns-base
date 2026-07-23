#!/usr/bin/env bash
# Run the test suite for a given milestone, or all of them in order if none given.
# Usage: sudo bash scripts/test.sh [<milestone>]
#   milestone: 0  → test-basic.sh  (M0 pass-through smoke test)
#              1  → test-m1.sh    (M1 random-to-sequential translation)
#              2  → test-m2.sh    (M2 mkfs+mount round-trip)
#              3  → test-crash.sh (crash-recovery via WAL replay)
#              4  → test-checkpoint.sh (WAL checkpoint skip-replay verification)
#              5  → test-sstable-read.sh (SSTable read-path, latest-seq_no-wins)
#              6  → test-compaction.sh (SSTable count shrinks, data survives merge)
#   (no argument) → runs 1, 2, 3, 4, 5, 6 in order
#
# Milestone 0(test-basic.sh)은 no-arg 실행에서 제외 — M1부터 target이 위로
# conventional(non-zoned) 인터페이스를 내주기로 하면서 .report_zones/
# .iterate_devices를 일부러 뺐는데, test-basic.sh는 그 이전 동작(zone
# passthrough)을 확인하는 스크립트라 지금 설계와 안 맞음. `test.sh 0`으로
# 직접 지정하면 참고용으로 여전히 돌릴 수 있음.
#
# Automatically brings up /dev/nullb0 if it is not present.
# Env: UNDERLYING (default /dev/nullb0), all nullblk-up.sh env vars forwarded.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
UNDERLYING=${UNDERLYING:-/dev/nullb0}

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

# ── argument check ────────────────────────────────────────────────────────────
if [ $# -gt 1 ]; then
    echo "Usage: sudo bash scripts/test.sh [<milestone>]" >&2
    echo "  milestone: 0 (M0), 1 (M1), 2 (M2), 3 (crash-recovery), 4 (checkpoint), 5 (sstable-read), 6 (compaction)" >&2
    echo "  omit milestone to run 1, 2, 3, 4, 5, 6 in order" >&2
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
        *) return 1 ;;
    esac
}

if [ $# -eq 1 ]; then
    script_for "$1" >/dev/null || {
        echo "[!] Unknown milestone '$1'. Supported: 0, 1, 2, 3, 4, 5, 6" >&2
        exit 1
    }
    MILESTONES=("$1")
else
    MILESTONES=(1 2 3 4 5 6)
fi

# ── ensure nullb0 is up ───────────────────────────────────────────────────────
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

# ── run the test(s) ───────────────────────────────────────────────────────────
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
