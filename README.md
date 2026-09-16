# QDOS

An operating system for a homebuilt calculator, shipped as firmware. The OS is C
— bar one file that reads Quadrate's syntax tree, which has no C interface — and
the shell is [Quadrate](https://github.com/quadrate-language/quadrate).

Target: **Raspberry Pi Zero W**, 400x240 Sharp Memory LCD, 5x10 keypad.
See [docs/design.md](./docs/design.md) for why it is built this way.

## Building

Requires Meson, Ninja, a C++20 compiler, SDL3 (simulator only), and the Quadrate
source tree built alongside this one.

```bash
git clone https://github.com/quadrate-language/quadrate
cd quadrate && make release

cd ../qdos
meson setup build
meson compile -C build
./build/qdos                    # SDL3 simulator
```

Dependencies: `pacman -S meson ninja gcc sdl3` or
`apt install build-essential meson ninja-build libsdl3-dev`.

Options:

```
-Dquadrate_src=PATH     Quadrate source tree (default ../quadrate)
-Dquadrate_dist=PATH    dist directory inside it (default dist)
-Dsim=true|false        SDL3 simulator backend (default true)
-Ddevice=true|false     Linux framebuffer/evdev backend (default true)
-Dstatic=true|false     Link statically, no runtime dependencies (default false)
```

## Using it

Two modes. **Calculator** (`>`) is RPN: digits build a number, `Enter` pushes it,
an operator applies immediately. **Line** (`:`) takes whole Quadrate — control
flow, definitions, strings. `Escape` empties the line, and takes you back to the
calculator once it is empty.

```
6  Enter  7  *                       ->  42
:  fn sq(x:i64 -- r:i64) { dup * }   Enter    saved 'sq'
:  7 sq                              Enter    -> 49
```

`Tab` completes words. F1-F5 are soft keys, labelled on the bottom row of the
display; the last of them is the angle mode, and reads `DEG` or `RAD` rather
than naming itself — it is the one setting that changes an answer without
saying so, and a label that is the setting is already the annunciator. `STO`
and `RCL` are shifted keys on the stack row and take the digit after them,
reaching registers 0 to 9; the other ninety are `n sto` and `n rcl` written
out. Where the keypad has more than one face, which one is live shows beside
the prompt (`:A `), because a keycap cannot light up.

`lst` browses installed programs, `edit` opens one, `check` compiles without
saving, `forget` removes one. Programs load from three scopes — system, inbox
(`.qd` files uploaded over USB or on the card), user — and a user copy shadows
the others. The stack, registers and declared words survive a power cycle.

A line that will not evaluate stays on the input to be corrected rather than
being thrown away: on a keypad with no letters of its own, retyping it is the
expensive part. Infix is refused instead of run — `5 - 3` is valid Quadrate that
pushes 5, subtracts it from whatever the stack was already holding, and pushes
3, which is a wrong answer with nothing to report it, so the shell says
`RPN: TRY 5 3 -` and evaluates nothing.

`check` does more than parse. Declaring a word in Quadrate does not resolve the
names in its body: the interpreter looks each one up as it runs, so a typo in a
branch that is never taken waits there until the day it is. After declaring the
editor's text into a throwaway interpreter, `check` walks the body the same way
the interpreter walks it and reports the first thing it would refuse — a word
that is not in the vocabulary, or a construct the interpreter has no answer for
(locals, `for`, structs; the language has them, this tier does not). One
finding at a time, because there is one line to say it on.

## Testing

```bash
meson test -C build                     # Full suite
./tests/run-device-tests.sh             # Exercise /dev/uinput and /dev/fb0 for real
./cross/run.sh                          # Cross-compile ARMv6, run tests under QEMU
QDOS_ARCH=aarch64 ./cross/run.sh        # ... and ARM64
```

## Running on hardware

`QDOS_BOARD` selects the target: `zerow` (default, ARMv6) or `zero2w` (ARMv8).

```bash
./firmware/stage-to-raspios.sh pi@host  # Bring-up: onto Raspberry Pi OS Lite
./firmware/build-image.sh               # Firmware: a Buildroot sdcard.img
QDOS_BOARD=zero2w ./firmware/run-qemu.sh   # Boot under QEMU (zerow does not)

sudo dd if=~/.cache/qdos-firmware/build-zerow/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
```

See [firmware/README.md](./firmware/README.md) for what the image contains.
`QDOS_FB`, `QDOS_INPUT`, `QDOS_STORE` and `QDOS_INBOX` override the device paths.

## Status

Early. The simulator runs; it cross-compiles to ARMv6 and ARM64 and passes its
suite under emulation. It has never run on a Pi.

## Links

- **Quadrate**: https://github.com/quadrate-language/quadrate
- **Language documentation**: https://quad.r8.rs
- **Issues**: https://github.com/quadrate-language/qdos/issues

## License

GNU General Public License v3.0 — see [LICENSE](./LICENSE)

SPDX-License-Identifier: GPL-3.0-or-later

QDOS links Quadrate's `lib/qc`, which is GPL-3.0, so the whole is GPL-3.0.
