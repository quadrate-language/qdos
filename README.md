# QDOS

An operating system for a homebuilt calculator, shipped as firmware. The OS is C
— bar one file that reads Quadrate's syntax tree, which has no C interface — and
the shell is [Quadrate](https://github.com/quadrate-language/quadrate).

Target: **Raspberry Pi Zero W**, 400x240 Sharp Memory LCD, 54-key keypad.
See [docs/design.md](./docs/design.md) for why it is built this way, and
[docs/hardware.md](./docs/hardware.md) for the parts it is built from.

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
:  fn sq(x:i64 -- r:i64) { x x * }   Enter    declared 'sq'
:  7 sq                              Enter    -> 49
```

A word written at the prompt lives in memory and goes with the power. The card
holds programs, and they get there by `edit` or by being uploaded — scratch
work at the prompt is not that, and would otherwise fill the store with every
`sq` ever tried, each one a row in `APPS`.

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
(uploaded over USB or on the card), user — and a user copy shadows the others.
The stack and the registers survive a power cycle.

## Apps

A folder on the card is an app, named after the folder.

```
doom/
	main.qd        fn main( -- )   the entry point
	libdoom.so     doom::start, doom::tick, doom::press
	doom1.wad
```

`APPS` lists it as `doom` and typing `doom` runs its `main`. What is in the
folder belongs to it: the module loads but gets no row of its own, and the data
it brought is found through `api->path()` without the app knowing where the
card put it. Renaming the folder renames the app, and deleting it deletes the
whole thing.

An app runs in an interpreter of its own, so every one of them can call its
entry point `main` and name its helpers whatever suits it without two of them
ever meeting; nothing it declares is left in the vocabulary afterwards.

Loose files at the top level are what they always were. A `.qd` is a library of
words, declared at boot and listed by the words it brings. A `lib*.so` is a
shared library any program may call, listed as `foo::` — or, if it has a
`main()`, a program taking its bare name.

A line that will not evaluate stays on the input to be corrected rather than
being thrown away: on a keypad with no letters of its own, retyping it is the
expensive part. Infix is refused instead of run — `5 - 3` is valid Quadrate that
pushes 5, subtracts it from whatever the stack was already holding, and pushes
3, which is a wrong answer with nothing to report it, so the shell says
`RPN: TRY 5 3 -` and evaluates nothing.

## Native modules

A shared object on the card is either of two things, or both.

A **library** offers words. They are scoped by the file, so `libfoo.so` gives
you `foo::triple`, and `APPS` lists it as `foo::`.

A **program** offers `main()`. It takes the name on its own — `liblife.so` is
listed as `life` and run by typing `life` — and while it runs it holds the
screen and the keypad, handing both back when `main()` returns. There is no
graphics mode to ask for: the panel is 400x240 pixels and always was, and the
text the shell draws is only another thing written into them.

```c
static int life_main(qdos_native_ctx* ctx, const qdos_native_api* api) {
	int w, h;
	uint8_t* canvas = api->canvas(ctx, &w, &h);   /* one byte per pixel */

	while (api->running(ctx)) {
		/* ... draw into canvas ... */
		api->present(ctx);

		qdos_key key;
		char ch;
		while (api->key(ctx, &key, &ch))
			if (key == QDOS_KEY_CLEAR)
				return 0;

		api->wait(ctx, 20);
	}
	return 0;
}
```

[examples/native/life.c](./examples/native/life.c) is a working one: Conway's
Life at one cell per pixel, which is what a 400x240 one-bit panel is already
shaped like. Build it and type `life`.

The library half looks like this:

```c
#include <qdos/native.h>

static int triple(qdos_native_ctx* ctx, const qdos_native_api* api) {
	int64_t n;
	if (api->pop_int(ctx, &n) != 0) {
		api->fail(ctx, "triple: NEED A NUMBER");
		return 1;
	}
	return api->push_int(ctx, n * 3);
}

static const qdos_native_word WORDS[] = {
	{"triple", "(n:i64 -- r:i64)", triple},
};

const qdos_native_module qdos_module = {
	QDOS_NATIVE_HEADER, WORDS, sizeof(WORDS) / sizeof(*WORDS), NULL, NULL,
};
```

```bash
./cross/build-module.sh foo.c                    # Zero W  -> libfoo.so
QDOS_ARCH=aarch64 ./cross/build-module.sh foo.c  # Zero 2 W
```

Then `foo::triple` is callable from the line, from a stored program, and from
the catalog, and `Tab` completes its words. The signature is what the
interpreter checks before the call, so a word is never entered without its
arguments.

Drawing is on both halves of the API, but only one of them can use it for much:
a word may plot a graph, while anything touching all 96,000 pixels a frame has
to be the program half. The interpreter cannot do a frame.

It can drive one, though. An app can ask the machine about itself —
`qdos::key ( -- key:i64 ch:i64 got:i64)`, `qdos::running ( -- r:i64)` and
`qdos::ticks ( -- ms:i64)` — so the loop and the controls can live in Quadrate
on the card while a library does the per-pixel work. DOOM is built that way:
`doom/libdoom.so` offers `doom::start`, `doom::tick` and `doom::press`, and
`doom/main.qd` holds the loop and the key table, editable on the calculator
itself.

A module includes that one header and libc — no Quadrate, no QDOS, nothing to
link against. Everything it may call arrives in a table at call time, so a
module already built keeps working when either of them moves underneath it, and
it exports exactly one symbol. [examples/native/bits.c](./examples/native/bits.c)
is a working one: `bits::hex`, `bits::pop` and `bits::rev`, which is the base
the display has no key for.

A module that will not load is listed with the reason rather than dropped —
`BUILT FOR aarch64`, `ABI 2, WANTED 1`, `WILL NOT LOAD`. And because native code
can fault where Quadrate cannot, a module is noted in the store while it is
being opened: one still noted at the next start is left alone and reported,
since init respawns the shell and opening it again would be a loop with no way
out. `SETTINGS` grows a `MODULES` row while anything is blocked, and changing it
lets them all back in.

This makes the card executable: anything that can write it runs code as the
shell. That is fine for your own machine and worth knowing before lending it.

`check` does more than parse. Declaring a word in Quadrate does not resolve the
names in its body: the interpreter looks each one up as it runs, so a typo in a
branch that is never taken waits there until the day it is. After declaring the
editor's text into a throwaway interpreter, `check` walks the body the same way
the interpreter walks it and reports the first thing it would refuse — a word
that is not in the vocabulary, or a construct the interpreter has no answer for
(`defer`, structs and their methods, anonymous functions, imports; the language
has them, this tier does not). One finding at a time, because there is one line
to say it on.

That list shortens as the interpreter grows, and shortening it is not optional:
naming a construct the machine now runs costs the user a program that would
have worked, with no way past the refusal from the keypad.

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
./firmware/upload.sh hello.qd ../qdos-doom/doom  # Onto the inbox, as a PC would
QDOS_BOARD=zero2w ./firmware/run-qemu.sh   # Boot under QEMU (zerow does not)

sudo dd if=~/.cache/qdos-firmware/build-zerow/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
```

See [firmware/README.md](./firmware/README.md) for what the image contains.
`QDOS_FB`, `QDOS_INPUT`, `QDOS_STORE` and `QDOS_INBOX` override the device paths.

`upload.sh` writes the same FAT partition the USB gadget hands to a PC, so it
is the same act as dropping a file, or an app's folder, on the drive that
appears when the machine is plugged in — minus the gadget, which needs a USB
device controller and so needs the board. The simulator has the other half: sharing the card there makes
QDOS stop reading `qdos-inbox/` and hands it to you, and taking it back reads
it afresh, which is the whole of what the shell has to cope with either way.

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
