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

`Tab` completes the word being typed, against everything the interpreter
knows -- builtins, the native words below, and anything you have declared:

```
:  wob  Tab                      -> wobble, if that is the only match
:  d    Tab      dup drop ...    -> types the shared prefix, lists the rest
```

An ambiguous prefix still types the part every match shares, so `Tab` is never
a wasted keystroke. It only fires on words: after an operator or a number there
is nothing to complete.

Backspace edits, F10 powers off.

A word declared in line mode is written to storage as it is defined, so it is
still there after a power cycle:

```
:  fn sq(x:i64 -- r:i64) { dup * }   Enter    saved 'sq'
   ... reboot ...
:  7 sq                              Enter    -> 49
```

`"sq" forget` removes it from storage as well as from the session.

Programs can also be uploaded without typing them. Mount the card's first
partition — FAT, so any machine can write to it — and drop a `.qd` file in the
`qdos/` directory:

```
/boot/qdos/sq.qd     fn sq(x:i64 -- r:i64) { dup * }
```

QDOS copies it into the writable store at boot, after which it behaves like a
word declared on the calculator itself. A file that does not parse is skipped
rather than fatal: one bad upload should not cost the user their calculator.

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
src/ui/             Framebuffer text console and 16x24 font
src/hal/sim/        SDL3 backend (develop on the desktop)
src/hal/device/     Pi backend (framebuffer + evdev)
cross/              ARM cross-compilation (container, meson cross files, runner)
firmware/           Buildroot image and Raspberry Pi OS staging
tests/              Test suite
docs/               Design notes
```

## Testing

```bash
meson test -C build                     # Full suite
./build/tests/test_console --dump       # Print the font as pixel art
./tests/run-device-tests.sh             # Exercise /dev/uinput and /dev/fb0 for real
./cross/run.sh                          # Cross-compile ARMv6, run tests under QEMU
QDOS_ARCH=aarch64 ./cross/run.sh        # ... and ARM64

# ARM's char is unsigned where x86's is signed; this catches the difference here
meson setup build/uchar -Dc_args=-funsigned-char -Dcpp_args=-funsigned-char
meson test -C build/uchar
```

## Running on hardware

Two targets, selected with `QDOS_BOARD`. They share everything but the
architecture, kernel config and device tree.

| `QDOS_BOARD` | Board | SoC | QEMU machine |
|---|---|---|---|
| `zerow` (default) | Pi Zero W | BCM2835, ARM1176, ARMv6 | `raspi0` — does not boot |
| `zero2w` | Pi Zero 2 W | BCM2710A1, Cortex-A53, ARMv8 | `raspi3b` |

QEMU's `raspi0` emulation does not match a Zero W closely enough to boot it.
`firmware/qemu/make-qemu-dtb.sh` patches the device tree far enough to get the
card enumerated and the partitions read, and then the guest freezes with QEMU
pegged at 100% -- the guest clock stops, so even a plain `rootdelay` sleep never
returns. `run-qemu.sh` says so rather than showing a black screen.

So the Zero W is verified two other ways: `./cross/run.sh` builds it as ARMv6
and runs the whole suite under `qemu-arm`, and the `zero2w` target boots the
same userspace in 25 seconds. Only the kernel and the architecture differ.

```bash
./cross/run.sh                          # Build for the target, test under emulation
./firmware/stage-to-raspios.sh pi@host  # Bring-up: onto Raspberry Pi OS Lite
./firmware/build-image.sh               # Firmware: a Buildroot sdcard.img
./firmware/run-qemu.sh                  # Boot that image under QEMU, no hardware needed

QDOS_BOARD=zero2w ./firmware/build-image.sh   # the other target
QDOS_BOARD=zero2w ./firmware/run-qemu.sh
```

Each board builds into its own output tree, so switching targets never
rebuilds the other one.

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

Declared words persist too, as plain `.qd` source in the same store, replayed
at boot. That the stored form is source rather than an image is what lets a
program be uploaded from a PC: a file dropped on the boot partition and a word
typed on the keypad are the same thing by the time the shell sees them.

Control flow works — `if`/`else`, `loop`, `break`, `continue` — with a step
budget, because nothing can interrupt a running evaluation on a calculator and
an unbounded `loop {}` would otherwise need a power cycle.

Quadrate-defined functions work: declare one in line mode and call it from
either mode, and `"name" forget` removes one again. Recursion is bounded, since
each level costs a C stack frame.

The keypad has two modes: pressing an operator evaluates immediately, and `:`
switches to typing whole lines of Quadrate, where `Tab` completes words.

The panel is 400x240 -- a Sharp Memory LCD (LS027B7DH01), reflective and
instant rather than backlit -- and only 2.7 inches, so its active area is
58.8 x 35.3mm. That is small enough that the font had to be drawn for it rather
than scaled up: `font16x24.c` is 95 glyphs with a 16 row cap height, 11 row
x-height, 2px stems and even bearings, giving a 25x10 console where a capital
is 2.4mm tall.

Everything is drawn at that one size, which leaves five stack entries visible
above the input line. A 64-bit integer is 19 digits and fits across 25 columns
beside its index label.

The panel is one bit per pixel -- Sharp's datasheet calls it "internal 1bit
memory within the panel", and the kernel reduces the grayscale buffer to it
with a plain cut at 128 -- so there is no antialiasing to be had, and the font
is fitted to the pixel grid instead. At 173 DPI that is not the compromise it
sounds like: a pixel is 0.147mm against the ~0.175mm an eye resolves at arm's
length.

The simulator applies the same cut and the same reflective colours, so what it
shows is what the hardware shows, and a test asserts no pixel QDOS draws lands
near the threshold. It opens 1:1 by default, which on a typical monitor is
still about 1.6x life size; `QDOS_SIM_SCALE` enlarges it for inspecting pixels.

Below the panel it draws a 5x8 keypad you can click, for trying a layout before
wiring one. The table in `src/hal/sim/keypad_ui.c` is the whole layout, so
rearranging it is a one-file edit. A button either sends a logical key or types
text; only the logical keys and `:` reach calculator mode, which is the same
constraint a physical keypad with forty buttons would have.

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
