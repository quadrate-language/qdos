/**
 * @file padfont.h
 * @brief Antialiased keycap font for the simulator
 *
 * Separate from the panel font on purpose. The panel is one bit per pixel and
 * its font is a bitmap fitted to that grid, because that is what the hardware
 * can show. The simulator's case is 24-bit RGB and its keycaps are chrome, so
 * they can be antialiased -- which is the only way a face small enough to
 * leave room around a label stays legible.
 *
 * Codes 1 to 9 are the same symbols the panel font puts there, so a keycap in
 * the layout table reads the same whichever font draws it.
 */

#ifndef QDOS_PADFONT_H
#define QDOS_PADFONT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QDOS_PADFONT_FIRST 1
#define QDOS_PADFONT_LAST 126

/*
 * Symbols that are keycaps and nothing else. They sit above the QDOS_GLYPH_*
 * range the panel font shares, because the panel never draws them and the
 * shell scans that range looking for drawn symbols -- a blank glyph inside it
 * would match every blank cell on the display.
 */
#define QDOS_PAD_GLYPH_SQUARED "\x10"
#define QDOS_PAD_GLYPH_SPACE "\x11"

/**
 * @brief Which of the two faces
 *
 * A keycap and the shift legend printed above it cannot be the same size, or
 * the pair reads as two labels rather than a key and its annotation, so the
 * legend is set a good deal smaller.
 */
typedef enum {
	QDOS_PADFACE_CAP = 0, ///< Moulded into the key
	QDOS_PADFACE_SHIFT,	  ///< Printed on the case above it
	QDOS_PADFACE__COUNT
} qdos_padface;

/** @brief One glyph's bitmap size and where it sits relative to the pen */
typedef struct {
	uint8_t w; ///< Ink width in pixels
	uint8_t h; ///< Ink height
	int8_t bx; ///< Ink left edge, from the pen position
	int8_t by; ///< Ink top edge, from the baseline; negative is above it
	uint8_t advance;
	uint16_t offset; ///< Where this glyph starts in the coverage blob
} qdos_padglyph;

/** @brief NULL for a character this font has nothing for */
const qdos_padglyph* qdos_padfont_glyph(qdos_padface face, unsigned char ch);

/** @brief One byte of coverage per pixel, row-major, all glyphs end to end */
const uint8_t* qdos_padfont_coverage(qdos_padface face);

/** @brief Height of a capital, which is what a label is centred on */
int qdos_padfont_cap_height(qdos_padface face);

/** @brief How wide @p text sets in @p face, in pixels */
int qdos_padfont_advance(qdos_padface face, const char* text);

#ifdef __cplusplus
}
#endif

#endif // QDOS_PADFONT_H
