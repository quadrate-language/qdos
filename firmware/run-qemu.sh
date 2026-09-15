#!/usr/bin/env bash
#
# Boot the QDOS image under QEMU.
#
#   ./firmware/run-qemu.sh              # graphical, if a display is available
#   ./firmware/run-qemu.sh --serial     # serial console in this terminal
#   ./firmware/run-qemu.sh --vnc        # screen on VNC :5900
#   ./firmware/run-qemu.sh --screenshot # boot headless and capture a PNG
#
# QDOS_BOARD picks the target: zerow (default) or zero2w.
#
# QEMU does not emulate the GPU bootloader, so the kernel is loaded directly
# rather than through bootcode.bin and start.elf; everything above that is the
# real image.
#
# Ctrl-C quits the graphical mode; Ctrl-A then X quits the serial ones.
set -euo pipefail

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CACHE=${QDOS_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/qdos-firmware}
BOARD=${QDOS_BOARD:-zerow}
IMAGES="$CACHE/build-$BOARD/images"
SERIAL_LOG="$CACHE/serial-$BOARD.log"
MODE=${1:-}

case "$BOARD" in
zerow)
	# BCM2835, ARM1176, ARMv6. QEMU calls the machine raspi0; the Zero W is a
	# Zero with wireless, which QEMU does not emulate anyway.
	QEMU_BIN=qemu-system-arm
	MACHINE=raspi0
	KERNEL=zImage
	DTB=bcm2708-rpi-zero-w.dtb
	# BCM2835 puts its peripherals at 0x20000000, unlike the later chips
	EARLYCON=0x20201000
	# QEMU's raspi0 does not implement the BCM2835 power controller, and
	# probing it takes an external abort that kills the deferred-probe worker
	# with interrupts disabled. Nothing else needs it under emulation.
	EXTRA_APPEND="initcall_blacklist=bcm2835_power_driver_init"
	# The stock device tree looks for the SD card on a controller QEMU does not
	# have. See firmware/qemu/make-qemu-dtb.sh.
	PATCH_DTB=1
	# raspi0 is a single emulated ARM1176 and runs roughly two orders of
	# magnitude slower than realtime, so a headless capture has to wait
	# minutes rather than seconds. Override with QDOS_BOOT_WAIT.
	BOOT_WAIT=${QDOS_BOOT_WAIT:-900}
	KNOWN_BROKEN=1
	;;
zero2w)
	# BCM2837 is the closest QEMU offers to the Zero 2 W's BCM2710A1 -- both
	# quad Cortex-A53 on the same silicon family.
	QEMU_BIN=qemu-system-aarch64
	MACHINE=raspi3b
	KERNEL=Image
	DTB=bcm2710-rpi-zero-2-w.dtb
	EARLYCON=0x3f201000
	EXTRA_APPEND=""
	PATCH_DTB=0
	BOOT_WAIT=${QDOS_BOOT_WAIT:-25}
	KNOWN_BROKEN=0
	;;
*)
	echo "Unknown QDOS_BOARD '$BOARD' (expected zerow or zero2w)" >&2
	exit 1
	;;
esac

# raspi0 does not get QDOS on screen. The patched device tree gets it as far as
# enumerating the card and finding the partitions, and then the guest freezes
# with QEMU pegged at 100% -- the guest clock stops, so even a plain rootdelay
# sleep never returns. Neither clk_ignore_unused nor rootdelay changes it.
#
# Not worth more digging: the Zero 2 W target boots the same userspace in 25s,
# and ./cross/run.sh already runs the whole suite as ARMv6 under qemu-arm.
if [ "$KNOWN_BROKEN" = 1 ] && [ "${QDOS_FORCE_BROKEN:-0}" != 1 ]; then
	echo "QDOS_BOARD=zerow does not boot under QEMU's raspi0 (known, see comment above)." >&2
	echo >&2
	echo "  Same userspace, 25s boot:  QDOS_BOARD=zero2w $0 ${MODE:-}" >&2
	echo "  ARMv6 correctness:         ./cross/run.sh" >&2
	echo "  Anyway, for debugging it:  QDOS_FORCE_BROKEN=1 $0 ${MODE:-}" >&2
	exit 1
fi

if [ ! -f "$IMAGES/sdcard.img" ]; then
	echo "No $BOARD image. Build one first:" >&2
	echo "  QDOS_BOARD=$BOARD ./firmware/build-image.sh" >&2
	exit 1
fi

# Prefer QEMU on the host: a container has no GPU access and needs the X socket
# plumbed in. Install with `pacman -S qemu` or
# `apt install qemu-system-arm qemu-system-gui`.
if command -v "$QEMU_BIN" >/dev/null 2>&1; then
	NATIVE=1
else
	NATIVE=0
	echo "$QEMU_BIN not installed; running it in a container."
	echo "Installing it on the host gives a smoother display."
	echo
	docker build -q -t qdos-qemu "$QDOS_DIR/firmware/qemu" >/dev/null
fi

# QEMU insists an SD card image is a power of two, and ours is not. Pad a copy
# rather than inflating what gets flashed to real hardware.
PADDED="$CACHE/qemu-sdcard-$BOARD.img"
if [ ! -f "$PADDED" ] || [ "$IMAGES/sdcard.img" -nt "$PADDED" ]; then
	echo "Padding image to 256M for QEMU..."
	cp "$IMAGES/sdcard.img" "$PADDED"
	if [ "$NATIVE" = 1 ]; then
		qemu-img resize -f raw "$PADDED" 256M >/dev/null
	else
		docker run --rm -v "$CACHE:/c" qdos-qemu qemu-img resize -f raw "/c/qemu-sdcard-$BOARD.img" 256M >/dev/null
	fi
fi

# The device tree QEMU is handed: the board's own, or a patched copy for
# machines whose emulation does not match the real hardware.
DTB_ARG="/img/$DTB"
if [ "$PATCH_DTB" = 1 ]; then
	PATCHED="$CACHE/${DTB%.dtb}-qemu.dtb"
	if [ ! -f "$PATCHED" ] || [ "$IMAGES/$DTB" -nt "$PATCHED" ]; then
		"$QDOS_DIR/firmware/qemu/make-qemu-dtb.sh" "$IMAGES/$DTB" "$PATCHED"
	fi
	DTB_ARG="/c/$(basename "$PATCHED")"
fi

# Paths are written as the container sees them and rewritten for a native run.
QEMU_ARGS=(
	-M "$MACHINE"
	-kernel "/img/$KERNEL"
	-dtb "$DTB_ARG"
	-drive file=/c/qemu-sdcard-$BOARD.img,format=raw,if=sd
	# earlycon is QEMU-specific: without it the PL011 console registers too late
	# to show the boot, which is the one thing worth watching when it fails.
	-append "root=/dev/mmcblk0p2 rootwait ro console=ttyAMA0,115200 earlycon=pl011,$EARLYCON $EXTRA_APPEND loglevel=4 vt.global_cursor_default=0 logo.nologo consoleblank=0"
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
		"$QEMU_BIN" "${args[@]}"
	else
		docker run "${DOCKER_ARGS[@]}" qdos-qemu "$QEMU_BIN" "${QEMU_ARGS[@]}"
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
	QEMU_ARGS+=(-display none -serial "file:/c/$(basename "$SERIAL_LOG")" -monitor stdio)
	DOCKER_ARGS+=(-i)
	rm -f "$CACHE/screen.ppm"
	echo "Booting headless, capturing to $OUT (waiting ${BOOT_WAIT}s) ..."
	{ sleep "$BOOT_WAIT"; echo "screendump /c/screen.ppm"; sleep 5; echo "quit"; } | run_qemu >/dev/null 2>&1 || true
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
