# QDOS

An operating system for a homebuilt calculator, shipped as firmware. The OS is C; the shell is Quadrate.

> Target: **Raspberry Pi Zero 2 W** (ARMv8 / Cortex-A53). See [docs/design.md](./docs/design.md) for why it is built this way.

## Building

Requires: Meson, Ninja, a C++20 compiler, SDL3 (simulator only), and the Quadrate
source tree built alongside this one.

```bash
git clone https://github.com/quadrate-language/quadrate
cd quadrate && make release

cd ../qdos
meson setup build
meson compile -C build
./build/qdos                    # SDL3 simulator
```

Escape clears, F10 powers off, everything else types.

### Platform dependencies

**Arch Linux**: `pacman -S meson ninja gcc sdl3`

**Debian/Ubuntu**: `apt install build-essential meson ninja-build libsdl3-dev`

### Build options

```
-Dquadrate_src=PATH     Quadrate source tree (default ../quadrate)
-Dquadrate_dist=PATH    dist directory inside it (default dist)
-Dsim=true|false        SDL3 simulator backend (default true)
-Ddevice=true|false     Linux framebuffer/evdev backend (default true)
-Dstatic=true|false     Link statically, no runtime dependencies (default false)
```

## Project structure

```
include/qdos/       Public interfaces (HAL, shell)
src/shell/          Input line, stack display, evaluation via Quadrate's lib/interp
src/ui/             Framebuffer text console and 8x8 font
src/hal/sim/        SDL3 backend (develop on the desktop)
src/hal/device/     Pi backend (framebuffer + evdev)
cross/              ARM64 cross-compilation (container, meson cross file, runner)
firmware/           Buildroot image and Raspberry Pi OS staging
tests/              Test suite
docs/               Design notes
```

## Testing

```bash
meson test -C build                     # Full suite
./build/tests/test_console --dump       # Print the font as pixel art
./tests/run-device-tests.sh             # Exercise /dev/uinput and /dev/fb0 for real
./cross/run.sh                          # Cross-compile ARM64, run tests under QEMU

# ARM's char is unsigned where x86's is signed; this catches the difference here
meson setup build/uchar -Dc_args=-funsigned-char -Dcpp_args=-funsigned-char
meson test -C build/uchar
```

## Running on hardware

```bash
./cross/run.sh                          # Build for Cortex-A53, test under emulation
./firmware/stage-to-raspios.sh pi@host  # Bring-up: onto Raspberry Pi OS Lite
./firmware/build-image.sh               # Firmware: a Buildroot sdcard.img
```

Flash the image with:

```bash
sudo dd if=~/.cache/qdos-firmware/build/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
```

See [firmware/README.md](./firmware/README.md) for what the image contains and
which path to use when.

`QDOS_FB`, `QDOS_INPUT` and `QDOS_STORE` override the device paths, so the binary
can be pointed at whatever the board enumerates without rebuilding.

An SPI panel driven by `fbtft` (ST7789, ILI9341) presents a genuine `/dev/fb0`,
which is what this backend wants. A GPIO matrix behind the `matrix-keypad` device
tree overlay produces a real evdev node. See
[docs/design.md](./docs/design.md#running-on-hardware) for the trade-offs.

## Status

Early. The simulator runs and evaluates arithmetic, stack, comparison and bitwise
instructions against a stack that persists across lines, with errors displayed
rather than fatal. Cross-compiles to ARM64 and passes its suite under emulation;
the evdev and framebuffer paths are verified against real kernel devices.

It has never run on a Pi.

The machine's capabilities reach typed Quadrate as native words:

```
value slot sto      store in a numbered register (0-99), any type
slot rcl            recall it
slot clr            empty it
cls                 clear the message line
```

Registers persist through the HAL, and the stack itself is saved on power-off
and restored on start, so the machine comes back holding what it held.

Not yet: control flow (`if`, `loop`), Quadrate-defined functions, variables, and
routing `print` output to the display instead of stdout.

## Links

- **Quadrate**: https://github.com/quadrate-language/quadrate
- **Language documentation**: https://quad.r8.rs
- **Issues**: https://github.com/quadrate-language/qdos/issues

## License

GNU General Public License v3.0 — see [LICENSE](./LICENSE)

SPDX-License-Identifier: GPL-3.0-or-later

QDOS links Quadrate's `lib/qc`, which is GPL-3.0, so the whole is GPL-3.0.
