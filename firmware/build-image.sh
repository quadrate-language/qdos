#!/usr/bin/env bash
#
# Build the QDOS SD card image.
#
#   ./firmware/build-image.sh            # build
#   ./firmware/build-image.sh menuconfig # adjust the configuration
#
# The first build downloads and compiles a toolchain and a kernel, so expect
# 30-60 minutes. Later builds reuse firmware/build and are far quicker.
#
# Output: $XDG_CACHE_HOME/qdos-firmware/build/images/sdcard.img
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
OUT="$CACHE/build"
TARGET=${1:-}

echo "qdos:      $QDOS_DIR"
echo "quadrate:  $QUADRATE_DIR"
echo "buildroot: $BUILDROOT_VERSION"
echo "output:    $CACHE"
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
		if [ ! -f /work/build/.config ]; then
			make O=/work/build qdos_zero2w_defconfig
		fi
		make O=/work/build '"${TARGET}"'
	'
