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

# Programs uploaded from a PC, on their own FAT partition so any machine can
# write it and the USB gadget can hand the whole thing over. Mounted read-only
# and read in place: QDOS never copies out of here, so editing an upload on the
# calculator cannot be undone by the next boot.
::sysinit:/bin/mkdir -p /mnt/inbox
::sysinit:/bin/mount -t vfat -o ro /dev/mmcblk0p4 /mnt/inbox

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

# Hands the inbox partition to a PC and takes it back. QDOS runs this from the
# USB setting; the messy half lives here so it can be read and fixed on the
# machine without rebuilding the firmware.
#
# The partition is unmounted while the host has it. Two operating systems
# writing one filesystem corrupts it, and the host must be the only writer.
cat > "${TARGET_DIR}/usr/bin/qdos-usb" <<'EOF'
#!/bin/sh
set -eu

DEV=/dev/mmcblk0p4
MNT=/mnt/inbox

case "${1:-}" in
share)
	# Flush and let go before the host touches a single block
	sync
	umount "$MNT" 2>/dev/null || true

	if ! modprobe g_mass_storage "file=$DEV" removable=1 stall=0 iSerialNumber=qdos; then
		# Put it back: failing to share is a nuisance, but leaving the
		# calculator without its inbox is a fault
		mount -t vfat -o ro "$DEV" "$MNT" 2>/dev/null || true
		exit 1
	fi
	;;
take)
	modprobe -r g_mass_storage 2>/dev/null || true

	# Never fail here. The gadget is already gone, so refusing would leave
	# QDOS thinking the card is still shared when it is not. A host that
	# unplugged early may have left the filesystem dirty; an inbox that will
	# not mount reads as empty, which the shell already copes with.
	mount -t vfat -o ro "$DEV" "$MNT" 2>/dev/null || true
	;;
*)
	echo "usage: qdos-usb share|take" >&2
	exit 2
	;;
esac
EOF
chmod 0755 "${TARGET_DIR}/usr/bin/qdos-usb"

mkdir -p "${TARGET_DIR}/mnt/inbox"

# Programs shipped with the firmware. These live on the read-only rootfs, so a
# user can override one but never lose it, and an unclean power-off cannot
# corrupt them.
mkdir -p "${TARGET_DIR}/usr/share/qdos/programs"
install -m 0644 "${BOARD_DIR}/../../../programs/system/"*.qd \
	"${TARGET_DIR}/usr/share/qdos/programs/"

# Apps are folders, each copied whole
for app in "${BOARD_DIR}/../../../programs/system/"*/; do
	[ -d "${app}" ] || continue
	cp -r "${app%/}" "${TARGET_DIR}/usr/share/qdos/programs/"
done

# One user app out of the box, seeding the writable partition
install -m 0644 "${BOARD_DIR}/../../../programs/user/"*.qd \
	"${TARGET_DIR}/var/lib/qdos/"

# fsck on every boot would cost seconds the calculator does not have
cat > "${TARGET_DIR}/etc/fstab" <<'EOF'
/dev/root       /               ext4    ro,noatime      0 0
devtmpfs        /dev            devtmpfs defaults       0 0
proc            /proc           proc    defaults        0 0
sysfs           /sys            sysfs   defaults        0 0
tmpfs           /tmp            tmpfs   defaults        0 0
/dev/mmcblk0p3  /var/lib/qdos   ext4    defaults,noatime 0 0
/dev/mmcblk0p4  /mnt/inbox      vfat    ro,noatime      0 0
EOF

# Device-tree overlays we maintain ourselves, compiled into the firmware's
# overlay directory so config.txt can name them.
mkdir -p "${BINARIES_DIR}/rpi-firmware/overlays"
for dts in "${BOARD_DIR}"/overlays/*.dts; do
	[ -f "$dts" ] || continue
	name=$(basename "$dts" -overlay.dts)
	"${HOST_DIR}/bin/dtc" -@ -I dts -O dtb -o \
		"${BINARIES_DIR}/rpi-firmware/overlays/${name}.dtbo" "$dts"
done

install -m 0644 "${BOARD_DIR}/cmdline.txt" "${BINARIES_DIR}/rpi-firmware/cmdline.txt"
