#!/usr/bin/env python3
"""Draw the kernel boot logo from the console font.

The logo is the first thing on screen, before Linux has a framebuffer driver,
and it should match the shell that follows. Reading src/ui/font16x24.c rather
than the TTF keeps it identical to what the console actually draws.

    tools/genlogo.py QDOS firmware/buildroot/board/logo.png
"""

import re
import sys
from PIL import Image

CELL_W, CELL_H, SCALE = 16, 24, 2
FIRST = 32


def glyphs():
    src = open("src/ui/font16x24.c").read()
    rows = re.findall(r'^\t\t"([.#]{16})",$', src, re.M)
    if len(rows) % CELL_H:
        sys.exit("font16x24.c: rows do not divide into glyphs")
    return [rows[i : i + CELL_H] for i in range(0, len(rows), CELL_H)]


def main():
    text, out = sys.argv[1], sys.argv[2]
    table = glyphs()

    img = Image.new("L", (len(text) * CELL_W * SCALE, CELL_H * SCALE), 0)
    px = img.load()
    for i, ch in enumerate(text):
        cell = table[ord(ch) - FIRST]
        for y in range(CELL_H):
            for x in range(CELL_W):
                if cell[y][x] != "#":
                    continue
                for sy in range(SCALE):
                    for sx in range(SCALE):
                        px[(i * CELL_W + x) * SCALE + sx, y * SCALE + sy] = 0xD8

    img.save(out)
    print(f"{out}: {text} at {img.width}x{img.height}")


if __name__ == "__main__":
    main()
