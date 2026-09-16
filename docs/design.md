# QDOS design notes

Why QDOS is built the way it is. See [README](../README.md) for building and
running.

The OS is C. The shell is [Quadrate](https://quad.r8.rs) — a stack language, on a
device whose natural input model is RPN. The calculator's input language and its
system language are the same thing.

Target: **Raspberry Pi Zero W** (ARMv6 / ARM1176JZF-S at 1 GHz, 512 MB). The
Zero 2 W (ARMv8 / Cortex-A53) is kept as a second target — see
[Cross-compiling for the Pi](#cross-compiling-for-the-pi).

## Architecture

QDOS executes Quadrate through **`lib/interp`**, the interpreter tier in the
Quadrate tree: it parses with the compiler front-end and walks the AST, calling
the runtime's C operations directly. No LLVM on the device.

```
  keypress ──> shell ──> qd_interp_eval() ──> AST walk ──> qd_add/qd_dup/... ──> display
                          (lib/interp + lib/qc, no LLVM)      (lib/rt)
```

The interpreter lives upstream rather than here, because it is a general
capability — any embedder wanting interactive Quadrate needs it — and keeping it
in QDOS would have meant this project owning a piece of the language.

### Why not the LLVM embedding API

`lib/qd` in the Quadrate tree is the obvious way to embed the language, and it is
the wrong tool here. It links `libLLVM.so`, and `qd_build()` shells out to `cc`,
`nm` and a linker before `dlopen`ing the result — so using it means shipping a C
toolchain on the calculator. The latency settles it:

Both paths evaluating `2 3 + drop`, measured on an x86 desktop. The LLVM figure
is the marginal cost of one more expression in a warm session, so it excludes
process and LLVM startup:

| path | per expression | footprint |
|---|---|---|
| `lib/qd` (LLVM ORC JIT), via `quadrepl` | ~83 ms | `libqd.so` 18 MB + `libLLVM.so` 164 MB, plus `cc`, `nm` and a linker on the device |
| `lib/interp` | **~7 µs** | `libinterp.a` 452 KB + `librt.a` 376 KB + `libqc.a` 2.4 MB |

About twelve thousand times. Scaling to the target is an estimate rather than a
measurement — nothing has been timed on a Pi — but an in-order single-issue
ARM1176 at 1 GHz is somewhere around 25–50× slower than the desktop these were
measured on, which puts the JIT at several seconds to evaluate `2 3 +` and the
interpreter in the low hundreds of microseconds. One of those is a calculator.

The footprint column matters as much as the latency: the JIT path needs a C
toolchain present at run time, because `qd_build()` spawns one, and a 164 MB
`libLLVM.so` against the board's 512 MB is its own argument.

### Errors must not power the machine off

The Quadrate runtime treats a stack underflow, a type mismatch, an out-of-range
`pick` and a division by zero as fatal: it prints a diagnostic and calls
`_exit(1)`. That is the right call for a compiled program, where such a failure
means the generated code was wrong. It is the wrong call for a calculator, where
the same failure is someone typing `1 0 /`.

Both halves of the fix are upstream in `lib/interp` and `lib/rt`:

- **In the runtime** — an opt-in recovery mode (`qd_recovery_arm`,
  `qd_recovery_buf`). While armed, a fatal error stores its message on the
  context and unwinds to the embedder instead of ending the process. Nothing
  changes for embedders that do not arm it.
- **In the interpreter** — a per-instruction arity check before dispatch, so the
  common underflow produces a written-for-a-human message rather than relying on
  unwinding.

### Native words

`qd_interp_register()` makes a C function callable by name from typed source,
which is how the machine's capabilities become part of the language rather than
sitting behind a menu. `sto`, `rcl`, `clr` and `cls` are registered this way in
`src/shell/shell.c`.

Values are encoded explicitly, little-endian, behind a magic and a version
(`src/shell/storage.c`). A raw struct write would have tied stored registers to
one compiler's padding and field order, and these bytes have to outlive the
firmware that wrote them. A record that fails to decode reads as absent rather
than as garbage, so a store written by other firmware leaves the machine
working.

The stack is saved on power-off and restored on start. That is what makes the
machine feel instant-on despite a real boot: the seconds spent booting are not
also seconds spent re-entering values.

Settings go the same way, one entry each (`settings.angle`, `settings.decimals`,
`settings.autooff`) rather than one record holding all three, so adding a setting
cannot make the others unreadable — a key nobody has written reads as absent and
keeps its default. They are written the moment a setting changes rather than on
the way out, because pulling the battery is a normal way to turn a calculator
off. Each is range-checked on the way back in: a store written by other firmware
must not be able to select a timeout this build has no name for.

This is the one piece of `lib/qd`'s API that was worth having here, and it is
much cheaper interpreted. In a compiled pipeline a registered function needs a
generated C stub linked so `dlopen` can resolve the symbol — that is precisely
why `qd_build()` spawns `cc`, `nm` and a linker. Interpreted, it is a lookup in a
map while walking the tree.

### What the display has to say that the keypad cannot

A calculator's keys are moulded, not lit, so nothing on the machine can show
what a key will do *next* — whether ALPHA is still locked, or whether `sin` is
about to take degrees. Both are states that quietly change the meaning of the
next press, and both used to live only in a menu two pages deep.

- **The angle** is the fifth soft key in both calculator and line mode, labelled
  `DEG` or `RAD`. The label is the setting and pressing it turns it over, so the
  annunciator and the control are the same five characters. The settings page
  still lists it; they are one setting, not two.
- **The keypad's live face** comes from the backend, through `hal->modifier`,
  because the shell cannot know it: the layer belongs to whatever is reading
  keys. It shows as one character after the prompt, and a keypad with a single
  face reports `QDOS_MOD_NONE` and costs nothing. The simulator also tints the
  modifier key, which is the part real hardware cannot do.

Numbers get the same treatment from the other side. A value too wide for its row
used to keep its tail, which on a number means dropping the leading digits and
leaving something that still reads as an answer; it now moves to exponent form,
and only text is cut — marked, and from the end.

### Typing postfix on a keypad laid out for infix

`5 - 3` is not an error in Quadrate. It pushes 5, subtracts that from whatever
the stack was already holding, and pushes 3. `5-3` is two numbers. Neither
reports anything, and the keypad makes both of them the easiest thing to type,
so the shell recognises the shape — a number, an operator, a number, nothing
else — and answers with the user's own numbers the right way round rather than
running it. The check is deliberately narrow: three tokens or one, and every
other line goes through untouched.

The other half of that is not throwing the line away. A line that fails to
evaluate keeps its text, because the stack it half-moved is visible above it and
the typing is what cost something. `Escape` is the way to be rid of it, and only
leaves the mode once there is nothing left on the line.

### Two tiers

| tier | executed by | cost |
|---|---|---|
| interactive line (`2 3 +`) | AST interpreter over `libqdrt` | sub-ms |
| stored program / app | cross-compiled on host with `quadc` | compile once |

Nothing compiles on the device. The rootfs is the C kernel plus 208 KB of runtime
plus the interpreter.

### Doing nothing, cheaply

The shell loop sleeps on the keypad rather than polling it: `hal->wait()` is a
`poll()` on the evdev fd, with a timeout only when something is actually due.
Nothing is due most of the time, so the timeout is usually infinite and an idle
calculator costs no CPU at all — measured at zero jiffies over three seconds in
the simulator, against sixty-two wakeups a second before.

The cursor blinks at 1 Hz while you are typing and goes solid ten seconds after
the last key. That is not a power decision so much as what it buys: a cursor
that blinks forever is a timer that fires forever, and settling is what lets the
wait go back to having no timeout. The blink itself is close to free — the Sharp
panel holds its image unpowered and costs only what is clocked into it, and the
driver is already sending a VCOM message every second in software VCOM mode.

Auto-off is the only thing here that saves real power, because the Pi Zero W has
no usable suspend: blanking the display would save microwatts against an SoC
floor of something like half a watt. After ten minutes idle (a setting; `NEVER`,
5, 10, 30 or 60) the machine warns for ten seconds and then saves the session and
stops. A card handed to a PC over USB holds it awake — there is nothing to come
back to a half-finished transfer for.

**This does not yet turn anything off.** Two things are missing, both outside the
shell:

| | what is missing |
|---|---|
| **The rootfs** | `inittab` has `tty1::respawn:/usr/bin/qdos --device`, so the shell exiting restarts it. Until that changes, both the PWR key and auto-off are a reboot of the app, not a power-off. |
| **The board** | `poweroff` halts the SoC but nothing cuts the rail, so a halted Pi still draws current. Actually being off needs a soft-latch circuit the firmware can trigger — and until one exists, making PWR halt the machine would leave no way back except pulling the battery. |

Ten minutes is a guess, and deliberately longer than the five most calculators
use: the timeout should be a function of how long a boot takes, and no boot has
been timed. See [Running on hardware](#running-on-hardware).

## Changes to Quadrate

QDOS drove two additions to the Quadrate tree, both now on `master` there:

| | what |
|---|---|
| `lib/interp` | The interpreter tier — `qd_interp_create/eval/peek/attach`. New library, with `README.md` and 60 tests. |
| `lib/rt` | Opt-in recovery from fatal runtime errors — `qd_recovery_buf/arm/disarm/armed`, and `qdrt_fatal_raise()` behind the existing fatal macros. |

Packaging was extended so the interpreter is actually linkable: `libinterp.a`,
`libqc.a` and `libu8t.a` now ship in `dist/`, alongside the `quadrate/interp` and
`quadrate/qc` headers. The `u8t` archive is repacked rather than copied, because
meson builds it thin.

Default behaviour is unchanged throughout — an embedder that never arms recovery
still gets the diagnostic and `_exit(1)`. Quadrate's suite passes: 2060 tests.

## Running on hardware

### Cross-compiling for the Pi

```bash
./cross/run.sh
```

Builds a container with an ARM toolchain and QEMU, cross-compiles the Quadrate
libraries and QDOS, and **runs every test under emulation**. No host packages
needed beyond Docker. `QDOS_ARCH` picks the target, defaulting to the Zero W:

| `QDOS_ARCH` | board | flags |
|---|---|---|
| `armv6` (default) | Zero W | `-mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard` |
| `aarch64` | Zero 2 W | `-mcpu=cortex-a53` |

The Quadrate side is configured with `-Dbuild_tools=false`, which drops the
command-line tools and with them the LLVM requirement. `lib/rt`, `lib/qc` and
`lib/interp` do not need it — that is the whole point of the interpreter tier.

### What that proves, and what it does not

Passing under QEMU is a real result: it covers the ARM ABI on both targets —
32-bit hard-float on the Zero W, 64-bit on the Zero 2 W — alignment, struct
layout, `setjmp`/`longjmp` on a different architecture, and the pixel packing.
Combined with a native run under `-funsigned-char` (ARM's `char` is unsigned
where x86's is signed), the portability questions that usually bite are
answered. The 32-bit target is the one that matters most here: it is where
pointer and `long` width differ from the desktop.

It does not cover the device interfaces — but those do not need a Pi either,
because they are kernel-generic. Both are exercised against the real kernel:

```bash
./tests/run-device-tests.sh
```

| | how it is covered |
|---|---|
| **Keypad** | `test_keypad` creates a virtual keypad through `/dev/uinput`, injects real key presses and reads them back through the production evdev code. |
| **Framebuffer** | `test_framebuffer` opens `/dev/fb0`, reads its geometry through the real ioctls, checks our validation agrees and that pitch × height is mappable. Read-only — the blit goes into a private buffer, so your display is untouched. |

These need device permissions, so they skip cleanly under a plain `meson test`
and the script runs them in a container that has them. Adding yourself to the
`input` and `video` groups works equally well.

What remains genuinely unverified:

| | risk |
|---|---|
| **The panel itself** | An `fbtft` SPI panel has never been driven. Geometry handling is proven against a real framebuffer, but not against *that* one. |
| **The physical keypad** | Matrix wiring and the device tree overlay that turns it into an evdev node. |
| **Boot and power** | Nothing has been booted, timed, or measured. This is where the Pi is genuinely required — and boot time is what should set the auto-off timeout, so that setting is a guess until it is. |

### Panel and keypad choices this backend assumes

An **SPI panel** driven by `fbtft` (ST7789, ILI9341) presents a genuine
`/dev/fb0`, which is exactly what this backend wants — fbdev is the native
interface there, not a compatibility shim.

Be aware that on current Raspberry Pi OS the HDMI path is DRM/KMS
(`vc4-kms-v3d`), where `/dev/fb0` exists only through the kernel's fbdev
emulation. That is fine for bring-up on a monitor, but it is a deprecated
interface to depend on. If the display ends up being HDMI rather than SPI, the
backend should move to DRM dumb buffers.

For the keypad, a GPIO matrix exposed through the `matrix-keypad` device tree
overlay (or `gpio-keys` for a handful of discrete keys) produces a real evdev
node, so no userspace scanning is needed and the existing backend applies
unchanged.

### First bring-up

`QDOS_FB`, `QDOS_INPUT` and `QDOS_STORE` override the device paths, so the
binary can be pointed at whatever the board actually enumerates without
rebuilding:

```bash
QDOS_FB=/dev/fb1 QDOS_INPUT=/dev/input/event2 ./qdos --device
```

