#!/usr/bin/env bash
# Run the test suite for a given milestone.
# Usage: sudo bash scripts/test.sh <milestone>
#   milestone: 0  → test-basic.sh (M0 pass-through smoke test)
#              1  → test-m1.sh   (M1 random-to-sequential translation)
#
# Automatically brings up /dev/nullb0 if it is not present.
# Env: UNDERLYING (default /dev/nullb0), all nullblk-up.sh env vars forwarded.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
UNDERLYING=${UNDERLYING:-/dev/nullb0}

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

# ── argument check ────────────────────────────────────────────────────────────
if [ $# -ne 1 ]; then
    echo "Usage: sudo bash scripts/test.sh <milestone>" >&2
    echo "  milestone: 0 (M0), 1 (M1)" >&2
    exit 1
fi

case "$1" in
    0) TEST_SCRIPT="$SCRIPT_DIR/test-basic.sh" ;;
    1) TEST_SCRIPT="$SCRIPT_DIR/test-m1.sh" ;;
    *)
        echo "[!] Unknown milestone '$1'. Supported: 0, 1" >&2
        exit 1
        ;;
esac

if [ ! -f "$TEST_SCRIPT" ]; then
    echo "[!] $TEST_SCRIPT not found." >&2
    exit 1
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

# ── run the test ──────────────────────────────────────────────────────────────
echo "[*] Running M$1 test: $TEST_SCRIPT"
echo
bash "$TEST_SCRIPT"
