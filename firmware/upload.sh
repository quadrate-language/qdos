#!/usr/bin/env bash
#
# Put files on a built image's inbox, the way a PC does.
#
#   ./firmware/upload.sh hello.qd libfoo.so
#   ./firmware/upload.sh ../qdos-doom/doom      # an app: a whole folder
#   QDOS_BOARD=zero2w ./firmware/upload.sh hello.qd
#   ./firmware/upload.sh --list
#
# The inbox is the FAT partition the USB gadget hands over, so writing it from
# here is the same act as dropping a file on the drive that appears when the
# calculator is plugged in -- minus the gadget, which needs the board.
#
# mtools rather than a loop mount: no root, and no chance of leaving a stale
# mount behind if this is interrupted.
set -euo pipefail

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CACHE=${QDOS_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/qdos-firmware}
BOARD=${QDOS_BOARD:-zerow}
IMAGE=${QDOS_IMAGE:-$CACHE/build-$BOARD/images/sdcard.img}

# The partition genimage.cfg.in puts the inbox in
INBOX_PARTITION=4

if ! command -v mcopy >/dev/null; then
	echo "$0: needs mtools (mcopy, mdir)" >&2
	exit 1
fi

if [ ! -f "$IMAGE" ]; then
	echo "$0: no image at $IMAGE" >&2
	echo "Build one first: QDOS_BOARD=$BOARD ./firmware/build-image.sh" >&2
	exit 1
fi

# Read the offset rather than hardcoding a sector: the layout has changed once
# already, and a wrong offset writes into the middle of another filesystem.
read -r START UNIT < <(
	fdisk -l -o Device,Start "$IMAGE" 2>/dev/null |
		awk -v want="$INBOX_PARTITION" '
			/^Units:/ { unit = $(NF-1) }
			$1 ~ want"$" && $2 ~ /^[0-9]+$/ { print $2, (unit ? unit : 512); exit }
		'
)

if [ -z "${START:-}" ]; then
	echo "$0: no partition $INBOX_PARTITION in $IMAGE" >&2
	echo "An image built before the inbox existed has only three." >&2
	exit 1
fi

OFFSET=$((START * UNIT))
DRIVE="$IMAGE@@$OFFSET"

if [ "${1:-}" = "--list" ]; then
	mdir -i "$DRIVE" ::
	exit 0
fi

if [ $# -lt 1 ]; then
	echo "usage: $0 FILE|APPDIR... | --list" >&2
	exit 2
fi

for file in "$@"; do
	name=$(basename "${file%/}")

	# -o overwrites: uploading a second time should replace, as it would on a
	# drive the host has open
	if [ -d "$file" ]; then
		# An app is a folder and goes over whole, entry point and all
		if [ ! -f "${file%/}/main.qd" ]; then
			echo "$0: $file is not an app: no main.qd in it" >&2
			exit 1
		fi
		mmd -i "$DRIVE" "::$name" 2>/dev/null || true
		mcopy -i "$DRIVE" -s -o "${file%/}"/* "::$name/"
		echo "uploaded app $name/"
	elif [ -f "$file" ]; then
		mcopy -i "$DRIVE" -o "$file" "::$name"
		echo "uploaded $name"
	else
		echo "$0: no such file: $file" >&2
		exit 1
	fi
done

echo
mdir -i "$DRIVE" ::
