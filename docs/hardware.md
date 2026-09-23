# Hardware

What the calculator is made of. The choices were already in the tree — in
`config.txt`, the kernel fragment, the panel overlay and the HAL constants —
but nowhere in one place, which is what this file is for.

**Nothing here has been powered on.** A part is *chosen* when something in the
build depends on it: a driver enabled, an overlay written, a screen size
compiled in. That is a stronger commitment than a shortlist and a weaker one
than a working machine. What is still open is listed at the end.

## The board

| | | arch | cross target |
|---|---|---|---|
| **Zero W** (default) | BCM2835, ARM1176JZF-S at 1 GHz, 512 MB | ARMv6 | `armv6` |
| Zero 2 W (second target) | BCM2710A1, quad Cortex-A53 | ARMv8 | `aarch64` |

`QDOS_BOARD` picks the target throughout and defaults to `zerow`. **The Zero W
is the machine** — reaffirmed 2026-09-19. The Zero 2 W stays supported for one
reason: it is the only one QEMU can boot, so it is where the software path gets
exercised. That makes ARMv6 the architecture every part choice has to suit, and
it means the hardware is the first place the real target runs at all. See
`firmware/README.md`.

## The display

**Adafruit 4694** — the SHARP Memory Display Breakout, 2.7" — which carries the
**Sharp LS027B7DH01**: 400×240, one bit per pixel, reflective, no backlight.
Ordered as DigiKey 13157994. Decided 2026-09-19.

The bare panel wants 5 V with 3 V logic, which is the reason for taking the
breakout rather than the panel: it adds a 3 V regulator, a 5 V boost converter
and level shifting, so a 3.3 V Pi can drive it directly. The panel slots into a
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

## The keypad

**5 columns × 10 rows = 50 keys**, three layers: the plain face, an ALPHA lock
for letters, and a SYMBOL one-shot for Quadrate's syntax. `QDOS_PAD_COLS` and
`QDOS_PAD_ROWS` in `src/hal/sim/keypad_ui.h` are the simulator's mirror of the
physical pad, and `src/hal/sim/keypad_ui.c` holds the legend for every key.

| | |
|---|---|
| Scanning | GPIO matrix through the `matrix-keypad` overlay — a real evdev node, no userspace scanning |
| Kernel | `CONFIG_KEYBOARD_MATRIX`, `CONFIG_INPUT_MATRIXKMAP`, `CONFIG_KEYBOARD_GPIO` |
| Expander | Adafruit GPIO Expander Bonnet (**MCP23017**) on I²C, so SPI0 stays with the panel |
| Kernel | `CONFIG_GPIO_MCP23S08`, `CONFIG_I2C_BCM2835`, `CONFIG_I2C_CHARDEV` |

A 5×10 matrix needs 15 lines, which is why the expander is there rather than the
header alone.

**Open:** the pin assignment. The placeholder in `config.txt` reads
`dtoverlay=matrix-keypad,rows=5,cols=5` — a stand-in, not the pad. It has to
become 10 rows by 5 columns against the expander's gpiochip before it means
anything.

Until the pad exists, input is a **USB keyboard**. `QDOS_KEYMAP` selects `us` or
`se`; the firmware image sets `se`, because on a Swedish layout the braces
Quadrate needs are on AltGr and a US table makes declaring a function impossible.

## USB

One OTG port, dual role: `dtoverlay=dwc2,dr_mode=otg`.

- **Host** — the keyboard that bring-up depends on.
- **Device** — plugged into a PC it offers the inbox partition as a USB drive;
  the PC drops `.qd` files on it and the shell declares them when it is handed
  back. `CONFIG_USB_F_MASS_STORAGE`, which costs nothing while unplugged.

## Storage

One microSD card. MBR, four partitions — all an MBR holds:

| | format | size | |
|---|---|---|---|
| boot | FAT | 64 M | firmware, kernel, `config.txt`, overlays |
| rootfs | ext4, **ro** | — | mounted read-only: pulling the power is the normal way to switch a calculator off |
| data | ext4 | 16 M | everything QDOS writes — session, registers, user programs |
| inbox | FAT32 (LBA) | 32 M | `QDOS-INBOX`, the partition the gadget hands to a PC |

## Console

Serial on `ttyAMA0` at 115200 (`dtoverlay=miniuart-bt`, `enable_uart=1`,
`console=ttyAMA0,115200` in `cmdline.txt`). The console stays off the panel
deliberately — QDOS holds the VT in `KD_GRAPHICS` so nothing draws over the
calculator.

## The enclosure

A front plate carrying the keys, the panel and every standoff, and a back shell
that is structurally a lid. 98.5 × 214.1 × 32 mm, which is an HP-48 that gained
7 mm of thickness.

It gained them to the expander bonnet. The pad and the panel both want the
front, so the Pi lives behind the panel, and the case is then as deep as the
tallest thing on the Pi's header. Moving the MCP23017 onto the pad board instead
takes the case to 24.5 mm.

## Power — the undecided one

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

## Still open

| | what is missing |
|---|---|
| **Panel** | The part is settled; it has never been driven. The geometry handling is proven against a real framebuffer, but not against *that* one. Uncomment the overlay, drop `gpu_mem` to 16, and find out. |
| **Keypad matrix** | The pin assignment and the overlay line that follows from it. Wiring and switch choice are unmade. |
| **Power** | Battery, charging, regulation and the soft-latch circuit. All of it. |
| **Boot timing** | Nothing has been booted, timed or measured on hardware. |
| **Enclosure** | No model. The dimensions above are estimates until the parts are in hand and a caliper has been on them. |

See [design.md](./design.md) for why the machine is built this way, and
`firmware/README.md` for how the image is produced.
