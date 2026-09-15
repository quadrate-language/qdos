#!/usr/bin/env python3
"""Rasterise a TTF into src/ui/font16x24.c.

The panel is one bit per pixel, so glyphs are thresholded at build time and
baked in as bitmaps. Nothing links a rasteriser.

    tools/genfont.py assets/fonts/VCR_OSD_MONO_1.001.ttf
"""

import sys
from PIL import Image, ImageDraw, ImageFont

CELL_W, CELL_H = 16, 24
FIRST, LAST = 1, 126

# Symbols the panel needs and the TTF has no usable glyph for. Drawn rather than
# scaled: at 16x24 and one bit, a stroke either lands on the grid or smears.
# They live below space, where ASCII has nothing printable, so a label stays a
# plain C string and qdos_font_row() keeps taking a char.
SYMBOLS = {
    0x01: ("up", [
        "................", "................", "................",
        ".......##.......", "......####......", ".....######.....",
        "....########....", "...##########...", "..############..",
        ".......##.......", ".......##.......", ".......##.......",
        ".......##.......", ".......##.......", ".......##.......",
        ".......##.......", ".......##.......", ".......##.......",
        "................", "................", "................",
        "................", "................", "................",
    ]),
    0x04: ("sqrt", [
        "................", "................", "................",
        "..........######", "..........##....", "..........##....",
        "..........##....", "..........##....", ".........##.....",
        ".........##.....", ".........##.....", ".........##.....",
        "........##......", "........##......", "##......##......",
        "##......##......", ".##....##.......", ".##....##.......",
        "..##..##........", "..##..##........", "...####.........",
        "....##..........", "................", "................",
    ]),
    0x05: ("divide", [
        "................", "................", "................",
        "................", "................", "................",
        ".......##.......", ".......##.......", "................",
        "................", "..############..", "..############..",
        "................", "................", ".......##.......",
        ".......##.......", "................", "................",
        "................", "................", "................",
        "................", "................", "................",
    ]),
    0x06: ("times", [
        "................", "................", "................",
        "................", "................", "...##......##...",
        "....##....##....", ".....##..##.....", "......####......",
        ".......##.......", "......####......", ".....##..##.....",
        "....##....##....", "...##......##...", "................",
        "................", "................", "................",
        "................", "................", "................",
        "................", "................", "................",
    ]),
    0x07: ("plusminus", [
        "................", "................", "................",
        ".......##.......", ".......##.......", ".......##.......",
        "..############..", "..############..", ".......##.......",
        ".......##.......", ".......##.......", "................",
        "................", "..############..", "..############..",
        "................", "................", "................",
        "................", "................", "................",
        "................", "................", "................",
    ]),
    0x08: ("pi", [
        "................", "................", "................",
        "................", "..############..", "..############..",
        "....##....##....", "....##....##....", "....##....##....",
        "....##....##....", "....##....##....", "....##....##....",
        "....##....##....", "....##....###...", "...####....##...",
        "................", "................", "................",
        "................", "................", "................",
        "................", "................", "................",
    ]),
}

# Down is the up arrow mirrored, so the pair cannot drift apart
SYMBOLS[0x02] = ("down", list(reversed(SYMBOLS[0x01][1])))

SYMBOLS[0x03] = ("left", [
    "................", "................", "................",
    "................", "................", "................",
    "................", ".......#........", "......##........",
    ".....###........", "....####........", "...#############",
    "...#############", "....####........", ".....###........",
    "......##........", ".......#........", "................",
    "................", "................", "................",
    "................", "................", "................",
])

SYMBOLS[0x09] = ("right", [row[::-1] for row in SYMBOLS[0x03][1]])

# Rows where thresholding lands a pixel wrong. A diagonal's coverage falls
# either side of the cut depending on where it crosses the grid, so a shape the
# typeface draws symmetric comes out lopsided. Each row here is its own mirror.
PATCHES = {
    ord("8"): {18: "...##########..."},
    ord("x"): {7: ".###........###.", 10: "....###..###....", 18: "....###..###...."},
    ord("*"): {9: "...##..##..##..."},
}


def largest_fitting_size(path):
    """The biggest size whose ink still fits a cell, for every character."""
    best = None
    for size in range(8, 48):
        font = ImageFont.truetype(path, size)
        w = h = 0
        for code in range(33, LAST + 1):
            mask = font.getmask(chr(code), mode="1")
            w, h = max(w, mask.size[0]), max(h, mask.size[1])
        if w <= CELL_W and h <= CELL_H:
            best = size
    if best is None:
        sys.exit(f"{path}: no size fits {CELL_W}x{CELL_H}")
    return best


def baseline_for(font):
    """Sit the glyphs so ascenders and descenders both land inside the cell."""
    top = bottom = 0
    for code in range(33, LAST + 1):
        _, y0, _, y1 = font.getbbox(chr(code), anchor="ls")
        top, bottom = min(top, y0), max(bottom, y1)
    return (CELL_H - (bottom - top)) // 2 - top


def render(path, size):
    font = ImageFont.truetype(path, size)
    base = baseline_for(font)
    x = round((CELL_W - font.getlength("M")) / 2)

    blank = ["." * CELL_W] * CELL_H
    glyphs = {}
    for code in range(FIRST, 32):
        glyphs[code] = SYMBOLS[code][1] if code in SYMBOLS else list(blank)

    for code in range(32, LAST + 1):
        cell = Image.new("L", (CELL_W, CELL_H), 0)
        ImageDraw.Draw(cell).text((x, base), chr(code), font=font, fill=255, anchor="ls")
        px = cell.load()
        glyphs[code] = [
            "".join("#" if px[cx, cy] >= 128 else "." for cx in range(CELL_W))
            for cy in range(CELL_H)
        ]
        for row, bits in PATCHES.get(code, {}).items():
            glyphs[code][row] = bits
    return glyphs


def emit(glyphs, source, size, out):
    label = {ord("*"): "star", ord("/"): "slash"}
    out.write(f'''/**
 * @file font16x24.c
 * @brief 16x24 bitmap font
 *
 * Generated by tools/genfont.py from {source} at {size}px.
 * Edit that font, or the generator, rather than this file.
 */

#include "font16x24.h"

#include <stdbool.h>

#define FIRST_CHAR {FIRST}
#define LAST_CHAR {LAST}
#define GLYPH_COUNT (LAST_CHAR - FIRST_CHAR + 1)

/* '#' is a lit pixel, '.' is unlit. Leftmost character is the leftmost pixel. */
static const char* const GLYPHS[GLYPH_COUNT][QDOS_FONT_H] = {{
''')
    for code in range(FIRST, LAST + 1):
        out.write(f"\t/* {label.get(code, chr(code))} */ {{\n")
        for row in glyphs[code]:
            out.write(f'\t\t"{row}",\n')
        out.write("\t},\n")
    out.write('''};

static uint16_t packed[GLYPH_COUNT][QDOS_FONT_H];
static bool packed_ready;

static void pack_glyphs(void) {
\tfor (int i = 0; i < GLYPH_COUNT; i++) {
\t\tfor (int y = 0; y < QDOS_FONT_H; y++) {
\t\t\tuint16_t bits = 0;
\t\t\tfor (int x = 0; x < QDOS_FONT_W; x++) {
\t\t\t\tif (GLYPHS[i][y][x] == '#')
\t\t\t\t\tbits |= (uint16_t)(1u << x);
\t\t\t}
\t\t\tpacked[i][y] = bits;
\t\t}
\t}
\tpacked_ready = true;
}

uint16_t qdos_font_row(char ch, int row) {
\tif (row < 0 || row >= QDOS_FONT_H)
\t\treturn 0;

\tconst unsigned char c = (unsigned char)ch;
\tif (c < FIRST_CHAR || c > LAST_CHAR)
\t\treturn 0;

\tif (!packed_ready)
\t\tpack_glyphs();

\treturn packed[c - FIRST_CHAR][row];
}
''')


def main():
    path = sys.argv[1]
    size = int(sys.argv[2]) if len(sys.argv) > 2 else largest_fitting_size(path)
    with open("src/ui/font16x24.c", "w") as out:
        emit(render(path, size), path.split("/")[-1], size, out)
    print(f"src/ui/font16x24.c from {path} at {size}px")


if __name__ == "__main__":
    main()
