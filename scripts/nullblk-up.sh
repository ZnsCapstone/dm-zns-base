#!/usr/bin/env bash
# zoned null_blk 장치를 생성한다.
# 같은 이름의 host-managed zoned 장치가 있으면 유지한다.
# 환경변수: NB_NAME / NB_SIZE_MB / NB_ZONE_SIZE_MB, 기본값은 nullb0 / 2048 / 64이다.

set -uo pipefail

NB_NAME=${NB_NAME:-nullb0}
NB_SIZE_MB=${NB_SIZE_MB:-2048}
NB_ZONE_SIZE_MB=${NB_ZONE_SIZE_MB:-64}

DEV="/dev/$NB_NAME"
CONFIG="/sys/kernel/config/nullb/$NB_NAME"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

if [ -b "$DEV" ] && [ "$(cat /sys/block/$NB_NAME/queue/zoned 2>/dev/null)" = "host-managed" ]; then
	echo "[*] $DEV is already host-managed zoned; nothing to do."
	exit 0
fi

# nr_devices=0으로 자동 생성을 막고 configfs에서 장치를 구성한다.
if ! lsmod | grep -q '^null_blk '; then
	echo "[*] Loading null_blk"
	modprobe null_blk nr_devices=0 || { echo "[!] modprobe null_blk failed" >&2; exit 1; }
fi

if ! mountpoint -q /sys/kernel/config; then
	mount -t configfs none /sys/kernel/config
fi

if [ -d "$CONFIG" ]; then
	echo "[*] Removing stale $CONFIG"
	echo 0 > "$CONFIG/power" 2>/dev/null || true
	rmdir "$CONFIG" || { echo "[!] Could not remove existing instance" >&2; exit 1; }
fi

echo "[*] Creating zoned null_blk: $DEV (${NB_SIZE_MB} MB, ${NB_ZONE_SIZE_MB} MB zones)"
mkdir "$CONFIG"
echo 1                 > "$CONFIG/zoned"
echo $NB_ZONE_SIZE_MB  > "$CONFIG/zone_size"
echo $NB_SIZE_MB       > "$CONFIG/size"
echo 1                 > "$CONFIG/memory_backed"
echo 1                 > "$CONFIG/power"

sleep 0.2

if [ ! -b "$DEV" ]; then
	echo "[!] $DEV did not appear — check dmesg" >&2; exit 1;
fi

echo
echo "[OK] $DEV is ready"
echo "    zoned         = $(cat /sys/block/$NB_NAME/queue/zoned)"
echo "    chunk_sectors = $(cat /sys/block/$NB_NAME/queue/chunk_sectors)"
echo "    nr_zones      = $(cat /sys/block/$NB_NAME/queue/nr_zones)"
echo "    size(sectors) = $(cat /sys/block/$NB_NAME/size)"
