/**
 * @file console.h
 * @brief Character console over a grayscale framebuffer
 *
 * Owns the pixel buffer the shell paints into and hands to the HAL. Coordinates
 * are in character cells, not pixels.
 */

#ifndef QDOS_CONSOLE_H
#define QDOS_CONSOLE_H

#include "font8x8.h"

#include <qdos/hal.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Console width in characters */
#define QDOS_COLS (QDOS_SCREEN_W / QDOS_FONT_W)
/** @brief Console height in characters */
#define QDOS_ROWS (QDOS_SCREEN_H / QDOS_FONT_H)

/**
 * @brief Console state — the framebuffer plus the pen
 */
typedef struct {
	uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H]; ///< Grayscale pixels, row-major
	uint8_t ink;							   ///< Foreground level
	uint8_t paper;							   ///< Background level
} qdos_console;

/**
 * @brief Reset to default ink and paper and clear the screen
 */
void qdos_console_init(qdos_console* con);

/**
 * @brief Fill the whole framebuffer with paper
 */
void qdos_console_clear(qdos_console* con);

/**
 * @brief Draw one character at a cell
 *
 * Cells outside the console are ignored.
 *
 * @param col Column, 0 through QDOS_COLS - 1
 * @param row Row, 0 through QDOS_ROWS - 1
 */
void qdos_console_putc(qdos_console* con, int col, int row, char ch);

/**
 * @brief Draw a string starting at a cell, clipped at the right edge
 *
 * @return Number of characters actually drawn
 */
int qdos_console_puts(qdos_console* con, int col, int row, const char* text);

/**
 * @brief Draw a string ending at the right edge of the console
 *
 * Used for the stack display, where the significant digits are at the end and
 * a long value should lose its head rather than its tail.
 */
void qdos_console_puts_right(qdos_console* con, int row, const char* text);

/**
 * @brief Invert a run of cells, for the cursor and for highlights
 */
void qdos_console_invert(qdos_console* con, int col, int row, int count);

/**
 * @brief Draw a horizontal rule across the console at a row
 */
void qdos_console_rule(qdos_console* con, int row);

#ifdef __cplusplus
}
#endif

#endif // QDOS_CONSOLE_H
