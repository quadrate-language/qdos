/**
 * @file font8x16.h
 * @brief 8x16 bitmap font covering printable ASCII, for the editor
 *
 * A program wants columns more than it wants reading size: at 16x24 the editor
 * shows 24 of them, which is less than most lines. This is Terminus 8x16, a
 * font drawn one bit at this cell, so it needs none of the correcting the panel
 * font did. The pixels are in assets/font8x16.txt and tools/gensmallfont.py
 * packs them here.
 */

#ifndef QDOS_FONT8X16_H
#define QDOS_FONT8X16_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QDOS_SMALL_FONT_W 8
#define QDOS_SMALL_FONT_H 16

/**
 * @brief Row bitmap for one character
 * @return Bitmask of lit pixels in that row, bit 0 leftmost; 0 outside ASCII
 */
uint8_t qdos_small_font_row(char ch, int row);

#ifdef __cplusplus
}
#endif

#endif // QDOS_FONT8X16_H
