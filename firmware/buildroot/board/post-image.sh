#!/bin/sh
#
# Assembles the SD card image.
set -eu

BOARD_DIR="$(dirname "$0")"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# The partition layout is identical for both boards; only the kernel image and
# device tree are named differently. Fill them in from the active .config
# rather than keeping two near-identical genimage files in sync.
DTS=$(sed -n 's/^BR2_LINUX_KERNEL_INTREE_DTS_NAME="\(.*\)"/\1/p' "${BR2_CONFIG}")
DTB="$(basename "${DTS}").dtb"
if grep -q '^BR2_aarch64=y' "${BR2_CONFIG}"; then
	KERNEL="Image"
else
	KERNEL="zImage"
fi

GENIMAGE_CFG="${BUILD_DIR}/genimage.cfg"
sed -e "s/@DTB@/${DTB}/" -e "s/@KERNEL@/${KERNEL}/" \
	"${BOARD_DIR}/genimage.cfg.in" > "${GENIMAGE_CFG}"
echo "Image layout: kernel ${KERNEL}, device tree ${DTB}"

# An empty data partition, mounted at /var/lib/qdos on the device
rm -rf "${GENIMAGE_TMP}"
mkdir -p "${BINARIES_DIR}/data"

# Seeds the inbox partition, so a fresh card explains itself when a PC first
# opens it -- whether over USB or in a card reader.
mkdir -p "${BINARIES_DIR}/qdos-inbox"
cat > "${BINARIES_DIR}/qdos-inbox/README.TXT" <<'EOF'
Put Quadrate programs here as .qd files.

	fn sq(x:i64 -- r:i64) { dup * }

One word per file, named after the word: sq.qd

QDOS declares them at the next boot. To do it without one, turn USB off again
under SETTINGS and it reads the card there and then.

Files here are never written to by the calculator. Editing an uploaded program
on the machine keeps a separate copy, which wins while it exists; the APPS list
marks it USER*. Delete the .qd file here to be rid of an upload for good.
EOF

rm -rf "${GENIMAGE_TMP}"
genimage \
	--rootpath "${TARGET_DIR}" \
	--tmppath "${GENIMAGE_TMP}" \
	--inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"

echo
echo "Image ready: ${BINARIES_DIR}/sdcard.img"
echo "Flash with:  sudo dd if=${BINARIES_DIR}/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress"
