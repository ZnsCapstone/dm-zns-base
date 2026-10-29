#!/usr/bin/env bash
# Vagrant 초기 구성에서는 필요한 패키지만 설치한다.
# null_blk 생성은 scripts/nullblk-up.sh로 분리해 재실행할 수 있게 한다.
# 메모리 기반 장치는 재부팅 후 유지되지 않는다.

set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y \
	build-essential \
	git \
	linux-headers-$(uname -r) \
	linux-modules-extra-$(uname -r) \
	dmsetup \
	util-linux \
	nvme-cli \
	fio

cat <<'EOF'

[provision] Done. Next, inside the guest:
  cd /vagrant
  sudo bash scripts/nullblk-up.sh
  cd src && make && cd ..
  sudo bash scripts/test-basic.sh
EOF
