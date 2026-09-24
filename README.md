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
flow, definitions, strings. `MODE`, the leftmost soft key, goes between the two.
`Escape` empties the line, and takes you back to the calculator once it is
empty. The `π` key pushes pi, or types it into a line; shifted, it does the same with e.

```
6  Enter  7  *                          ->  42
MODE  fn sq(x:i64 -- r:i64) { x x * }   Enter    declared 'sq'
      7 sq                              Enter    -> 49
```

The `÷` key divides as a calculator does, `7 2 ÷` being 3.5, wherever it is
pressed: it types `divide` into a line. Quadrate's own `/` keeps its meaning, 3
for `7 2 /`, so a program behaves on the calculator as it does on a PC.
The `%` key is `modulo`, a calculator's mod: it takes decimals and has the
sign of the divisor, `-7 3 %` being 2 where Quadrate's `mod` gives -1.
Likewise the keypad's `+`, `−` and `×` are `plus`, `minus` and `times`: whole
numbers stay whole until a result will not fit 64 bits, and then it is a
float, where Quadrate's own operators wrap as C does. `sq`, `cb`, `abs` and
`fac` do the same, so `21 fac` is 5.1e19. A number typed on the keypad that is
too big for an integer is a float too. From the keypad, a result with no
finite value is refused as `OVERFLOW` or `UNDEFINED` and what it was worked
out from stays on the stack. In degrees, whole quarter turns are exact:
`180 sin` is 0 and `90 tan` is `UNDEFINED`.
Parameters, `-> name` locals and `for` loops work at the prompt as in a program;
a local bound at the prompt lasts until the next restart. What an evaluation
prints goes to the message row, or, when it is more than a line, to a page of
its own.

A word written at the prompt lives in memory and goes with the power. The card
holds programs, and they get there by `edit` or by being uploaded — scratch
work at the prompt is not that, and would otherwise fill the store with every
`sq` ever tried, each one a row in `APPS`.

`Tab` completes words. F1-F5 are soft keys, labelled on the bottom row of the
display. The angle mode, degrees or radians, is on the settings page, and the
status band shows which is in force beside the clock. `STO`
and `RCL` are keys of their own beside the digits and take the digit after them,
reaching registers 0 to 9; the other ninety are `n sto` and `n rcl` written
out. Where the keypad has more than one face, which one is live shows beside
the prompt (`:A `), because a keycap cannot light up.

`lst` browses installed programs, `edit` opens one, `check` compiles without
saving, `forget` removes one. Programs load from three scopes — system, inbox
(uploaded over USB or on the card), user — and a user copy shadows the others.
The stack and the registers survive a power cycle.

`PLOT`, the rightmost soft key, is the Y= page, as on a TI-83: six slots, `Y1` to
`Y6`, each the body of a function in x typed like any line (`x sin x *`),
with `x`, `y`, `t` and `theta` on soft keys while it is being typed. A
slot is declared as a word of its name, so `2 Y1` works at the prompt too, and
the slots are kept across a restart. `ON` picks which ones `GRAPH` draws --
together, solid, dashed and dotted -- and in trace, up and down go from one
curve to the next. A body that uses `y` is a surface, and `GRAPH` on it draws
it in 3D. One that uses `t` is parametric and leaves x and y (`t cos t sin`);
one that uses `theta` is polar and leaves r. `DEL` empties a slot.

`GRAPH` from Y= draws in a window of its own, kept across a restart. `MENU`
has the rest: `WINDOW` types its edges, tick spacing and the t and theta
ranges; `ZOOM`; `TABLE`, the functions down a column of x from `START` in
steps of `STEP`; `FORMAT`, the grid and the axes; and `STAT` and `STAT PLOT`.

On the graph, the arrows pan and `+` and `-` zoom. `ZOOM` offers box, in,
out, standard, fit, decimal and integer (a column on every 0.05, or every
whole number, so trace lands on round values), square (a unit as long across
as down), trig and stat. `TRACE` follows a curve with x and y read out under
it; a number typed while tracing is where it goes. `CALC` finds a value,
zero, minimum, maximum, intersection, dy/dx, integral (shaded) or tangent,
asking for the bounds as a TI does -- move the cursor and press `ENTER`, or
type the x -- and puts the answer on the calculator's stack: x for a zero, x
and y for an extremum or intersection, a and b for a tangent y = ax + b.

`"f" graph` plots a word that takes x and leaves y, one sample per pixel
column, with y scaled to fit, in the standard window of -10 to 10. The
calculator's stack is left as it was. The same searches are words, for any
word taking x and leaving y:

```
"f" a b root        x where f is 0         "f" x nderiv       f'(x)
"f" a b fmin        x where f is lowest    "f" a b fnint      the integral
"f" a b fmax        x where f is highest   "f" "g" a b intersect
```

`STAT` is the lists, `L1` to `L6`, three across, typed down like a
spreadsheet column; `DEL` removes a value, `CLR` twice empties the list.
They are words too: `L1 mean`, `[1 2 3] 2 lsto`. `CALC` there gives 1-Var
and 2-Var statistics and linear, quadratic, exponential, power and log
regressions on the stat plot's lists; `PUSH` puts the selected result on the
stack and `TO Y` the fitted equation in the first empty slot. `STAT PLOT`
draws them over the curves as a scatter plot, a joined line, a histogram
(bars the x scale wide) or a box plot.

The words work on any list of numbers: `sum`, `mean`, `median`, `stdev`,
`pstdev`, `corr`, and `linreg` (xs ys -- a b) and its siblings `quadreg`,
`expreg`, `pwrreg` and `lnreg`. For counting and chance there are `ncr`,
`npr`, `normalpdf` (x mu sigma), `normalcdf` (lo hi mu sigma), `invnorm` (p
mu sigma), `binompdf` and `binomcdf` (n p k), `rand` and `randint` (lo hi).

`"f" graph3` plots a word that takes x and y and leaves z, as a wireframe over
-10 to 10 on both, sampled once on a 24x24 grid. The arrows turn and tilt it
and `+` and `-` zoom; none of that runs the word again, so it turns as fast as
it can be drawn. Hidden lines go by painting the cells back to front, filled.

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
