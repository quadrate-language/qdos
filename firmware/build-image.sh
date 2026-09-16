#!/usr/bin/env bash
#
# Build the QDOS SD card image.
#
#   ./firmware/build-image.sh            # build
#   ./firmware/build-image.sh menuconfig # adjust the configuration
#
# QDOS is rebuilt every time; the defconfig and the kernel config fragments are
# reread when they change.
# Changes to the Quadrate source are not picked up automatically -- run
# ./firmware/build-image.sh quadrate-dirclean first. Not quadrate-rebuild:
# that reuses the build directory and its stale meson state.
#
# The first build downloads and compiles a toolchain and a kernel, so expect
# 30-60 minutes. Later builds reuse firmware/build and are far quicker.
#
# Output: $XDG_CACHE_HOME/qdos-firmware/build-$QDOS_BOARD/images/sdcard.img
#
# The Buildroot checkout and its output live outside this repository. They are
# several gigabytes, and Buildroot rsyncs the source tree into its own build
# directory -- with the output inside the tree, that copy includes itself.
set -euo pipefail

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
QUADRATE_DIR=${QUADRATE_DIR:-$(cd "$QDOS_DIR/../quadrate" && pwd)}
BUILDROOT_VERSION=${BUILDROOT_VERSION:-2025.02.18}
WORK="$QDOS_DIR/firmware"
CACHE=${QDOS_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/qdos-firmware}
BUILDROOT_DIR="$CACHE/buildroot-$BUILDROOT_VERSION"
BOARD=${QDOS_BOARD:-zerow}
OUT="$CACHE/build-$BOARD"
TARGET=${1:-}

case "$BOARD" in
zerow|zero2w) ;;
*)
	echo "Unknown QDOS_BOARD '$BOARD' (expected zerow or zero2w)" >&2
	exit 1
	;;
esac

# Each board gets its own output tree. They share nothing but the download
# cache, so switching targets never rebuilds the other one.
mkdir -p "$OUT"

echo "qdos:      $QDOS_DIR"
echo "quadrate:  $QUADRATE_DIR"
echo "buildroot: $BUILDROOT_VERSION"
echo "board:     $BOARD"
echo "output:    $OUT"
echo

if [ ! -d "$QUADRATE_DIR" ]; then
	echo "Quadrate source not found at $QUADRATE_DIR" >&2
	echo "Check it out alongside this project, or set QUADRATE_DIR." >&2
	exit 1
fi

docker build -t qdos-firmware "$WORK"

if [ ! -d "$BUILDROOT_DIR" ]; then
	echo "Fetching Buildroot $BUILDROOT_VERSION..."
	git clone --depth 1 --branch "$BUILDROOT_VERSION" \
		https://git.buildroot.net/buildroot "$BUILDROOT_DIR"
fi

mkdir -p "$OUT" "$CACHE"

# The package .mk files read these to find the local source trees
TTY=$([ -t 0 ] && echo -it || echo -i)
# Our pinned kernel tarball is not one Buildroot ships a hash for. Merge ours in
# so BR2_DOWNLOAD_FORCE_CHECK_HASHES still holds for everything else.
KERNEL_HASH="$QDOS_DIR/firmware/buildroot/board/linux.hash"
if [ -f "$KERNEL_HASH" ]; then
	grep -hv '^#' "$KERNEL_HASH" | while read -r line; do
		[ -n "$line" ] || continue
		grep -qF "$line" "$BUILDROOT_DIR/linux/linux.hash" 2>/dev/null \
			|| echo "$line" >> "$BUILDROOT_DIR/linux/linux.hash"
	done
fi

docker run --rm $TTY \
	-v "$QDOS_DIR:/work/qdos" \
	-v "$QUADRATE_DIR:/work/quadrate" \
	-v "$BUILDROOT_DIR:/work/buildroot" \
	-v "$OUT:/work/build" \
	-u "$(id -u):$(id -g)" \
	-e QDOS_SRC=/work/qdos \
	-e QDOS_QUADRATE_SRC=/work/quadrate \
	-e BR2_EXTERNAL=/work/qdos/firmware/buildroot \
	-w /work/buildroot \
	qdos-firmware \
	bash -c '
		set -e
		DEFCONFIG=/work/qdos/firmware/buildroot/configs/qdos_'"${BOARD}"'_defconfig
		if [ ! -f /work/build/.config ]; then
			make O=/work/build qdos_'"${BOARD}"'_defconfig
		elif [ "$DEFCONFIG" -nt /work/build/.config ]; then
			# Buildroot does not reread the defconfig on its own, so an edit
			# there would otherwise be silently ignored. This discards anything
			# set with menuconfig -- put lasting changes in the defconfig.
			echo ">>> defconfig changed; regenerating .config"
			make O=/work/build qdos_'"${BOARD}"'_defconfig
		fi
		# Kernel config fragments are read once, at linux-configure. Editing one
		# afterwards changes nothing at all -- the build succeeds and quietly
		# ships the previous kernel. Compare against the built .config and
		# reconfigure when a fragment is newer.
		KCONFIG=/work/build/build/linux-custom/.config
		if [ -f "$KCONFIG" ]; then
			for frag in /work/qdos/firmware/buildroot/board/*.fragment; do
				[ -f "$frag" ] || continue
				if [ "$frag" -nt "$KCONFIG" ]; then
					echo ">>> $(basename "$frag") changed; reconfiguring the kernel"
					make O=/work/build linux-reconfigure
					break
				fi
			done
		fi

		if [ -z "'"${TARGET}"'" ]; then
			# qdos is a local package under active development. Buildroot will
			# not notice its source changed, so it would otherwise keep shipping
			# whatever was built first. Cheap: one small binary.
			make O=/work/build qdos-rebuild
		fi
		make O=/work/build '"${TARGET}"'
	'
