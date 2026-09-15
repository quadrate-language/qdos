# Fonts

## VCR_OSD_MONO_1.001.ttf

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
