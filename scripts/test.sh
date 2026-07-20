#!/usr/bin/env bash
# Run the test suite for a given milestone, or all of them in order if none given.
# Usage: sudo bash scripts/test.sh [<milestone>]
#   milestone: 0  → test-basic.sh  (M0 pass-through smoke test)
#              1  → test-m1.sh    (M1 random-to-sequential translation)
#              2  → test-m2.sh    (M2 mkfs+mount round-trip)
#              3  → test-crash.sh (crash-recovery via WAL replay)
#   (no argument) → runs 0, 1, 2, 3 in order
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
    echo "  milestone: 0 (M0), 1 (M1), 2 (M2), 3 (crash-recovery)" >&2
    echo "  omit milestone to run 0, 1, 2, 3 in order" >&2
    exit 1
fi

script_for() {
    case "$1" in
        0) echo "$SCRIPT_DIR/test-basic.sh" ;;
        1) echo "$SCRIPT_DIR/test-m1.sh" ;;
        2) echo "$SCRIPT_DIR/test-m2.sh" ;;
        3) echo "$SCRIPT_DIR/test-crash.sh" ;;
        *) return 1 ;;
    esac
}

if [ $# -eq 1 ]; then
    script_for "$1" >/dev/null || {
        echo "[!] Unknown milestone '$1'. Supported: 0, 1, 2, 3" >&2
        exit 1
    }
    MILESTONES=("$1")
else
    MILESTONES=(0 1 2 3)
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
