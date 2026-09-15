/**
 * @file font16x24.h
 * @brief 16x24 bitmap font covering printable ASCII
 *
 * Drawn for the calculator's 2.7 inch panel, where a cell is 2.35 x 3.53mm.
 * Cap height is 16 rows (3 through 18), x-height 11, descenders to row 22,
 * stems 2px, and the body sits in columns 2 through 13 with even bearings.
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
