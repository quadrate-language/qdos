/**
 * @file font16x24.h
 * @brief 16x24 bitmap font covering printable ASCII
 *
 * Drawn for the calculator's 2.7 inch panel, where a cell is 2.35 x 3.53mm.
 * Cap height is 19 rows (3 through 21), x-height 16 (6 through 21), stems 3px,
 * and the body sits in columns 1 through 14 with even bearings. The typeface
 * has no descent, so nothing reaches past the baseline; only the underscore
 * uses row 22.
 */

#ifndef QDOS_FONT16X24_H
#define QDOS_FONT16X24_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Glyph cell width in pixels */
#define QDOS_FONT_W 16
/** @brief Glyph cell height in pixels */
#define QDOS_FONT_H 24

/*
 * Symbols with no ASCII of their own, drawn in tools/genfont.py rather than
 * taken from the typeface. They sit below space, so a label is still a plain
 * C string and this function still takes a char.
 */
#define QDOS_GLYPH_UP "\x01"
#define QDOS_GLYPH_DOWN "\x02"
#define QDOS_GLYPH_LEFT "\x03"
#define QDOS_GLYPH_SQRT "\x04"
#define QDOS_GLYPH_DIVIDE "\x05"
#define QDOS_GLYPH_TIMES "\x06"
#define QDOS_GLYPH_PLUSMINUS "\x07"
#define QDOS_GLYPH_PI "\x08"
#define QDOS_GLYPH_RIGHT "\x09"

/** @brief First and last drawn symbol, for walking them */
#define QDOS_GLYPH_FIRST 1
#define QDOS_GLYPH_LAST 9

/**
 * @brief Row bitmap for one character
 * @param ch  Character to look up
 * @param row Row index, 0 through QDOS_FONT_H - 1
 * @return Bitmask of lit pixels in that row, bit 0 leftmost
 */
uint16_t qdos_font_row(char ch, int row);

#ifdef __cplusplus
}
#endif

#endif // QDOS_FONT16X24_H
