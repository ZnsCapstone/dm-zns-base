#!/usr/bin/env bash
# 선택 사항: 스크립트를 비밀번호 입력 없이 실행하도록 /etc/sudoers.d/dm-zns-base를 설치한다.
# 제거할 때는 sudo rm /etc/sudoers.d/dm-zns-base를 실행한다.

set -euo pipefail

DST=/etc/sudoers.d/dm-zns-base
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

if [ "$(id -u)" -ne 0 ]; then
	echo "[!] Run as root:  sudo bash $0" >&2
	exit 1
fi

USER_NAME=${SUDO_USER:-$(logname 2>/dev/null || echo splab)}

cat > "$TMP" <<EOF
# dm-zns-base 보조 스크립트의 비밀번호 없는 실행 규칙이다.
$USER_NAME ALL=(root) NOPASSWD: /usr/bin/bash /home/splab/dm-zns-base/scripts/nullblk-up.sh
$USER_NAME ALL=(root) NOPASSWD: /usr/bin/bash /home/splab/dm-zns-base/scripts/nullblk-down.sh
$USER_NAME ALL=(root) NOPASSWD: /usr/bin/bash /home/splab/dm-zns-base/scripts/test-basic.sh
EOF

if ! visudo -c -f "$TMP"; then
	echo "[!] sudoers syntax check failed; aborting" >&2
	exit 1
fi

install -m 0440 -o root -g root "$TMP" "$DST"
echo "[OK] Installed $DST (for user: $USER_NAME)"
echo "    scripts/*.sh can now be run without a password prompt."
