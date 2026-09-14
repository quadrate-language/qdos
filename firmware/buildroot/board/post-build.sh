#!/bin/sh
#
# Runs after the target filesystem is assembled, before the image is made.
set -eu

TARGET_DIR=$1
BOARD_DIR="$(dirname "$0")"

# QDOS is respawned by init rather than being PID 1. Running it as PID 1 would
# shave a little boot time, but then a crash panics the kernel and there is no
# console to find out why -- not a trade worth making before the hardware
# exists. Serial stays available for bring-up.
cat > "${TARGET_DIR}/etc/inittab" <<'EOF'
::sysinit:/bin/mount -t proc proc /proc
::sysinit:/bin/mount -t sysfs sysfs /sys
# /dev is not mounted here: the kernel has CONFIG_DEVTMPFS_MOUNT, so it has
# already done it, and mounting again just fails noisily.

# The panel exists from about a second into the boot but nothing draws to it
# until the shell is ready. Say something in between, so the machine looks busy
# rather than dead.
::sysinit:/usr/bin/qdos --splash --device

::sysinit:/bin/mkdir -p /dev/pts
::sysinit:/bin/mount -t devpts devpts /dev/pts
::sysinit:/bin/mount -o remount,ro /
::sysinit:/bin/mkdir -p /var/lib/qdos
::sysinit:/bin/mount -t ext4 /dev/mmcblk0p3 /var/lib/qdos

# The calculator itself, restarted if it ever exits.
#
# QDOS_KEYMAP tells it how to read a USB keyboard: evdev reports which key was
# pressed, not what it is labelled. Irrelevant once the machine has its own
# keypad, whose keys mean the same thing everywhere.
tty1::respawn:/usr/bin/env QDOS_KEYMAP=se /usr/bin/qdos --device

# Serial console for bring-up
ttyAMA0::respawn:/sbin/getty -L ttyAMA0 115200 vt100

::shutdown:/bin/umount -a -r
EOF

# Where sto/rcl registers and the saved session live. A separate writable
# partition keeps the rootfs read-only, so pulling the power cannot corrupt it.
mkdir -p "${TARGET_DIR}/var/lib/qdos"

# fsck on every boot would cost seconds the calculator does not have
cat > "${TARGET_DIR}/etc/fstab" <<'EOF'
/dev/root       /               ext4    ro,noatime      0 0
devtmpfs        /dev            devtmpfs defaults       0 0
proc            /proc           proc    defaults        0 0
sysfs           /sys            sysfs   defaults        0 0
tmpfs           /tmp            tmpfs   defaults        0 0
/dev/mmcblk0p3  /var/lib/qdos   ext4    defaults,noatime 0 0
EOF

install -m 0644 "${BOARD_DIR}/cmdline.txt" "${BINARIES_DIR}/rpi-firmware/cmdline.txt"
