# QDOS on the ESP32-S3

The calculator on a microcontroller instead of a Pi: no Linux, no boot, and
deep sleep between sessions. The shell, the interpreter and the programs are
the same; only the backend underneath is new.

**Target: an ESP32-S3 with 8 MB of octal PSRAM** — an N8R8 or N16R8 module,
such as the ESP32-S3-DevKitC-1-N16R8. A chip without PSRAM cannot hold it: the
shell's state and a parsed program need well over the 320 KB a plain ESP32
has. The numbers are below.

**Nothing here has run on a real chip.** It builds, and it runs under
Espressif's QEMU: arithmetic, recursion to the depth limit, every system app,
power-off, and the session coming back on the next boot. The panel driver, the
key matrix and deep-sleep wake are written but untested, because QEMU emulates
none of them.

## Building

ESP-IDF 5.5 and the Quadrate source tree beside this one, as for the Pi. The
component builds Quadrate's runtime, front-end and interpreter from source, so
nothing needs to be built in the Quadrate tree first — except that u8t has to
be fetched (`meson subprojects download` there, or any Meson build of it).

```bash
cd esp32
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Or with no ESP-IDF installed, from the directory holding both trees:

```bash
docker run --rm -it -u $(id -u):$(id -g) -e HOME=/tmp -v $PWD:/work \
    -w /work/qdos/esp32 espressif/idf:v5.5 idf.py build
```

`QUADRATE_SRC` points at a Quadrate tree somewhere else. `idf.py menuconfig`,
under **QDOS**, has the pins, the shell's stack and the call depth.

### Under QEMU

Espressif's QEMU 9.2.2 or later, for the S3's PSRAM; the one in the v5.5 image
is older. `sdkconfig.qemu` moves the console to the UART, drops the panel and
echoes the stack on the console instead, since there is nothing to look at:

```bash
idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build
idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig qemu \
    --qemu-extra-args='-m 8M -global driver=ssi_psram,property=is_octal,value=true'
```

Type at it as at the serial console, below. Each change to the stack is
logged with how much of the shell's stack it has taken.

## Wiring

All of it is in menuconfig; these are the defaults.

| | GPIO | |
|---|---|---|
| Panel SCLK | 39 | SPI2, 2 MHz |
| Panel DI (MOSI) | 40 | |
| Panel CS | 41 | Active high, as on the Pi |
| Panel DISP | — | The breakout pulls it up; set a pin to drive it |
| Key rows 0–9 | 8–17 | Open-drain, top row first |
| Key columns 0–5 | 1, 2, 4, 5, 6, 7 | Pull-ups. Must be RTC GPIOs (0–21): they wake the chip |

Left alone on purpose: 0, 3, 45 and 46 are strapping pins, 19 and 20 are the
USB port, and 26 to 37 belong to the flash and the octal PSRAM.

The matrix is the pad's own layout from `src/hal/sim/keypad_ui.c`: row *r*, key
*c* in that row goes to row pin *r* and column pin *c*, and a five-key row leaves
column 5 unwired. Put a diode on each key if three keys at the corners of a
rectangle might ever be down together.

The panel is the Adafruit 4694 breakout, as in [docs/hardware.md](../docs/hardware.md),
on 3V3 logic. Only the lines that changed are sent, and VCOM is toggled once a
second.

## The serial console

The USB port is a keyboard too, as a USB keyboard is on the Pi: what the
simulator's keyboard does, a terminal does. Digits and operators are their
keys, Enter is ENTER, Backspace is DEL, Tab completes, and Escape is ESC. The
arrows are the arrows, F1–F5 the soft row, and F10 is power.

## Power

Power-off saves the session, as everywhere, then the chip goes into deep sleep
with every key row held low, so any key wakes it. Waking is a fresh start
that restores the session from flash: under QEMU the shell is up 1.3 s after
reset, which the real chip has yet to confirm. Auto-off goes the same way.

**Open:** VCOM stops while the chip sleeps, and the panel is still powered.
The breakout cannot switch itself off, so either the panel's supply gets a
switch, or the ULP coprocessor keeps toggling VCOM through sleep.

## Memory

Measured under QEMU. What lives where:

| | where | size |
|---|---|---|
| The shell's stack | internal RAM | 224 KB, of which the shell uses 42 KB and each Quadrate call level about 1.25 KB more |
| The shell's state, parsed programs, the value stack | PSRAM, by `malloc` | 0.5–0.9 MB |
| The shell's and Quadrate's static buffers | PSRAM, by linker fragment | 150 KB |
| Free once the shell is up | | 153 KB internal, 7.5 MB PSRAM |

The stack has to be internal RAM: the store is on flash, and a task whose stack
is in PSRAM may not touch flash. That is what limits recursion. Quadrate
refuses a call deeper than `QDOS_CALL_DEPTH`, 100 by default against the Pi's
256, which keeps 99 levels at 165 KB well inside the stack.

## Storage

| partition | | |
|---|---|---|
| `system` | 512 KB, read-only FAT | `programs/system`, built into an image and flashed with the firmware |
| `data` | 4 MB, wear-levelled FAT | `user/` — everything QDOS writes; `inbox/` — uploads |

The layout fits 8 MB of flash.

## What the Pi has that this does not

- **Native modules.** There is no dynamic linker. `hal->store_path` is NULL, so
  the shell never looks for them.
- **The USB drive.** The S3 has a USB device controller and TinyUSB has a
  mass-storage class, so this is possible, but it is not done. Until then the
  inbox can only be written by flashing.
- **A battery gauge**, until there is a battery.
- **The time of day.** It reads as unset until something sets the clock. The
  RTC keeps the time through deep sleep, but nothing sets it yet.

## How the port is put together

- `components/quadrate`: Quadrate's runtime, front-end, interpreter, u8t and
  the math module, compiled from the Quadrate tree. It uses that tree's
  `platform/pthread` (newlib has no C11 threads) and `platform/none` (no
  processes, no dynamic linker).
- `components/qdos_core`: the same sources as `qdos_core` in `meson.build`, less
  the evdev keypad and the fbdev blit. It is built with `QDOS_NO_DLOPEN` and
  `QDOS_CAPTURE_STDIO`, which catches what `print` writes through a per-task
  `stdout` rather than a pipe.
- `main/`: the backend (`hal_esp32.c`), the panel (`sharp_lcd.c`), the matrix
  (`keymatrix.c`), the console keyboard (`serial_keys.c`) and the entry point.
