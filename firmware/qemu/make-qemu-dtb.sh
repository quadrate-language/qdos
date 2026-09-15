#!/usr/bin/env bash
#
# Produce a QEMU-only device tree from a board DTB.
#
#   make-qemu-dtb.sh <in.dtb> <out.dtb>
#
# Emulator scaffolding. Nothing here is flashed to a real Pi.
#
# A real Zero W reads its SD card through bcm2835-sdhost at 0x20202000 and keeps
# the controller at 0x20300000 for the WiFi chip's SDIO. QEMU's raspi0 puts the
# card on 0x20300000 and has nothing on sdhost, so with the stock tree mmcblk0
# never appears, rootwait waits forever, and the screen stays black.
#
# Enabling the tree's spare `sdhci` node at the same address does not help: it
# never binds. The node that does bind is `mmcnr`, so that is the one to reuse --
# take the SDIO-only properties off it and it enumerates the card normally.
#
# This edits decompiled source rather than applying an overlay, because the
# change is mostly deletions and fdtoverlay cannot express those.
set -euo pipefail

IN=$1
OUT=$2
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

dtc -I dtb -O dts -o "$TMP/board.dts" "$IN" 2>/dev/null

python3 - "$TMP/board.dts" "$TMP/qemu.dts" <<'PY'
import re, sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()

# The SD controller QEMU does not emulate
s = re.sub(r'(sdhost: mmc@7e202000 \{.*?)status = "okay";',
           r'\1status = "disabled";', s, count=1, flags=re.S)

start = s.index('mmcnr: mmcnr@7e300000 {')
end = s.index('\n\t\t};', s.index('wifi@1 {', start)) + len('\n\t\t};')
node = s[start:end]

# A WiFi SDIO bus declares a fixed function and no card detect. Drop both and
# the same controller enumerates QEMU's SD card.
node = node.replace('\t\t\tnon-removable;\n', '')
node = re.sub(r'\n\t*brcmf: wifi@1 \{.*?\n\t*\};', '', node, flags=re.S)

s = s[:start] + node + s[end:]

# The card now enumerates, but as mmcblk1: the aliases still number the
# disabled sdhost as mmc0. Point mmc0 at the controller that has the card so
# root=/dev/mmcblk0p2 means the same thing on both boards.
s = re.sub(r'mmc0 = "/soc/mmc@7e202000";',
           'mmc0 = "/soc/mmcnr@7e300000";', s)
s = re.sub(r'\n\t*mmc1 = "/soc/mmcnr@7e300000";', '', s)

# On real hardware config.txt's `dtoverlay=miniuart-bt` hands the console to
# the PL011. QEMU never runs the firmware, so no overlay from config.txt is
# ever applied and serial0 stays on the aux UART -- which QEMU cannot register
# ("unable to register 8250 port"), leaving init with nowhere to talk. Do the
# same swap here so the console works under emulation.
s = s.replace('serial0 = "/soc/serial@7e215040";', 'serial0 = "/soc/serial@7e201000";')
s = s.replace('serial1 = "/soc/serial@7e201000";', 'serial1 = "/soc/serial@7e215040";')

open(dst, 'w').write(s)
PY

dtc -I dts -O dtb -o "$OUT" "$TMP/qemu.dts" 2>/dev/null
echo "QEMU device tree: $OUT"
