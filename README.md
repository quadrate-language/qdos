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

### Using it

Two modes, because a calculator is operated by reflex and a language is typed
into deliberately.

**Calculator** (`>`). Digits build a number; `Enter` pushes it; an operator
applies immediately:

```
6  Enter  7  *        ->  42
7  Enter  Enter  *    ->  49   (bare Enter duplicates, as on an HP)
```

**Line** (`:`). Press `:` to type whole Quadrate — control flow, stored
registers, and function definitions. `Enter` evaluates and you stay in line
mode, so a word can be defined and then used. `Escape` returns to the
calculator.

A line with an open brace or paren continues rather than evaluating, so a
definition can be typed the way it reads. The prompt becomes `..` while one is
open:

```
:  fn sq(x:i64 -- r:i64) {   Enter
..   dup *                   Enter
..  }                        Enter     defines
:  7 sq                      Enter     -> 49
:  Escape
```

Braces inside a string are text, not structure, and an unmatched closer submits
so the parser can report it.

Backspace edits, F10 powers off.

On the device the evdev mapping covers a full keyboard, so line mode is usable
with a USB keyboard before the machine has its own keys. Set `QDOS_KEYMAP` to
`us` or `se`: evdev reports which key was pressed, not what it is labelled, and
on a Swedish keyboard the braces a function needs are on AltGr.

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
./firmware/run-qemu.sh                  # Boot that image under QEMU, no hardware needed
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
"name" forget       remove a Quadrate-defined word
cls                 clear the message line
```

Registers persist through the HAL, and the stack itself is saved on power-off
and restored on start, so the machine comes back holding what it held.

Control flow works — `if`/`else`, `loop`, `break`, `continue` — with a step
budget, because nothing can interrupt a running evaluation on a calculator and
an unbounded `loop {}` would otherwise need a power cycle.

Quadrate-defined functions work: declare one in line mode and call it from
either mode, and `"name" forget` removes one again. Recursion is bounded, since
each level costs a C stack frame.

The keypad has two modes: pressing an operator evaluates immediately, and `:`
switches to typing whole lines of Quadrate.

Not yet: `for`, named locals (both `for i` and named parameters need a variable
scope), and routing `print` output to the display instead of stdout.

## Links

- **Quadrate**: https://github.com/quadrate-language/quadrate
- **Language documentation**: https://quad.r8.rs
- **Issues**: https://github.com/quadrate-language/qdos/issues

## License

GNU General Public License v3.0 — see [LICENSE](./LICENSE)

SPDX-License-Identifier: GPL-3.0-or-later

QDOS links Quadrate's `lib/qc`, which is GPL-3.0, so the whole is GPL-3.0.
