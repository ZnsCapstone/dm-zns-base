#!/usr/bin/env bash
# src/*.cmd 파일로부터 compile_commands.json을 재생성한다 (VS Code IntelliSense용).
# 모듈을 다시 빌드한 뒤(make) 이 스크립트를 다시 돌리면 됨.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KDIR=/lib/modules/$(uname -r)/build
GEN_SCRIPT="$KDIR/scripts/clang-tools/gen_compile_commands.py"

[ -f "$GEN_SCRIPT" ] || {
	echo "[!] $GEN_SCRIPT not found — kernel headers missing?" >&2
	exit 1
}

cd "$SRC_DIR"
python3 "$GEN_SCRIPT" -d "$KDIR" -o compile_commands.json .
echo "[*] compile_commands.json regenerated at $SRC_DIR/compile_commands.json"
