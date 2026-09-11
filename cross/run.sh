#!/usr/bin/env bash
#
# Build the cross toolchain image and run the ARM64 build and test suite in it.
#
#   ./cross/run.sh
#
# Expects the Quadrate source checked out alongside this project.
set -euo pipefail

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
QUADRATE_DIR=${QUADRATE_DIR:-$(cd "$QDOS_DIR/../quadrate" && pwd)}

echo "qdos:     $QDOS_DIR"
echo "quadrate: $QUADRATE_DIR"
echo

docker build -t qdos-cross-aarch64 "$QDOS_DIR/cross"

docker run --rm \
	-v "$QDOS_DIR:/work/qdos" \
	-v "$QUADRATE_DIR:/work/quadrate" \
	-u "$(id -u):$(id -g)" \
	-e QUADRATE_SRC=/work/quadrate \
	-e QDOS_SRC=/work/qdos \
	qdos-cross-aarch64 \
	bash /work/qdos/cross/build-aarch64.sh
