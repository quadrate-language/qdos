#!/usr/bin/env bash
#
# Boot the QDOS image under QEMU.
#
#   ./firmware/run-qemu.sh              # graphical, if a display is available
#   ./firmware/run-qemu.sh --serial     # serial console in this terminal
#   ./firmware/run-qemu.sh --vnc        # screen on VNC :5900
#   ./firmware/run-qemu.sh --screenshot # boot headless and capture a PNG
#
# Emulates a Pi 3B, the closest machine QEMU offers to the Zero 2 W -- both are
# quad Cortex-A53 on the same Broadcom silicon family. QEMU does not emulate the
# GPU bootloader, so the kernel is loaded directly rather than through
# bootcode.bin and start.elf; everything above that is the real image.
#
# Ctrl-C quits the graphical mode; Ctrl-A then X quits the serial ones.
set -euo pipefail

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CACHE=${QDOS_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/qdos-firmware}
IMAGES="$CACHE/build/images"
SERIAL_LOG="$CACHE/serial.log"
MODE=${1:-}

if [ ! -f "$IMAGES/sdcard.img" ]; then
	echo "No image. Build one first:" >&2
	echo "  ./firmware/build-image.sh" >&2
	exit 1
fi

# Prefer QEMU on the host: a container has no GPU access and needs the X socket
# plumbed in. Install with `pacman -S qemu-system-aarch64` or
# `apt install qemu-system-arm qemu-system-gui`.
if command -v qemu-system-aarch64 >/dev/null 2>&1; then
	NATIVE=1
else
	NATIVE=0
	echo "qemu-system-aarch64 not installed; running it in a container."
	echo "Installing it on the host gives a smoother display."
	echo
	docker build -q -t qdos-qemu "$QDOS_DIR/firmware/qemu" >/dev/null
fi

# QEMU insists an SD card image is a power of two, and ours is not. Pad a copy
# rather than inflating what gets flashed to real hardware.
PADDED="$CACHE/qemu-sdcard.img"
if [ ! -f "$PADDED" ] || [ "$IMAGES/sdcard.img" -nt "$PADDED" ]; then
	echo "Padding image to 256M for QEMU..."
	cp "$IMAGES/sdcard.img" "$PADDED"
	if [ "$NATIVE" = 1 ]; then
		qemu-img resize -f raw "$PADDED" 256M >/dev/null
	else
		docker run --rm -v "$CACHE:/c" qdos-qemu qemu-img resize -f raw /c/qemu-sdcard.img 256M >/dev/null
	fi
fi

# Paths are written as the container sees them and rewritten for a native run.
QEMU_ARGS=(
	-M raspi3b
	-kernel /img/Image
	-dtb /img/bcm2710-rpi-zero-2-w.dtb
	-drive file=/c/qemu-sdcard.img,format=raw,if=sd
	# earlycon is QEMU-specific: without it the PL011 console registers too late
	# to show the boot, which is the one thing worth watching when it fails. The
	# address is the BCM2837 UART0, where -M raspi3b puts it.
	-append "root=/dev/mmcblk0p2 rootwait ro console=ttyAMA0,115200 earlycon=pl011,0x3f201000 loglevel=4 vt.global_cursor_default=0 logo.nologo consoleblank=0"
	-usb -device usb-kbd
	# The machine has no audio and the default backend probes PipeWire, which is
	# absent in the container and complains about it on every boot.
	-audio none
)

# --init so docker's PID 1 reaps children and forwards signals.
#
# Always as the host user: anything the container writes into the cache -- the
# serial log, screendumps -- must stay writable by later runs, and X access
# control is usually SI:localuser:<you>, which a root container does not satisfy.
DOCKER_ARGS=(--rm --init -u "$(id -u):$(id -g)" -v "$IMAGES:/img:ro" -v "$CACHE:/c")

run_qemu() {
	if [ "$NATIVE" = 1 ]; then
		local args=()
		local arg
		for arg in "${QEMU_ARGS[@]}"; do
			arg=${arg//\/img\//$IMAGES/}
			arg=${arg//\/c\//$CACHE/}
			args+=("$arg")
		done
		qemu-system-aarch64 "${args[@]}"
	else
		docker run "${DOCKER_ARGS[@]}" qdos-qemu qemu-system-aarch64 "${QEMU_ARGS[@]}"
	fi
}

case "$MODE" in
--serial)
	# mon:stdio multiplexes the monitor onto the console, so Ctrl-A X quits. The
	# terminal is in raw mode here, which is why Ctrl-C will not.
	QEMU_ARGS+=(-display none -serial mon:stdio)
	DOCKER_ARGS+=(-i)
	[ -t 0 ] && DOCKER_ARGS+=(-t)
	echo "Serial console. Ctrl-A then X quits."
	echo
	run_qemu
	;;

--vnc)
	QEMU_ARGS+=(-vnc :0 -serial mon:stdio)
	DOCKER_ARGS+=(-p 5900:5900 -i)
	[ -t 0 ] && DOCKER_ARGS+=(-t)
	echo "Screen on VNC: display :0 on localhost, which is port 5900."
	echo "  vncviewer localhost:0          # most clients take a DISPLAY number"
	echo "  vncviewer localhost::5900      # some want a port, with two colons"
	echo
	echo "The screen is black until QDOS paints, about 10 seconds in."
	echo "Serial is on this terminal. Ctrl-A then X quits."
	echo
	run_qemu
	;;

--screenshot)
	# Boot headless and capture the panel. Needs no display at all, which makes
	# it the mode that always works -- and the one CI would use.
	OUT=${2:-$CACHE/screen.png}
	QEMU_ARGS+=(-display none -serial "file:/c/serial.log" -monitor stdio)
	DOCKER_ARGS+=(-i)
	rm -f "$CACHE/screen.ppm"
	echo "Booting headless, capturing to $OUT ..."
	{ sleep 25; echo "screendump /c/screen.ppm"; sleep 3; echo "quit"; } | run_qemu >/dev/null 2>&1 || true
	if [ ! -f "$CACHE/screen.ppm" ]; then
		echo "No screendump produced; see $SERIAL_LOG" >&2
		exit 1
	fi
	if command -v magick >/dev/null 2>&1; then
		magick "$CACHE/screen.ppm" "$OUT" && echo "Wrote $OUT"
	else
		echo "Wrote $CACHE/screen.ppm (install imagemagick for a PNG)"
	fi
	;;

*)
	if [ -z "${DISPLAY:-}" ] || [ ! -d /tmp/.X11-unix ]; then
		echo "No usable X display. Try --vnc, --serial, or --screenshot." >&2
		exit 1
	fi
	# A container has no GPU access, so GTK's GL probe fails noisily
	# (amdgpu/radeonsi) before falling back on its own. gl=off stops QEMU asking
	# for GL and LIBGL_ALWAYS_SOFTWARE stops GTK probing the hardware at all.
	#
	# Serial goes to a file rather than stdio, so the terminal stays in cooked
	# mode and Ctrl-C quits instead of being swallowed by the guest.
	QEMU_ARGS+=(-display gtk,gl=off -serial "file:/c/serial.log")
	if [ "$NATIVE" = 0 ]; then
		DOCKER_ARGS+=(
			-e "DISPLAY=$DISPLAY"
			-e "XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority}"
			-e LIBGL_ALWAYS_SOFTWARE=1
			-v /tmp/.X11-unix:/tmp/.X11-unix
		)
		if [ -n "${XAUTHORITY:-}" ] && [ -f "${XAUTHORITY}" ]; then
			DOCKER_ARGS+=(-v "${XAUTHORITY}:${XAUTHORITY}:ro")
		fi
	fi
	echo "A window should open. It stays black until QDOS paints, about 10s in:"
	echo "nothing draws to the framebuffer during boot by design."
	echo "Serial log: $SERIAL_LOG"
	echo "Ctrl-C here to quit."
	echo
	run_qemu
	;;
esac
