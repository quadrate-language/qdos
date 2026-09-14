# QEMU host for booting the QDOS image without hardware.
#
# qemu-system-gui carries the gtk and SDL display backends; qemu-system-arm on
# its own is headless and can only do VNC.
FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
		qemu-system-arm \
		qemu-system-gui \
		qemu-utils \
		socat \
	&& rm -rf /var/lib/apt/lists/*
