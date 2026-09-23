# Fonts

Two of them here, for two different jobs, and a third that is only a bitmap.

## VCR_OSD_MONO_1.001.ttf

The panel font, and the only one that reaches the device.

"VCR OSD Mono" by Riciery Leal, 2015. Published at
<https://www.dafont.com/vcr-osd-mono.font> under dafont's "100% Free" category.
The archive carries no licence file and the font's own name table has no
copyright or licence string, so that listing is the whole of the provenance.

It is **provenance, not source**. The glyphs were rasterised from it once, at
27px, and the pixels then corrected by hand; `assets/font16x24.txt` is what the
font is now and what `tools/genfont.py` packs into `src/ui/font16x24.c`.
Re-rasterising would throw those corrections away, so nothing does.

Kept because it is where the shapes came from and what a wholesale redraw would
start from again. See the font section of the top-level README for why 16x24 at
one bit ended up hand-drawn.

## DejaVuSansCondensed-Bold.ttf

The simulator's keycap font. `tools/genpadfont.py` rasterises it at 13px into
`src/hal/sim/padfont.c` as antialiased coverage, one byte per pixel.

Unlike the panel font this one *is* rasterised, and that is the point: the
panel is one bit per pixel, so its font has to be a bitmap fitted to the grid,
but the simulator's case is 24-bit RGB and can afford the greys that make a
13px face legible. It also already has `√ ÷ × ± π` and the four arrows, which
the panel font needs drawn by hand.

DejaVu Sans, a Bitstream Vera derivative. The licence is in
`DejaVu-LICENSE.txt`: free to use, redistribute and modify, with the condition
that a modified version not be sold on its own and not carry the reserved font
names. Copied from `/usr/share/fonts/TTF/` so regenerating does not depend on
what happens to be installed.

## Terminus 8x16 (no file here)

The editor's font, drawn in the program pane so it shows 10 lines of 50
columns instead of 7 of 24. `assets/font8x16.txt` is the source, packed into
`src/ui/font8x16.c` by `tools/gensmallfont.py`.

Terminus Font 4.20 by Dimitar Toshkov Zhekov, GPL-2.0-or-later, imported from
the Linux console's `Lat2-Terminus16.psfu.gz` with
`tools/gensmallfont.py --import`. Printable ASCII only. Terminus is drawn one
bit per pixel at this cell, so unlike the panel font nothing was corrected by
hand, and no TTF is kept: the bitmap is the whole font.
