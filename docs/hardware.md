# Hardware

What the calculator is made of. A part is *chosen* when something in the build
depends on it: a driver written, a pin assigned, a screen size compiled in.
What is still open is listed at the end.

## The board

An **ESP32-S3** on an **ESP32-S3-WROOM-1-N16R8** module: two Xtensa LX7 cores
at 240 MHz, 512 KB of internal RAM, 8 MB of octal PSRAM and 16 MB of flash. The
ESP32-S3-DevKitC-1-N16R8 carries the same module, and is the development board
for it. How it is built and flashed is in [esp32/README.md](../esp32/README.md).

**Not Linux, as the machine.** A calculator is on the moment a key is pressed,
and a Pi is not: its GPU firmware alone takes on the order of a second before
the kernel starts, a tuned boot still lands around two, and with no
suspend-to-RAM the only way to save power is to be off and boot again. The
Raspberry Pi stays as a second target, [below](#the-raspberry-pi-second-target).

**The PSRAM is not optional.** The shell's state, a parsed program and the value
stack take 0.5–0.9 MB, far past the S3's internal RAM; the numbers are in
[esp32/README.md](../esp32/README.md#memory). An N8R8 runs the shell too, but
DOOM's system store needs the 16 MB of flash.

**The radio goes unused.** Every ESP32-S3 has Wi-Fi and Bluetooth, and nothing
turns them on. The module's radio is certified as it is, so that costs no radio
certification of our own; the EMC testing every electronic product needs is
still ours.

## The display

**Adafruit 4694** — the SHARP Memory Display Breakout, 2.7" — which carries the
**Sharp LS027B7DH01**: 400×240, one bit per pixel, reflective, no backlight.
Ordered as DigiKey 13157994. Decided 2026-09-19.

The bare panel wants 5 V with 3 V logic, which is the reason for taking the
breakout rather than the panel: it adds a 3 V regulator, a 5 V boost converter
and level shifting, so 3.3 V logic drives it directly. The panel slots into a
ZIF socket on the board, and there are four mounting holes.

That geometry is compiled in, not configured: `QDOS_SCREEN_W`/`QDOS_SCREEN_H` in
`include/qdos/hal.h` are 400×240, which is 25×10 cells of the 16×24 font. The UI
was designed against this panel and no other.

Three properties earn it the slot:

- It holds its image with no power, so a cursor blink or an idle screen costs
  only what is clocked into it.
- A full frame is 12 KB on the wire, which is what sets the refresh ceiling —
  not the processor.
- One bit per pixel is the whole colour budget, which is why the UI is drawn the
  way it is.

| | |
|---|---|
| Bus | SPI2, 2 MHz |
| Chip select | **Active high** — the opposite of every other SPI device |
| VCOM | Software mode: the driver toggles it once a second |
| Driver | `esp32/main/sharp_lcd.c`. Only the lines that changed are sent |
| Supply | 3V3. On a DevKitC-1 powered from USB the panel stayed blank from 5V |

`EIN` (EXTMODE) is pulled low on the breakout, which *is* software VCOM mode,
and the cheaper one: driving EXTCOMIN externally at 1 Hz is reported to roughly
double the panel's draw. `DISP` is pulled high, so it needs no pin unless the
panel is to be blanked from software. `3Vo` is the regulator's output, not an
input: leave it open.

The pins are in [esp32/README.md](../esp32/README.md#wiring) and in menuconfig.

## The keypad

**54 keys in 10 rows**, three layers: the plain face, an ALPHA lock
for letters, and a SYMBOL one-shot for Quadrate's syntax. `QDOS_PAD_COLS` and
`QDOS_PAD_ROWS` in `src/hal/sim/keypad_ui.h` are the simulator's mirror of the
physical pad, and `src/hal/sim/keypad_ui.c` holds the legend for every key.
The soft row and the five rows of the numeric block are five keys wide; the
four function rows between them are six, with the arrows an inverted T at
their right, as on an HP 48.

A 6×10 matrix wired straight to the S3's GPIOs: ten open-drain rows and six
pulled-up columns, 16 pins, with no expander. The columns must be RTC GPIOs
(0–21), because they are what wakes the chip from deep sleep. The five-key rows
leave their sixth column unwired. The scanner is `esp32/main/keymatrix.c`.

A diode per key is needed if three keys at the corners of a rectangle might ever
be down together.

## USB

The S3's own USB port (GPIO 19 and 20), as a serial console:

- **A keyboard** — a terminal typed at it is the keypad, with a keyboard's
  extras for typing a long line.
- **Uploads** — `qdosctl` writes programs and app folders into the inbox over
  the same port. See `src/hal/serial_console.c`, which the simulator serves
  on a pseudo-terminal too, and qdosctl(1).

It is not a USB drive. The S3 has a device controller and TinyUSB has a
mass-storage class, so that is possible, but it is not done.

## Storage

The module's flash, in two FAT partitions beside the firmware:

| partition | | |
|---|---|---|
| `system` | 512 KB, read-only | `programs/system`, built into an image and flashed with the firmware. 6 MB in the DOOM build |
| `data` | 4 MB, wear-levelled | `user/` — everything QDOS writes; `inbox/` — uploads |

## Power

Off is deep sleep: the session is saved, every key row is held low, and any key
wakes the chip. Waking is a fresh start that restores the session from flash.
Auto-off goes the same way.

No battery, charger or regulator has been chosen.

## The enclosure

A front plate carrying the keys, the panel and every standoff, and a back shell
that is structurally a lid. The 98.5 × 214.1 × 32 mm worked out for the Pi was
set by the Pi and its expander stacked behind the panel (see the Pi's own
section); the S3 needs neither stacked, so the depth wants working out again.

## Still open

| | what is missing |
|---|---|
| **Wake time** | Under QEMU the shell is up 1.3 s after reset. It has to be timed on the chip, from a key to the stack on screen: if it is near that, the fix is in what the shell does at start, not in the part. |
| **Panel in sleep** | VCOM stops while the chip sleeps, and the panel is still powered. Either the panel's supply gets a switch, or the ULP coprocessor keeps toggling VCOM through sleep. |
| **Power** | Battery, charging and regulation, and the current drawn in deep sleep. |
| **Clock** | The RTC keeps the time through deep sleep, but nothing sets it. |
| **Enclosure** | No model, and the dimensions above are the Pi's. |

## The Raspberry Pi (second target)

| | | arch | cross target |
|---|---|---|---|
| **Zero W** | BCM2835, ARM1176JZF-S at 1 GHz, 512 MB | ARMv6 | `armv6` |
| Zero 2 W | BCM2710A1, quad Cortex-A53 | ARMv8 | `aarch64` |

The same panel and pad on a Linux backend (`src/hal/device`), built into a
Buildroot image; see `firmware/README.md`. `QDOS_BOARD` picks the board and
defaults to `zerow`. The Zero 2 W is there because it is the only one QEMU can
boot, so it is where the Linux path gets exercised. **Nothing here has been
powered on.**

### The display on the Pi

The bare panel wants 5 V with 3 V logic, which is the reason for taking the
breakout rather than the panel: it adds a 3 V regulator, a 5 V boost converter
and level shifting, so a 3.3 V Pi can drive it directly. The panel slots into a
ZIF socket on the board, and there are four mounting holes.

| | |
|---|---|
| Bus | SPI0, CE0, 2 MHz max |
| Chip select | **Active high** — the opposite of every other SPI device |
| VCOM | Software mode: the driver toggles it in the message it already sends once a second |
| Kernel | `CONFIG_TINYDRM_SHARP_MEMORY` (in-tree DRM tiny driver since 6.13) |
| Interface | `CONFIG_DRM_FBDEV_EMULATION` — it is a DRM device, but QDOS draws through `/dev/fb0` |
| Overlay | `firmware/buildroot/board/overlays/sharp-ls027b7dh01-overlay.dts` |

Wiring, by the breakout's own labels:

| breakout | Pi | header pin | |
|---|---|---|---|
| VIN | 5V | 2 | 3–5 V in. From 5V so the boost converter is not fed off the Pi's 3V3 regulator |
| GND | GND | 6 | |
| CLK | GPIO11 (SCLK) | 23 | |
| DI | GPIO10 (MOSI) | 19 | Write-only: there is no MISO to wire |
| CS | GPIO8 (CE0) | 24 | Active high |
| 3Vo | — | — | The regulator's **output**, not an input. Leave open |
| DISP | — | — | Pulled high on the board; wire it only to blank the panel from software |
| EIN | — | — | EXTMODE, pulled low on the board — which *is* software VCOM mode |
| EMD | — | — | EXTCOMIN, unused while EXTMODE is low |

Three of those are the board agreeing with the overlay already in the tree.
`EIN` pulled low is what `sharp,vcom-mode = "software"` assumes, and it is also
the cheaper mode: driving EXTCOMIN externally at 1 Hz is reported to roughly
double the panel's draw. `DISP` pulled high is why the overlay does not wire it.
And the active-high chip select is what the overlay's `cs-gpios` line exists to
say — polarity lives there, not in `spi-cs-high` on the device node, and getting
it wrong is the difference between a working panel and a blank one.

`dtoverlay=sharp-ls027b7dh01` is **commented out** in `config.txt` until the
panel is attached: it takes SPI0 CE0, and without it the image still boots to
HDMI and under QEMU. Two things change when it goes in — `gpu_mem` drops from 64
to 16, because the firmware framebuffer is no longer used, and the fbdev path
stops being an emulation of HDMI's DRM/KMS and becomes the panel's own.

**Kept as a fallback:** an `fbtft` SPI panel (ST7789, ILI9341) presents a real
`/dev/fb0` too, so the device backend needs no changes for either. The lines for
it are in `config.txt`, commented.

### The keypad on the Pi

| | |
|---|---|
| Scanning | GPIO matrix through the `matrix-keypad` overlay — a real evdev node, no userspace scanning |
| Kernel | `CONFIG_KEYBOARD_MATRIX`, `CONFIG_INPUT_MATRIXKMAP`, `CONFIG_KEYBOARD_GPIO` |
| Expander | Adafruit GPIO Expander Bonnet (**MCP23017**) on I²C, so SPI0 stays with the panel |
| Kernel | `CONFIG_GPIO_MCP23S08`, `CONFIG_I2C_BCM2835`, `CONFIG_I2C_CHARDEV` |

A 6×10 matrix needs 16 lines, which is why the expander is there rather than the
header alone — and it is all 16 of the MCP23017's, with none left over. The
five-key rows leave their sixth column unwired.

**Open:** the pin assignment. The placeholder in `config.txt` reads
`dtoverlay=matrix-keypad,rows=5,cols=5` — a stand-in, not the pad. It has to
become 10 rows by 6 columns against the expander's gpiochip before it means
anything.

Until the pad exists, input is a **USB keyboard**. `QDOS_KEYMAP` selects `us` or
`se`; the firmware image sets `se`, because on a Swedish layout the braces
Quadrate needs are on AltGr and a US table makes declaring a function impossible.

### USB

One OTG port, dual role: `dtoverlay=dwc2,dr_mode=otg`.

- **Host** — the keyboard that bring-up depends on.
- **Device** — plugged into a PC it offers the inbox partition as a USB drive;
  the PC drops `.qd` files on it and the shell declares them when it is handed
  back. `CONFIG_USB_F_MASS_STORAGE`, which costs nothing while unplugged.

### Storage

One microSD card. MBR, four partitions — all an MBR holds:

| | format | size | |
|---|---|---|---|
| boot | FAT | 64 M | firmware, kernel, `config.txt`, overlays |
| rootfs | ext4, **ro** | — | mounted read-only: pulling the power is the normal way to switch a calculator off |
| data | ext4 | 16 M | everything QDOS writes — session, registers, user programs |
| inbox | FAT32 (LBA) | 32 M | `QDOS-INBOX`, the partition the gadget hands to a PC |

### Console

Serial on `ttyAMA0` at 115200 (`dtoverlay=miniuart-bt`, `enable_uart=1`,
`console=ttyAMA0,115200` in `cmdline.txt`). The console stays off the panel
deliberately — QDOS holds the VT in `KD_GRAPHICS` so nothing draws over the
calculator.

### The enclosure

A front plate carrying the keys, the panel and every standoff, and a back shell
that is structurally a lid. 98.5 × 214.1 × 32 mm, which is an HP-48 that gained
7 mm of thickness.

It gained them to the expander bonnet. The pad and the panel both want the
front, so the Pi lives behind the panel, and the case is then as deep as the
tallest thing on the Pi's header. Moving the MCP23017 onto the pad board instead
takes the case to 24.5 mm.

### Power — the undecided one

No battery, charger, regulator or switch has been chosen. What is known:

- A Pi Zero has no suspend-to-RAM and idles at roughly 0.5–1 W, so auto-off is
  the only thing that saves real power.
- `poweroff` halts the SoC but **nothing cuts the rail** — a halted Pi still
  draws current. Actually being off needs a **soft-latch circuit the firmware
  can trigger**, and until one exists, making PWR halt the machine would leave
  no way back except pulling the battery.
- So today both the PWR key and auto-off are a restart of the app: `inittab`
  respawns `qdos --device`.
- The auto-off timeout is a guess until boot time is measured on the real board,
  because how long it takes to come back is what the timeout is trading against.

The panel helps here: it keeps its image unpowered, so what is on screen when
the rail is cut stays readable.

### Still open

| | what is missing |
|---|---|
| **Panel** | The part is settled; it has never been driven. The geometry handling is proven against a real framebuffer, but not against *that* one. Uncomment the overlay, drop `gpu_mem` to 16, and find out. |
| **Keypad matrix** | The pin assignment and the overlay line that follows from it. Wiring and switch choice are unmade. |
| **Power** | Battery, charging, regulation and the soft-latch circuit. All of it. |
| **Boot timing** | Nothing has been booted, timed or measured on hardware. |
| **Enclosure** | No model. The dimensions above are estimates until the parts are in hand and a caliper has been on them. |

See [design.md](./design.md) for why the machine is built this way, and
`firmware/README.md` for how the Pi's image is produced.
