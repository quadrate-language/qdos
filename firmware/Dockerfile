# Buildroot host environment. Buildroot builds its own cross toolchain, so this
# only needs the tools it drives.
FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
		bc build-essential bzip2 ca-certificates cpio file git \
		libncurses-dev locales python3 rsync sed unzip wget which \
		mtools dosfstools \
	&& rm -rf /var/lib/apt/lists/*

# Buildroot refuses to run as root
RUN useradd -m -u 1000 builder
USER builder
WORKDIR /work
