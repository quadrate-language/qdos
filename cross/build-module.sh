#!/usr/bin/env bash
#
# Cross-compile a native module for the calculator.
#
#   ./cross/build-module.sh gpio.c               # Zero W   (ARMv6) -> libgpio.so
#   QDOS_ARCH=aarch64 ./cross/build-module.sh gpio.c   # Zero 2 W (ARM64)
#
# The result goes beside the source, named the way the loader looks for it:
# libNAME.so, whose words the calculator reaches as NAME::word. Copy it onto the
# card along with any .qd that calls it.
#
# Uses the same container as ./cross/run.sh, so no toolchain is needed on the
# host beyond Docker. The flags are the ones in cross/*.ini, and they matter:
# a stock armhf compiler targets ARMv7 with NEON, which builds and then dies
# with SIGILL on an ARM1176.
set -euo pipefail

if [ $# -lt 1 ]; then
	echo "usage: $0 SOURCE.c [more.c ...]" >&2
	exit 2
fi

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ARCH=${QDOS_ARCH:-armv6}

case "$ARCH" in
	armv6)
		CC=arm-linux-gnueabihf-gcc
		FLAGS=(-mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -marm)
		;;
	aarch64)
		CC=aarch64-linux-gnu-gcc
		FLAGS=(-mcpu=cortex-a53)
		;;
	*)
		echo "$0: unknown QDOS_ARCH '$ARCH', expected armv6 or aarch64" >&2
		exit 2
		;;
esac

# The module is named after its first source file, that being the one carrying
# the descriptor in every example there is
SOURCE_DIR=$(cd "$(dirname "$1")" && pwd)
STEM=$(basename "$1" .c)
OUTPUT="lib${STEM}.so"

echo "arch:   $ARCH"
echo "source: $SOURCE_DIR"
echo "output: $OUTPUT"
echo

docker build -t qdos-cross "$QDOS_DIR/cross" >/dev/null

SOURCES=()
for file in "$@"; do
	SOURCES+=("/work/src/$(basename "$file")")
done

docker run --rm \
	-v "$QDOS_DIR:/work/qdos:ro" \
	-v "$SOURCE_DIR:/work/src" \
	-u "$(id -u):$(id -g)" \
	-w /work/src \
	qdos-cross \
	"$CC" "${FLAGS[@]}" -std=c11 -Wall -Wextra -O2 \
	-shared -fPIC -fvisibility=hidden \
	-I /work/qdos/include \
	"${SOURCES[@]}" -o "/work/src/$OUTPUT"

echo
echo "built $SOURCE_DIR/$OUTPUT"
