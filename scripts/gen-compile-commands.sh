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

# *.annotated.c는 실제로 컴파일되지 않아(Makefile이 참조 안 함) 위 생성기가
# 못 찾는다 — 상응하는 실제 소스 파일의 컴파일 커맨드를 그대로 복제해서
# clangd가 같은 include path/define으로 파싱하게 해준다(VS Code 표시용).
python3 - "$SRC_DIR" <<'PYEOF'
import json, sys, glob, os

src_dir = sys.argv[1]
cc_path = os.path.join(src_dir, "compile_commands.json")
with open(cc_path) as f:
    data = json.load(f)

by_file = {e["file"]: e for e in data}
data = [e for e in data if not e["file"].endswith(".annotated.c")]

for annotated in glob.glob(os.path.join(src_dir, "*.annotated.c")):
    base = annotated[: -len(".annotated.c")] + ".c"
    if base not in by_file:
        continue
    entry = dict(by_file[base])
    old_obj = base[:-2] + ".o"
    new_obj = annotated[:-2] + ".o"
    old_dep = "." + os.path.basename(base)[:-2] + ".o.d"
    new_dep = "." + os.path.basename(annotated)[:-2] + ".o.d"
    cmd = entry["command"]
    cmd = cmd.replace(old_dep, new_dep).replace(old_obj, new_obj).replace(base, annotated)
    entry["command"] = cmd
    entry["file"] = annotated
    data.append(entry)

with open(cc_path, "w") as f:
    json.dump(data, f, indent=2)
PYEOF

echo "[*] compile_commands.json regenerated at $SRC_DIR/compile_commands.json"
