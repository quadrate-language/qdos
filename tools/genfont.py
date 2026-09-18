#!/usr/bin/env python3
"""Pack assets/font16x24.txt into src/ui/font16x24.c.

The bitmap is the source. It was rasterised from a TTF once, but a 16x24 cell
at one bit is small enough that the rasteriser had to be corrected glyph by
glyph -- a stroke either lands on the pixel grid or smears across it -- so the
corrected pixels are what is kept and edited now. See assets/fonts/README.md.

    tools/genfont.py                     # pack, checking the metrics
    tools/genfont.py --check             # check only, write nothing
"""

import re
import sys

SOURCE = "assets/font16x24.txt"
OUTPUT = "src/ui/font16x24.c"

CELL_W, CELL_H = 16, 24
FIRST, LAST = 1, 126

# The metrics every glyph is held to. A bitmap font is only as good as its
# consistency: one glyph a pixel off the baseline is visible in a word even
# though a single pixel is not.
BASELINE = 21
CAP_TOP = 3
BODY_LEFT, BODY_RIGHT = 1, 14

# Codes 1 to 9 are symbols, not text: an arrow spans the cell so it reads as an
# arrow, which the body rule would refuse.
SYMBOL_LAST = 9

# A stem paired with another -- the two sides of H, n, O -- is 3px, sitting in
# columns 1-3 and 12-14. A stem standing alone in the middle of the cell cannot
# be: a run of columns mirrors about the centre only when its first and last
# add to 15, which no odd width satisfies in a cell 16 wide. So a centred stem
# is 2px in columns 7-8, and every one in the font is. Widening one to match
# the paired stems would put it off centre, which is worse than light.
CENTRED_STEM = (7, 8)
STEM_MAX_W = 5      # wider than this is a bar, not a stem
STEM_MIN_ROWS = 5   # shorter than this is a diagonal crossing the middle

# Only letters and digits are held to the reading line. Punctuation sits where
# its shape wants -- a hyphen in the middle, a quote at the top -- and a symbol
# below space is centred in the cell rather than sitting on a baseline at all.
#
# This typeface has no descent: g j p q y stop on the baseline like the rest,
# which is why the cell needs no room below it.
LETTERS = (
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
)

# Only these have a flat top to line up. Lowercase runs from ascenders down to
# the dot on an i, so a single cap line means nothing there.
CAPPED = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

# The underscore joins up between columns, so it is drawn the full width of the
# cell. The symbols below space are not text and are sized to read as symbols.
FULL_WIDTH = "_"


PIXELS = re.compile(r"[.#]{%d}" % CELL_W)
HEADER = re.compile(r"0x([0-9a-fA-F]{2})\s+(\S.*)")


def load(path):
    """Read the bitmap source. Returns {code: [24 rows of 16 chars]}."""
    glyphs = {}
    code = None
    rows = []
    lineno = 0

    for lineno, raw in enumerate(open(path), 1):
        line = raw.rstrip("\n")

        # Pixels first: a lit pixel at the start of a row looks exactly like a
        # comment marker, and a row is the one thing here with a fixed shape
        if PIXELS.fullmatch(line):
            if code is None:
                sys.exit(f"{path}:{lineno}: pixels before any glyph header")
            rows.append(line)
            continue

        header = HEADER.fullmatch(line)
        if header:
            if code is not None:
                glyphs[code] = finish(code, rows, path, lineno)
            code, rows = int(header.group(1), 16), []
            continue

        if not line or line.startswith("#"):
            continue

        sys.exit(f"{path}:{lineno}: not a header, a comment or {CELL_W} pixels: {line!r}")

    if code is not None:
        glyphs[code] = finish(code, rows, path, lineno)

    missing = [c for c in range(FIRST, LAST + 1) if c not in glyphs]
    if missing:
        sys.exit(f"{path}: no glyph for " + ", ".join(f"0x{c:02x}" for c in missing))
    return glyphs


def finish(code, rows, path, lineno):
    if len(rows) != CELL_H:
        sys.exit(f"{path}:{lineno}: glyph 0x{code:02x} has {len(rows)} rows, want {CELL_H}")
    return rows


def ink_rows(rows):
    return [y for y, r in enumerate(rows) if "#" in r]


def centred_stems(rows):
    """Count the rows of each narrow, centred, alone-in-its-row run."""
    found = {}
    for row in rows:
        runs = [(m.start(), m.end() - 1) for m in re.finditer(r"#+", row)]
        if len(runs) != 1:
            continue  # a crossbar or a bowl is beside it, so this is not a bare stem

        a, b = runs[0]
        if a + b == CELL_W - 1 and b - a + 1 <= STEM_MAX_W:
            found[(a, b)] = found.get((a, b), 0) + 1
    return found


def check(glyphs):
    """Report every glyph that breaks the metrics. Returns a list of problems."""
    problems = []

    for ch in LETTERS:
        rows = glyphs[ord(ch)]
        ys = ink_rows(rows)
        if not ys:
            problems.append(f"'{ch}' is blank")
            continue

        bottom = max(ys)
        if bottom != BASELINE:
            where = "above" if bottom < BASELINE else "below"
            problems.append(f"'{ch}' sits {where} the baseline: ends row {bottom}, want {BASELINE}")

        if ch in CAPPED and min(ys) != CAP_TOP:
            problems.append(f"'{ch}' starts row {min(ys)}, off the cap line {CAP_TOP}")

    # A stem standing alone in the cell is 2px in columns 7-8, always
    for code in range(SYMBOL_LAST + 1, LAST + 1):
        for (a, b), rows in centred_stems(glyphs[code]).items():
            if rows >= STEM_MIN_ROWS and (a, b) != CENTRED_STEM:
                problems.append(
                    f"'{chr(code)}' has a centred stem in columns {a}-{b} over {rows} rows; "
                    f"it can only mirror in {CENTRED_STEM[0]}-{CENTRED_STEM[1]}"
                )

    # Text glyphs keep inside the body, so adjacent cells never touch
    for code in range(SYMBOL_LAST + 1, LAST + 1):
        xs = [x for r in glyphs[code] for x, c in enumerate(r) if c == "#"]
        if not xs or chr(code) in FULL_WIDTH:
            continue
        if min(xs) < BODY_LEFT or max(xs) > BODY_RIGHT:
            name = chr(code) if 0x21 <= code <= 0x7E else f"0x{code:02x}"
            problems.append(
                f"'{name}' uses columns {min(xs)}-{max(xs)}, "
                f"outside the body {BODY_LEFT}-{BODY_RIGHT}"
            )

    return problems


def emit(glyphs, out):
    label = {ord("*"): "star", ord("/"): "slash", ord(" "): "space"}
    out.write(f'''/**
 * @file font16x24.c
 * @brief 16x24 bitmap font
 *
 * Generated by tools/genfont.py from {SOURCE}. Edit the bitmap, not this file.
 */

/* A row per glyph is what makes this readable at all. */
/* clang-format off */

#include "font16x24.h"

#include <stdbool.h>

#define FIRST_CHAR {FIRST}
#define LAST_CHAR {LAST}
#define GLYPH_COUNT (LAST_CHAR - FIRST_CHAR + 1)

/* '#' is a lit pixel, '.' is unlit. Leftmost character is the leftmost pixel. */
static const char* const GLYPHS[GLYPH_COUNT][QDOS_FONT_H] = {{
''')
    for code in range(FIRST, LAST + 1):
        name = label.get(code, chr(code) if 0x21 <= code <= 0x7E else f"0x{code:02x}")
        out.write(f"\t/* {name} */ {{\n")
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
    check_only = "--check" in sys.argv[1:]
    glyphs = load(SOURCE)

    problems = check(glyphs)
    for p in problems:
        print(f"  {p}", file=sys.stderr)

    if check_only:
        print(f"{SOURCE}: {len(problems)} off the metrics", file=sys.stderr)
        return 1 if problems else 0

    with open(OUTPUT, "w") as out:
        emit(glyphs, out)
    print(f"{OUTPUT} from {SOURCE} ({len(problems)} off the metrics)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
