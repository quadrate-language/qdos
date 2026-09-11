# Build environment for cross-compiling QDOS to the Raspberry Pi Zero 2 W.
#
# Cross-compiles on x86 rather than emulating an ARM64 userland, which is far
# faster, and carries qemu-user so the resulting tests can still be executed.
FROM debian:trixie-slim

# Quadrate's tree configures every library, including ones the calculator does
# not need (tls, http), so their ARM64 dependencies must be present for meson
# setup to succeed even though QDOS links none of them.
RUN dpkg --add-architecture arm64

RUN apt-get update && apt-get install -y --no-install-recommends \
		libssl-dev:arm64 \
		zlib1g-dev:arm64 \
		gcc-aarch64-linux-gnu \
		g++-aarch64-linux-gnu \
		meson \
		ninja-build \
		python3 \
		pkg-config \
		qemu-user \
		make \
		file \
	&& rm -rf /var/lib/apt/lists/*

# Where qemu-aarch64 finds the target's shared libraries
ENV QEMU_LD_PREFIX=/usr/aarch64-linux-gnu

WORKDIR /work
