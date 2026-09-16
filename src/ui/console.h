/**
 * @file console.h
 * @brief Character console over a grayscale framebuffer
 */

#ifndef QDOS_CONSOLE_H
#define QDOS_CONSOLE_H

#include "font16x24.h"

#include <qdos/hal.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The font is drawn at reading size, so ordinary text is unscaled */
#define QDOS_FONT_SCALE 1

#define QDOS_CELL_W (QDOS_FONT_W * QDOS_FONT_SCALE)
#define QDOS_CELL_H (QDOS_FONT_H * QDOS_FONT_SCALE)
#define QDOS_COLS (QDOS_SCREEN_W / QDOS_CELL_W)
#define QDOS_ROWS (QDOS_SCREEN_H / QDOS_CELL_H)

typedef struct {
	uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H]; ///< Grayscale pixels, row-major
	uint8_t ink;							   ///< Foreground level
	uint8_t paper;							   ///< Background level
} qdos_console;

/** @brief Default ink and paper, and clear */
void qdos_console_init(qdos_console* con);

void qdos_console_clear(qdos_console* con);

void qdos_console_putc(qdos_console* con, int col, int row, char ch);

/** @brief Clipped at the right edge; returns characters drawn */
int qdos_console_puts(qdos_console* con, int col, int row, const char* text);

void qdos_console_puts_right(qdos_console* con, int row, const char* text);

/** @brief Marks text that did not fit, in its last cell */
#define QDOS_ELIDED '~'

/** @brief Right-aligned, using only the columns from @p from onwards */
void qdos_console_puts_right_within(qdos_console* con, int row, int from, const char* text);

/** @brief Invert cells, for the cursor and for errors */
void qdos_console_invert(qdos_console* con, int col, int row, int count);

void qdos_console_rule(qdos_console* con, int row);

/** @brief Centred, each pixel enlarged; scale 1 matches puts() */
void qdos_console_puts_centered(qdos_console* con, int row, const char* text, int scale);

#ifdef __cplusplus
}
#endif

#endif // QDOS_CONSOLE_H
