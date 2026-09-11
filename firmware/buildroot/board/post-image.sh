#!/bin/sh
#
# Assembles the SD card image.
set -eu

BOARD_DIR="$(dirname "$0")"
GENIMAGE_CFG="${BOARD_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# An empty data partition, mounted at /var/lib/qdos on the device
rm -rf "${GENIMAGE_TMP}"
mkdir -p "${BINARIES_DIR}/data"

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
