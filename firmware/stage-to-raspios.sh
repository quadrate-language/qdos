#!/usr/bin/env bash
#
# Put QDOS onto an existing Raspberry Pi OS Lite install.
#
#   ./firmware/stage-to-raspios.sh pi@raspberrypi.local
#
# This is the bring-up path, not the firmware one: a normal distro with QDOS on
# it. Use it to find out whether the panel and keypad work before spending time
# on a Buildroot image. The firmware image is build-image.sh.
set -euo pipefail

TARGET=${1:-}
if [ -z "$TARGET" ]; then
	echo "usage: $0 user@host" >&2
	exit 1
fi

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BINARY="$QDOS_DIR/build/aarch64/qdos"

if [ ! -f "$BINARY" ]; then
	echo "No ARM64 binary. Build one first:" >&2
	echo "  ./cross/run.sh" >&2
	exit 1
fi

file "$BINARY" | grep -q aarch64 || { echo "$BINARY is not an ARM64 binary" >&2; exit 1; }

echo "Copying $(du -h "$BINARY" | cut -f1) to $TARGET..."
scp "$BINARY" "$TARGET:/tmp/qdos"

ssh "$TARGET" 'sudo sh -s' <<'REMOTE'
set -e
install -m 0755 /tmp/qdos /usr/local/bin/qdos
rm -f /tmp/qdos
mkdir -p /var/lib/qdos

cat > /etc/systemd/system/qdos.service <<'UNIT'
[Unit]
Description=QDOS calculator
After=local-fs.target

[Service]
# Runs on the console rather than under a login shell
ExecStart=/usr/local/bin/qdos --device
Restart=always
StandardInput=tty
StandardOutput=tty
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes
Environment=QDOS_STORE=/var/lib/qdos

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
echo
echo "Installed. The framebuffer and input device it will use:"
ls -l /dev/fb* /dev/input/event* 2>/dev/null || echo "  none found -- check your overlays"
echo
echo "Start it with:   sudo systemctl start qdos"
echo "On every boot:   sudo systemctl enable qdos"
echo "Override paths:  systemctl edit qdos   (QDOS_FB, QDOS_INPUT)"
REMOTE
