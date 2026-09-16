# QDOS

An operating system for a homebuilt calculator, shipped as firmware. The OS is C;
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
flow, definitions, strings; `Escape` returns to the calculator.

```
6  Enter  7  *                       ->  42
:  fn sq(x:i64 -- r:i64) { dup * }   Enter    saved 'sq'
:  7 sq                              Enter    -> 49
```

`Tab` completes words. F1-F5 are soft keys, labelled on the bottom row of the
display. `lst` browses installed programs, `edit` opens one, `check` compiles
without saving, `forget` removes one. Programs load from three scopes — system,
inbox (`.qd` files uploaded over USB or on the card), user — and a user copy
shadows the others. The stack, registers and declared words survive a power
cycle.

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
