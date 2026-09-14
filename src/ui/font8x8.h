/**
 * @file font8x8.h
 * @brief 8x8 bitmap font covering printable ASCII
 */

#ifndef QDOS_FONT8X8_H
#define QDOS_FONT8X8_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Glyph cell width in pixels */
#define QDOS_FONT_W 8
/** @brief Glyph cell height in pixels */
#define QDOS_FONT_H 8

/**
 * @brief Row bitmap for one character
 * @param ch  Character to look up
 * @param row Row index, 0 through QDOS_FONT_H - 1
 * @return Bitmask of lit pixels in that row
 */
uint8_t qdos_font_row(char ch, int row);

#ifdef __cplusplus
}
#endif

#endif // QDOS_FONT8X8_H
