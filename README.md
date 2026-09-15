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

Programs come from three places, each a scope of its own:

| Scope | On the device | In the simulator | Override |
|---|---|---|---|
| system | `/usr/share/qdos/programs` | `programs/system` | `QDOS_SYSTEM_STORE` |
| inbox | `/mnt/inbox` | `qdos-inbox` | `QDOS_INBOX` |
| user | `/var/lib/qdos` | `qdos-store` | `QDOS_STORE` |

They load in that order, so a program of yours shadows an uploaded one, which
shadows one shipped in the firmware. Only the last is writable, and the HAL
enforces that by shape rather than by a check: `store_read` and `store_list`
take a scope, and `store_write` does not, so nothing else has a path to them.
The simulator does not seed your store, so to try the shipped user app there:
`cp programs/user/*.qd qdos-store/`.

`"name" edit` opens the program in an editor over the stack area -- seven lines
at a time of the real source, comments and layout intact, scrolling as the
cursor leaves the pane. Arrows move, `ent` splits a line, and the soft
keys do the rest.

`check` (F3) compiles what is on screen and says so, without saving and without
leaving the editor -- a syntax error is reported the same way a save would
report it, but you keep the text. It runs in a throwaway interpreter loaded with
the installed programs, because evaluating a source file both declares what
parses and runs whatever sits at top level, and neither belongs in the session
until you save. So a word checked and then dropped is not callable afterwards,
and the stack is as you left it.

A name with no program behind it starts a new one from a template rather than
failing, so `edit` is how you write a program as well as change one. Saving is
refused if the source does not parse, and you stay in the editor, since the
buffer is the only copy. Editing a shipped program writes your copy, leaving
theirs intact underneath.

`"name" forget` deletes your copy. If a shipped program of that name exists it
comes straight back, so forgetting means undoing your override and a shipped
program can never be lost. Forgetting one you never overrode says `is built in`.

Shipped today: `isqrt` and `hyp` (Pythagoras, in Quadrate, in
`programs/system/`). `hello` is installed as a user app to start you off.

`lst` (F1) browses the installed programs, marked `sys` or `user`, and `user*`
where one of yours overrides a shipped one. Arrows move, `ent` picks the name
into the input line, `clr` goes back. `tab` widens to every word the interpreter
knows -- useful, but the sixty builtins bury three apps, which is why the
programs get their own view, and why the widening is on `tab` rather than
spending one of the five function keys.

`Tab` completes the word being typed, against everything the interpreter
knows -- builtins, the native words below, and anything you have declared:

```
:  wob  Tab                      -> wobble, if that is the only match
:  d    Tab      dup drop ...    -> types the shared prefix, lists the rest
```

An ambiguous prefix still types the part every match shares, so `Tab` is never
a wasted keystroke. It only fires on words: after an operator or a number there
is nothing to complete.

Left and right move the cursor, so text is inserted where you are rather than
only appended. Backspace deletes to the left of it. F10 powers off.

A row of five soft keys is labelled along the bottom edge of the panel, right
above the function keys themselves, because what they do follows the mode: `f1` is always the way out, whatever
backing out means there -- `clr` cancels the number you are typing, `esc` leaves
line mode or the apps menu, `drop` abandons an edit. All four are the same key
underneath, only the label differs. The rest follow the mode: `apps` from the
calculator, `comp` in line mode, `down up pick edit` in the apps menu, `check`
and `save` in the editor. The apps menu is therefore navigable with nothing but the five function
keys, which matters if the built keypad ends up without arrows. They are F1 to F5 on a keyboard and the top keypad row. Five
labels across 25 columns is 80 pixels each, the same as a keypad button, so a
label sits squarely over the key it names. They cost a row of the display, which
is the price of the machine explaining itself rather than expecting the keys to
be remembered.

A word declared in line mode is written to storage as it is defined, so it is
still there after a power cycle:

```
:  fn sq(x:i64 -- r:i64) { dup * }   Enter    saved 'sq'
   ... reboot ...
:  7 sq                              Enter    -> 49
```

`"sq" forget` removes it from storage as well as from the session.

### Installing programs

Programs can also be uploaded rather than typed. The card's fourth partition is
the inbox: FAT, labelled `QDOS-INBOX`, so any machine can write to it. Drop a
`.qd` file at its root, one word per file, named after the word:

```
sq.qd     fn sq(x:i64 -- r:i64) { dup * }
```

Two ways to reach it:

- **Over USB.** Set `USB` to `SHARED` under SETTINGS and plug the data port into
  a PC; the inbox appears as a USB drive. Setting it back to `OFF` unmounts it
  from the host and declares whatever arrived, without a reboot. Chosen over a
  network service because it costs nothing while unplugged, and because the
  machine is on wall power at exactly the moment the feature is used.
- **In a card reader.** Pull the card and mount the inbox partition directly.

QDOS reads the inbox in place and never writes to it, so it is a scope of its
own rather than a copy. That means an uploaded program can be overridden by
editing it on the calculator — the APPS list marks the result `USER*` — and the
override survives every later boot. Deleting an upload for good means deleting
the file from the card, which is where it came from.

A file that does not parse is skipped rather than fatal: one bad upload should
not cost the user their calculator.

The APPS list says where each program came from:

| Mark | Where it lives |
|---|---|
| `SYS` | Shipped in the firmware, on the read-only rootfs |
| `CARD` | Uploaded, on the inbox partition |
| `USER` | Written on the calculator |
| `USER*` | Written here, covering a `SYS` or `CARD` copy underneath |

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
assets/fonts/       The font the glyphs are rasterised from
tools/              genfont.py and genlogo.py, which generate the font and
                    the kernel boot logo from it
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

# Coverage. Two tests reach for real device nodes and quietly skip without
# them, so check here rather than in the pass/fail count that they ran.
meson setup build/cov -Db_coverage=true && meson test -C build/cov
ninja -C build/cov coverage-text        # needs gcovr or lcov

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

`QDOS_FB`, `QDOS_INPUT`, `QDOS_STORE` and `QDOS_INBOX` override the device paths,
so the binary can be pointed at whatever the board enumerates without rebuilding.
`QDOS_USB_HELPER` names the script behind the USB setting; where it is missing
the setting is not offered at all.

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
cls                 take a message down without pressing a key
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
58.8 x 35.3mm. That is small enough that the font has to suit the pixel grid rather
than be scaled up to it: `src/ui/font16x24.c` is 95 glyphs baked in as bitmaps,
giving a 25x10 console where a capital is 2.4mm tall.

The glyphs come from "VCR OSD Mono" by Riciery Leal, rasterised by
`tools/genfont.py` at the largest size whose ink still fits a cell -- 27px, as
it happens. Thresholding happens at build time, so the firmware links no
rasteriser and the panel gets bitmaps. Point the generator at another font to
change it; the metrics are worked out rather than hard coded.

The kernel's boot logo is the same glyphs: `tools/genlogo.py` reads
`font16x24.c` rather than the TTF, so what the firmware paints before Linux has
a framebuffer driver cannot drift from what the shell paints after. Regenerate
both when the font changes.

Everything is drawn at that one size, which leaves eight stack entries visible
above the input line. A 64-bit integer is 19 digits and fits across 25 columns
beside its index label.

The eighth is there because a message has no row of its own. Errors and what a
program printed take over the input line while they are up, inverted if they are
errors, and the next keypress hands the line back with what you were typing
still on it. A row held permanently for something on screen one keypress in
twenty is a row wasted the other nineteen, and on a ten-row panel that is the
difference between seven stack entries and eight. It buys a row everywhere, not
just in the calculator: the apps list and the editor show seven lines rather
than six. `cls` takes a message down without pressing anything.

A rule is a single pixel along the bottom edge of a row, and the lowest ink in
the font is two pixels above that, so a rule underlines a row of content rather
than occupying a row of its own. Ten rows is not enough to spend two of them on
lines: the caption is underlined, the last row of content is underlined, and
both of those rows still hold text. That is two rows back, which is a third of
the pane in the list and the editor.

The calculator does not caption itself. A title row costs a stack entry to say
what the machine in your hand already is, and `1:` `2:` `3:` down the left edge
count the depth that a `DEPTH n` readout used to spell out, so the stack starts
at the top row and a ninth entry shows as `...` rather than scrolling away
unannounced. The apps menu and the editor keep their headers, which carry
something the pane below cannot: `APPS` or `WORDS` with the position in the
list, and the program name with `line:col`.

What QDOS says for itself is in capitals -- headers, soft key labels, keycaps,
status messages, the `SYS` and `USER` markers. Anything that is Quadrate stays exactly
as it is written: program names, the source in the editor, the line you are
typing, and the word a runtime error is complaining about. The language is
case-sensitive, so shouting a program name back at you would be a lie about
what you would have to type.

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

Below the panel it draws a clickable 5x10 keypad, for trying a layout before
wiring one. The table in `src/hal/sim/keypad_ui.c` is the whole layout, so
rearranging it is a one-file edit. Each button carries three actions, one per
layer, and `ALP` and `SYM` switch between them. Buttons with nothing on the
symbol layer dim rather than disappear.

It is arranged like a SwissMicros DM42, because a calculator is operated by
muscle memory and there is no reason to make ours a different shape. Keycaps are
capitals, being the machine's own lettering; what a key types is not, so `FN`
types `fn ` and `I64` types `i64`:

```
      plain                 alpha (locks)         symbol (one press)
F1  F2  F3  F4  F5
sin cos tan ln  log      A   B   C   D   E      (    )    {   }   "
 v  sq  pow inv abs      F   G   H   I   J      fn   --   i64 f64 str
FLR ceil RND SPC SYM     K   L   M   SPC        if   else loop BRK
dup drop over rot mod    N   O   P   Q   R      nip  UNDO [   ]   =
ENT swap +-  TAB BKS     ENT S   T   U   BKS    APP  roll free <  >
 ^   7   8   9  div      ^   V   W   X   Y      <-   and  or  xor shl
 v   4   5   6   x       v   Z   _   6   x      ->   not  ==  !=  ,
ALP 1   2   3   -        ALP 1   2   3   -           <=   >=  pi  INC
ESC 0   .   :   +        ESC 0   .   :   +      PWR  len  nth ;   DEC
```

(The caps shown as `^ v <- -> +- x div pi` and the root sign are drawn glyphs,
not those letters.)



Navigation runs down the left column, the digits sit in a 3x3 block, and the
operators run down the right in the DM42's order. `ESC` is its EXIT, and `PWR`
is on the symbol layer of that key exactly as OFF is shift-EXIT there, so the
machine cannot be switched off by a slip of the thumb. The DM42 has no left and
right arrows; ours are the symbol layer of `UP` and `DN`, which costs a press
in the editor and is the one place the resemblance is inconvenient. `:`
takes the R/S slot, being the key that runs something.

The numpad sits at the bottom of the pad with nothing under it, where a thumb
expects it, and the menu keys stay directly under the labels they answer to.

A cap in lower case is exactly the word it stands for, so `sin` can be typed as
it is printed; a cap in capitals is QDOS's own -- `FLR` and `RND` because
`floor` and `round` are five characters and would touch the bezel, `ENT` and
`ESC` because the language has no name for them. The case is the signal, and a
test enforces it: no cap mixes the two, and a lower-case one that types text
must type exactly itself. `log` is registered as a name for `log10` so that the
cap can be honest about it, which is what a calculator means by log anyway. The
alphabet is the exception, printing capitals and typing small, as every
keyboard does.

Three faces, because one modifier is not enough for a keypad this size -- the
same reason a TI-83 carries both `2nd` and `ALPHA`. Plain is a calculator:
trigonometry, roots and powers, rounding, the stack. `ALP` is the alphabet,
without which no program name, word or string can be typed at all. `SYM` is
Quadrate's syntax -- brackets, quotes, type names, control flow -- which cannot
be reached any other way. Anything that is an ordinary word is in the catalog
and needs no key at all, which is what leaves room for the other two.

`ALP` locks and `SYM` does not, which follows from what each is for: a name is
several letters and a bracket is one. `SYM` hands back to whichever layer was
showing, so it can be used in the middle of a locked word. The alpha layer only
replaces the buttons that carry a letter -- Enter, backspace, escape, the
arrows, the digits and the space all keep working underneath it, or locking
would strand you mid-word with no way to finish. A test asserts exactly that,
along with all 26 letters being present exactly once.

That is enough to write a program without touching a keyboard:
`: SYM fn ALP h i ALP SYM ( SYM -- SYM ) SYM { 3 SYM } Enter` defines
`fn hi( -- ) { 3 }` in nineteen presses.

`/` on the keypad is not Quadrate's `/`. The language divides integers into an
integer, so `22 7 / 100 *` is 300, which is the language being consistent and
the wrong answer on a calculator. The key applies `divide` instead, which gives
314.28..., and keeps the result an integer when it divides exactly so the
bitwise words still take it. Typed into a line, `/` is still the language's.
`pi` and `e` are registered, and the angle mode converts on the way in and out
of the trigonometry, because `sin 30` is asked for far more often than
`sin 0.5236`.

`print` and `nl` are built into the runtime and write to stdout, which on the
device goes nowhere at all. Evaluation runs with stdout on a pipe, so what a
program prints lands on the input line and in the debug log. It cannot be
done by registering a word: the runtime resolves these names itself and a
native of the same name is never called.

The maths comes from Quadrate's `lib/math`, which ships in the dist but is not
registered with the interpreter; `src/shell/mathwords.c` registers its 34
functions so they are words like any other. Integers coerce, so `9 sqrt` works
without a decimal point. A word of yours shadows a built-in of the same name,
and forgetting yours brings the built-in back.

Nothing typed into the shell can end it. Quadrate treats a runtime fault as
fatal -- `1.5 2.5 and`, a string where a number was wanted, `ln` of a negative
-- and printed a stack dump before exiting, taking the calculator and the
stack with it. `lib/rt` has a recovery mechanism for this, armed with
`qd_recovery_arm()` and a `setjmp`, but only some paths went through the helper
that honours it; 128 sites across `lib/rt` and `stdlib/math` reported and
exited directly instead. Those now raise through the same helper, so an
embedder that has armed recovery survives them. Every evaluation in QDOS is
armed, including the one that loads stored programs at boot, so a bad upload
cannot stop the shell starting. Driving all 99 words against 14 stack shapes
through the shell ends the process in no case, where before the change it ended
in 69.

`lib/math` treats a domain error as fatal: `ln` of a negative prints a stack
dump and aborts the process. That is reasonable for a program and fatal for a
calculator, where pressing `LN` four times in a row is enough to get there --
5 becomes 1.609, then 0.476, then -0.742. Eleven of the thirty-four have a
domain to fall out of (`sqrt`, `ln`, `log10`, `log2`, `asin`, `acos`, `acosh`,
`atanh`, `inv`, `fac`, `fmod`), so those check the argument first and raise an
ordinary error instead, leaving the value on the stack. Anything they let
through is still `lib/math`'s to compute. Which eleven was settled by running
every function against out-of-range inputs in a forked child and recording
which ones did not come back, rather than by reading the domains off a
textbook; `tests/test_mathwords.c` keeps them honest.

`INFO` (F4 from the calculator) shows what the firmware is: version, the git
commit it was built from, and the panel. `SET` and `LOG` sit behind it.
Settings holds the angle mode and how many decimals a number shows -- `AUTO`
being however many the interpreter renders, which is fifteen more often than
anyone wants. A fixed setting applies to whole numbers as well, since the point
of fixing it is a column of figures that line up; text is left as it is. The debug page is the log: everything a program printed and every
error, oldest first, scrollable, and clearable with `CLR`. The version comes from `meson.build`
and the commit from `git describe --dirty` at configure time, generated into
`qdos_version.h`, so neither can be written down twice and drift; a tree without
git reports `unknown` rather than failing to build.

`CAT` opens the catalog, which is the TI-83's answer to a keyboard that cannot
hold every function: all 102 words, alphabetical, and picking one types it into
the line. Typing a letter jumps to it, since four rows at a time of 102 is not
something to scroll through. The function keys are logical keys rather than
typed text, so they apply to the stack in calculator mode -- `45` `SQRT` --
which is the whole point of putting them on the front layer.

A button either sends a logical key or types text; only the logical keys and `:`
reach calculator mode, which is the same constraint a physical keypad would have.

Backspace in the apps list drops the selected program, asking once first; a
shipped one is refused, there being no way to put it back. `UNDO` (symbol layer
of `DRP`) puts back what the last operation consumed, one step, which is all a
calculator ever offers.

`DUP DRP OVR ROT SWP` are the stack, and `ROT` is the only way to reach the
third entry: without it the keypad can see two deep and no further. Like `NEG`
they are logical keys, not typed text -- a test walks the default layer and
fails on any button that types, because such a button does nothing in the mode
the layer exists for. Only a space and `:` are allowed to, the first meaning
nothing to a calculator and the second being how you leave it.

`NEG` is a logical key rather than typed text, because it is the only way to
enter a negative number in calculator mode: `-` there is subtraction, so `5` `-`
is an operator with one operand and nothing to take from. It works as `+/-` does
on an HP -- while a number is being typed it flips that number's sign, and with
nothing being typed it negates x.

The typeface is ASCII and nothing else, so the arrows, the root sign, divide,
times, plus-or-minus and pi are drawn by hand in `tools/genfont.py` rather than
taken from a font. VCR OSD Mono has arrows, but its vertical ones are outlines
that flare the wrong way at this size, and it has none of the mathematics. At
16x24 and one bit a stroke either lands on the grid or smears, so drawing them
is the lesser of the two jobs. They sit below space, where ASCII has nothing
printable, which keeps a keycap a plain C string and `qdos_font_row()` taking a
`char`. A test walks them and fails on any that has no ink, since a blank key
is not something the eye catches.

`SPC` earns its place because two numbers typed in a row would otherwise merge:
`7` `8` `+` is the single token `78` followed by an operator with nothing to add
it to. Operators and words need no separator -- the lexer breaks on the former,
and every word button types its own surrounding spaces -- so a space is only
ever needed between literals. ` depth ` gave up the slot, being already on the
symbol layer of `ROT`.

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
