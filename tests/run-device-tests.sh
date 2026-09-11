#!/usr/bin/env bash
#
# Run the tests that need access to real kernel devices.
#
#   ./tests/run-device-tests.sh
#
# test_keypad creates a virtual keypad through /dev/uinput and reads real key
# events back; test_framebuffer probes /dev/fb0's geometry. Both need device
# permissions an ordinary user does not have, and both skip cleanly without
# them — so `meson test` stays runnable by anyone, and this script is how the
# device paths actually get exercised.
#
# Nothing is written to the framebuffer: the probe is read-only and the blit
# goes into a private buffer. Running this does not disturb your display.
#
# The alternative to the container is adding yourself to the `input` and
# `video` groups, after which these run directly from `meson test`.
set -euo pipefail

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${BUILD:-build}
IMAGE=${IMAGE:-debian:trixie-slim}

if [ ! -x "$QDOS_DIR/$BUILD/tests/test_keypad" ]; then
	echo "Build first: meson compile -C $BUILD" >&2
	exit 1
fi

echo "Running device tests in a container with device access..."
echo

for t in test_keypad test_framebuffer; do
	echo "--- $t ---"
	docker run --rm --privileged \
		-v /dev:/dev \
		-v "$QDOS_DIR:/w" -w /w \
		"$IMAGE" "./$BUILD/tests/$t"
	echo
done
