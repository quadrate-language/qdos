# Build environment for cross-compiling QDOS to a Raspberry Pi Zero.
#
# Cross-compiles on x86 rather than emulating an ARM userland, which is far
# faster, and carries qemu-user so the resulting tests can still be executed.
#
# Both targets live here: armhf for the Zero W (ARM1176, ARMv6) and arm64 for
# the Zero 2 W (Cortex-A53, ARMv8).
FROM debian:trixie-slim

# Quadrate's tree configures every library, including ones the calculator does
# not need (tls, http), so their target dependencies must be present for meson
# setup to succeed even though QDOS links none of them.
RUN dpkg --add-architecture arm64 && dpkg --add-architecture armhf

RUN apt-get update && apt-get install -y --no-install-recommends \
		libssl-dev:arm64 \
		zlib1g-dev:arm64 \
		libssl-dev:armhf \
		zlib1g-dev:armhf \
		gcc-aarch64-linux-gnu \
		g++-aarch64-linux-gnu \
		gcc-arm-linux-gnueabihf \
		g++-arm-linux-gnueabihf \
		meson \
		ninja-build \
		python3 \
		pkg-config \
		qemu-user \
		make \
		file \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /work
