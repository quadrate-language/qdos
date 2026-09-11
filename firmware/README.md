# Firmware

Two ways to get QDOS onto a Raspberry Pi Zero 2 W. Start with the first.

## Bring-up: Raspberry Pi OS Lite

```bash
./cross/run.sh                                  # build for ARM64
./firmware/stage-to-raspios.sh pi@raspberrypi.local
```

A normal distro with QDOS on it, started by a systemd unit on tty1. Not firmware
— but the remaining risk in this project is the panel, the keypad wiring and the
device tree, and none of that needs a custom image to find out about. Buildroot
tuning is wasted effort until you know the display works.

`QDOS_FB`, `QDOS_INPUT` and `QDOS_STORE` override the device paths, so you can
chase whatever the board actually enumerates without rebuilding:

```bash
sudo systemctl edit qdos     # add Environment=QDOS_FB=/dev/fb1
```

## Firmware: a Buildroot image

```bash
./firmware/build-image.sh
sudo dd if=~/.cache/qdos-firmware/build/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
```

The first build downloads and compiles a toolchain and a kernel, so expect
30–60 minutes. Later builds reuse the cache and take minutes.

Buildroot and its output live in `~/.cache/qdos-firmware` (override with
`QDOS_BUILD_DIR`), not in this repository. They are several gigabytes, and
Buildroot rsyncs the source tree into its own build directory — with the output
inside the tree, that copy would include itself.

`./firmware/build-image.sh menuconfig` adjusts the configuration.

### What the image contains

```
partition 1  32M  FAT   bootcode.bin, start.elf, fixup.dat, config.txt, cmdline.txt, Image, dtb, overlays
partition 2  80M  ext4  read-only rootfs: busybox, kmod, qdos
partition 3  16M  ext4  /var/lib/qdos — registers and the saved session
```

The image is about 129 MB. `qdos` sits at `/usr/bin/qdos`, respawned by init on
tty1.

The rootfs is mounted **read-only** and everything QDOS writes goes to the third
partition. A calculator is always switched off by pulling the power, so the
filesystem holding the system must never be mid-write when that happens.

QDOS is respawned by init rather than being PID 1. As PID 1 it would save a
little boot time, but a crash would panic the kernel with no console to explain
why — not a trade worth making before the hardware exists. A serial getty stays
on ttyAMA0 for bring-up.

### Layout

```
buildroot/
  configs/qdos_zero2w_defconfig     derived from Buildroot's raspberrypizero2w_64
  package/quadrate/                 the language libraries, built without LLVM
  package/qdos/                     the calculator
  board/config.txt                  boot configuration, incl. panel overlays
  board/cmdline.txt                 kernel command line
  board/genimage.cfg                partition layout
  board/post-build.sh               inittab, fstab, data partition mount
  board/post-image.sh               assembles sdcard.img
```

`package/quadrate` builds with `-Dbuild_tools=false`, which drops the
command-line tools and with them the LLVM dependency. That is the interpreter
tier paying off: the language runs on the device without its compiler.

### Display and keypad

`board/config.txt` boots to HDMI by default, which is right for bring-up. An SPI
panel and a GPIO keypad need device tree overlays; both are in that file,
commented, with the parameters to adjust.

An `fbtft` panel (ST7789, ILI9341) presents a real `/dev/fb0`, and a
`matrix-keypad` overlay presents a real evdev node — so the device backend needs
no changes for either, only the right overlay lines.

## Status

The image builds and is structurally sound: three partitions, the kernel and Pi
firmware on the boot partition, QDOS and its libraries on the rootfs, init
configured to launch it.

It has never been booted. There is no Pi yet, and nothing here has been verified
against real hardware — the display, the keypad and the boot itself are all
unknowns until one exists.
