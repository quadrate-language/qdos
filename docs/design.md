# QDOS design notes

Why QDOS is built the way it is. See [README](../README.md) for building and
running.

The OS is C. The shell is [Quadrate](https://quad.r8.rs) — a stack language, on a
device whose natural input model is RPN. The calculator's input language and its
system language are the same thing.

Target: **Raspberry Pi Zero 2 W** (ARMv8 / Cortex-A53).

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

About twelve thousand times. A Cortex-A53 at 1 GHz is roughly 8–15× slower than
the desktop these were measured on, which puts the JIT near a second to evaluate
`2 3 +` and the interpreter around 100 µs. One of those is a calculator.

The footprint column matters as much as the latency: the JIT path needs a C
toolchain present at run time, because `qd_build()` spawns one.

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

This is the one piece of `lib/qd`'s API that was worth having here, and it is
much cheaper interpreted. In a compiled pipeline a registered function needs a
generated C stub linked so `dlopen` can resolve the symbol — that is precisely
why `qd_build()` spawns `cc`, `nm` and a linker. Interpreted, it is a lookup in a
map while walking the tree.

### Two tiers

| tier | executed by | cost |
|---|---|---|
| interactive line (`2 3 +`) | AST interpreter over `libqdrt` | sub-ms |
| stored program / app | cross-compiled on host with `quadc` | compile once |

Nothing compiles on the device. The rootfs is the C kernel plus 208 KB of runtime
plus the interpreter.

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

Builds a container with an aarch64 toolchain and QEMU, cross-compiles the
Quadrate libraries and QDOS for Cortex-A53, and **runs every test under
emulation**. No host packages needed beyond Docker.

The Quadrate side is configured with `-Dbuild_tools=false`, which drops the
command-line tools and with them the LLVM requirement. `lib/rt`, `lib/qc` and
`lib/interp` do not need it — that is the whole point of the interpreter tier.

### What that proves, and what it does not

Passing under QEMU is a real result: it covers the 64-bit ARM ABI, alignment,
struct layout, `setjmp`/`longjmp` on a different architecture, and the pixel
packing. Combined with a native run under `-funsigned-char` (ARM's `char` is
unsigned where x86's is signed), the portability questions that usually bite are
answered.

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
| **Boot and power** | Nothing has been booted, timed, or measured. This is where the Pi is genuinely required. |

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

