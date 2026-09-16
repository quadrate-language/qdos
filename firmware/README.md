# Firmware

Two ways to get QDOS onto a Raspberry Pi. Start with the first.

`QDOS_BOARD` picks the target throughout, defaulting to the **Zero W**:

| `QDOS_BOARD` | board | SoC | cross `QDOS_ARCH` |
|---|---|---|---|
| `zerow` (default) | Pi Zero W | BCM2835, ARM1176, ARMv6 | `armv6` |
| `zero2w` | Pi Zero 2 W | BCM2710A1, Cortex-A53, ARMv8 | `aarch64` |

## Bring-up: Raspberry Pi OS Lite

```bash
QDOS_ARCH=aarch64 ./cross/run.sh                # build for ARM64
./firmware/stage-to-raspios.sh pi@raspberrypi.local
```

Note that `stage-to-raspios.sh` currently stages `build/aarch64/qdos` only, and
refuses anything that is not an ARM64 binary — so this path reaches a Zero 2 W
running 64-bit Raspberry Pi OS, not a Zero W. Bringing up the ARMv6 target this
way means teaching the script to take `build/armv6/qdos` as well.

A normal distro with QDOS on it, started by a systemd unit on tty1. Not firmware
— but the remaining risk in this project is the panel, the keypad wiring and the
device tree, and none of that needs a custom image to find out about. Buildroot
tuning is wasted effort until you know the display works.

`QDOS_FB`, `QDOS_INPUT` and `QDOS_STORE` override the device paths, so you can
chase whatever the board actually enumerates without rebuilding:

```bash
sudo systemctl edit qdos     # add Environment=QDOS_FB=/dev/fb1
```

## Emulation: QEMU

```bash
./firmware/run-qemu.sh              # graphical; Ctrl-C quits
./firmware/run-qemu.sh --serial     # console in this terminal; Ctrl-A X quits
./firmware/run-qemu.sh --vnc        # screen on localhost:5900
./firmware/run-qemu.sh --screenshot # boot headless, write a PNG
```

Uses QEMU from the host if installed, otherwise a container. Installing it
natively is smoother for the graphical mode, since a container has no GPU access
and needs the X socket plumbed in:

```bash
sudo pacman -S qemu-system-aarch64          # Arch
sudo apt install qemu-system-arm qemu-system-gui   # Debian/Ubuntu
```

`--screenshot` needs no display at all, which makes it the mode that always
works — and the one CI would use. Serial always lands in
`~/.cache/qdos-firmware/serial.log`.

**The screen is black for the first ~10 seconds, by design.** Nothing draws to
the framebuffer during boot: the kernel logo is off, the console is on serial,
and QDOS paints only once init starts it. A black screen after that is a real
failure; before it, it is the intended behaviour.

For `--vnc`, note that most viewers take a *display number*, not a port:
`vncviewer localhost:0` reaches port 5900, while `vncviewer localhost:5900`
tries port 11900 and finds nothing. Clients that want a port usually take two
colons: `localhost::5900`.

**Emulation runs the `zero2w` target**, on QEMU's `raspi3b` — the closest machine
it offers to a Zero 2 W, both quad Cortex-A53 on the same Broadcom silicon
family. QEMU does not emulate the GPU bootloader, so the kernel is loaded
directly rather than through `bootcode.bin` and `start.elf`; everything above
that is the real image, including the framebuffer, the SD card and USB input.

The default `zerow` target does **not** boot here. QEMU's `raspi0` does not match
a Zero W closely enough: `qemu/make-qemu-dtb.sh` patches the device tree far
enough to get the card enumerated and the partitions read, and then the guest
freezes with QEMU pegged at 100% — the guest clock stops, so even a plain
`rootdelay` sleep never returns. `run-qemu.sh` says so and points at the other
target rather than showing a black screen:

```bash
QDOS_BOARD=zero2w ./firmware/build-image.sh
QDOS_BOARD=zero2w ./firmware/run-qemu.sh
```

The two images share everything but the architecture, kernel config and device
tree, so booting `zero2w` still exercises the userspace the Zero W will run. The
ARMv6 build is verified the other way, by `QDOS_ARCH=armv6 ./cross/run.sh`
running the whole suite under `qemu-arm`.

Worth running before touching hardware. It has already caught three bugs that
would have been baffling on a real Pi:

- `gpu_mem=16` starved the firmware framebuffer, which `CONFIG_FB_BCM2708`
  allocates from GPU memory — a black screen with no explanation.
- The console echoed every keystroke over the display, because QDOS had not
  taken the input device exclusively. Fixed with `EVIOCGRAB`.
- Buildroot silently shipped a stale binary, because it does not notice a local
  package's source changed.

### The boot screen

`qdos --splash` paints a startup screen, run from the inittab as soon as init
starts. Without it the panel is black from power-on until the shell is ready and
the machine looks dead rather than busy.

It cannot cover the whole boot. The framebuffer exists about a second before
init runs, and nothing in userspace can draw before then:

```
~1.5s   framebuffer registered      black
~2.6s   init starts, splash drawn   QDOS
~4s     shell paints                the calculator
```

Before 1.25s nothing in Linux can draw at all — the framebuffer driver does not
exist yet. That gap belongs to the **Pi's own firmware splash**, which is why
`disable_splash=1` is deliberately absent from `config.txt`. It is the Raspberry
Pi test pattern rather than anything of ours, but it appears within a few
hundred milliseconds of power-on, which nothing else can.

**None of that shows under QEMU**, which loads the kernel directly and never runs
the GPU bootloader. Emulated boots therefore look worse than the real thing at
the start, and the gap you see there is not the gap the hardware has.

`board/logo.png` is built from the QDOS font as a kernel logo, and is compiled
in, but is not displayed: fbcon will not draw a logo onto a console that is
already live, and `console=tty1` makes it live from 0.009s. Putting the console
there would also put `sysinit` output on the panel. Left disabled.

QDOS takes the VT in `KD_GRAPHICS` mode while it runs, so nothing can draw over
the calculator afterwards whatever the console is set to. The splash
deliberately does not hand the console back on exit: doing so makes the console
repaint and erase what it just drew.

### Boot messages you can ignore

Three warnings appear under QEMU and are not defects:

| | |
|---|---|
| `bcm2835-aux-uart: unable to register 8250 port` | QEMU does not emulate the mini UART. Real hardware registers it. |
| `bcm2835-power: ASB register ID returned 0x00000000` | QEMU's power controller is a stub. |
| `mmc1: Timeout waiting for hardware interrupt` | QEMU has no SDIO wifi on mmc1. |

A fourth appears on hardware too:

`Warning: unable to open an initial console` — the kernel probes for
`/dev/console` before the root filesystem is mounted, so a node inside the
rootfs cannot satisfy it. Nothing depends on it: the inittab names `tty1` and
`ttyAMA0` explicitly, so QDOS and the getty each get their own device. The node
is in the image regardless, via `board/device_table.txt`, because a rootfs
without one is wrong even when nothing notices.

A few things the emulator needs that hardware does not: `earlycon`, because the
PL011 console otherwise registers too late to show the boot; a power-of-two SD
image, so the script pads a copy; and software GL, because the container cannot
reach the GPU.

## Firmware: a Buildroot image

```bash
./firmware/build-image.sh
sudo dd if=~/.cache/qdos-firmware/build-zerow/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Each board builds into its own output tree (`build-zerow`, `build-zero2w`), so
switching targets never rebuilds the other one — and the path above changes with
`QDOS_BOARD`.

The first build downloads and compiles a toolchain and a kernel, so expect
30–60 minutes. Later builds reuse the cache and take minutes.

Buildroot and its output live in `~/.cache/qdos-firmware` (override with
`QDOS_BUILD_DIR`), not in this repository. They are several gigabytes, and
Buildroot rsyncs the source tree into its own build directory — with the output
inside the tree, that copy would include itself.

`./firmware/build-image.sh menuconfig` adjusts the configuration.

### What the image contains

```
partition 1  64M  FAT   bootcode.bin, start.elf, fixup.dat, config.txt, cmdline.txt, Image, dtb, overlays
partition 2  80M  ext4  read-only rootfs: busybox, kmod, qdos
partition 3  16M  ext4  /var/lib/qdos — registers and the saved session
partition 4  32M  FAT   /mnt/inbox — programs uploaded from a PC
```

Four is the limit an MBR allows, and all four are spoken for. The image is about
193 MB. `qdos` sits at `/usr/bin/qdos`, respawned by init on tty1.

The rootfs is mounted **read-only** and everything QDOS writes goes to the third
partition. A calculator is always switched off by pulling the power, so the
filesystem holding the system must never be mid-write when that happens.

The inbox is its own FAT partition rather than a directory on the boot one, for
two reasons. FAT so any computer can write it from a card reader, and separate so
the USB gadget can hand the whole partition to a host without exposing the boot
files or anything written on the calculator. It is mounted **read-only** and read
in place: QDOS never copies out of it, so editing an uploaded program on the
machine cannot be undone by the next boot.

`/usr/bin/qdos-usb share|take` is what does the handing over — it unmounts the
partition, binds `g_mass_storage` to it, and puts it back afterwards. QDOS runs
it from the `USB` row on the settings page. Two operating systems writing one
filesystem corrupts it, so the host is the only writer while it is shared.

The kernel console is on **serial only**. QDOS draws straight into `/dev/fb0`,
and the framebuffer console draws there too — with `console=tty1` the two fight
over the same pixels and boot messages land on top of the calculator. Kernel
output therefore goes to ttyAMA0, and `logo.nologo`, `vt.global_cursor_default=0`
and `consoleblank=0` keep anything else off the panel.

Note that `print` and `nl` in Quadrate still write to stdout, which is the same
problem one level up: on the device that output lands on the console rather than
the display. Routing it into the shell's own scrollback is outstanding.

QDOS is respawned by init rather than being PID 1. As PID 1 it would save a
little boot time, but a crash would panic the kernel with no console to explain
why — not a trade worth making before the hardware exists. A serial getty stays
on ttyAMA0 for bring-up.

### Layout

```
buildroot/
  configs/qdos_zerow_defconfig      the default target, ARMv6
  configs/qdos_zero2w_defconfig     derived from Buildroot's raspberrypizero2w_64
  package/quadrate/                 the language libraries, built without LLVM
  package/qdos/                     the calculator
  board/config.txt                  boot configuration, incl. panel overlays
  board/cmdline.txt                 kernel command line
  board/genimage.cfg                partition layout
  board/post-build.sh               inittab, fstab, partition mounts, qdos-usb
  board/post-image.sh               assembles sdcard.img
```

`package/quadrate` builds with `-Dbuild_tools=false`, which drops the
command-line tools and with them the LLVM dependency. That is the interpreter
tier paying off: the language runs on the device without its compiler.

### Rebuilding after a source change

QDOS is rebuilt on every `build-image.sh` run, because Buildroot does not notice
that a local package's source changed and would otherwise keep shipping whatever
was built first. Changes to the **Quadrate** source are not picked up
automatically:

```bash
./firmware/build-image.sh quadrate-rebuild
./firmware/build-image.sh
```

### Changing config.txt

`config.txt` is copied into the image by the `rpi-firmware` package at install
time, so editing it and rebuilding is not enough — the package is already
installed and will not re-copy. Force it:

```bash
./firmware/build-image.sh rpi-firmware-reinstall
./firmware/build-image.sh
```

`cmdline.txt` has no such problem; `post-build.sh` copies it every build.

### Display and keypad

`board/config.txt` boots to HDMI by default, which is right for bring-up. An SPI
panel and a GPIO keypad need device tree overlays; both are in that file,
commented, with the parameters to adjust.

An `fbtft` panel (ST7789, ILI9341) presents a real `/dev/fb0`, and a
`matrix-keypad` overlay presents a real evdev node — so the device backend needs
no changes for either, only the right overlay lines.

## Status

**The `zero2w` image boots and runs under QEMU.** Kernel to init to QDOS drawing
on the framebuffer, with USB keyboard input evaluated and displayed — the whole
chain, on emulated Pi silicon. The `zerow` image is the default target and the
one the hardware will run, but QEMU cannot boot it; it is covered by the ARMv6
cross-build and test run instead.

Not yet verified on real hardware: the specific SPI panel, the GPIO keypad
matrix, boot timing and power behaviour. QEMU emulates a generic framebuffer and
USB keyboard, so it proves the software path but says nothing about the parts
that do not exist yet.
