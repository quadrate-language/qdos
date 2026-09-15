#!/usr/bin/env bash
#
# Build the cross toolchain image and run an ARM build and test suite in it.
#
#   ./cross/run.sh            # Zero W   (ARMv6)
#   QDOS_ARCH=aarch64 ./cross/run.sh   # Zero 2 W (ARM64)
#
# Expects the Quadrate source checked out alongside this project.
set -euo pipefail

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
QUADRATE_DIR=${QUADRATE_DIR:-$(cd "$QDOS_DIR/../quadrate" && pwd)}
ARCH=${QDOS_ARCH:-armv6}

echo "qdos:     $QDOS_DIR"
echo "quadrate: $QUADRATE_DIR"
echo "arch:     $ARCH"
echo

docker build -t qdos-cross "$QDOS_DIR/cross"

docker run --rm \
	-v "$QDOS_DIR:/work/qdos" \
	-v "$QUADRATE_DIR:/work/quadrate" \
	-u "$(id -u):$(id -g)" \
	-e QUADRATE_SRC=/work/quadrate \
	-e QDOS_SRC=/work/qdos \
	-e QDOS_ARCH="$ARCH" \
	qdos-cross \
	bash /work/qdos/cross/build-cross.sh
