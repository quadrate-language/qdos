/**
 * @file console.c
 * @brief Character console over a grayscale framebuffer
 */

#include "console.h"

#include <string.h>

/** Default levels: dark text on a light ground, like a reflective LCD. */
#define DEFAULT_INK 0x10
#define DEFAULT_PAPER 0xD8

void qdos_console_init(qdos_console* con) {
	if (!con)
		return;
	con->ink = DEFAULT_INK;
	con->paper = DEFAULT_PAPER;
	qdos_console_clear(con);
}

void qdos_console_clear(qdos_console* con) {
	if (!con)
		return;
	memset(con->fb, con->paper, sizeof(con->fb));
}

void qdos_console_putc(qdos_console* con, int col, int row, char ch) {
	if (!con || col < 0 || row < 0 || col >= QDOS_COLS || row >= QDOS_ROWS)
		return;

	const int x0 = col * QDOS_FONT_W;
	const int y0 = row * QDOS_FONT_H;

	for (int y = 0; y < QDOS_FONT_H; y++) {
		const uint8_t bits = qdos_font_row(ch, y);
		uint8_t* line = &con->fb[(size_t)(y0 + y) * QDOS_SCREEN_W + x0];
		for (int x = 0; x < QDOS_FONT_W; x++)
			line[x] = (bits & (1u << x)) ? con->ink : con->paper;
	}
}

int qdos_console_puts(qdos_console* con, int col, int row, const char* text) {
	if (!con || !text)
		return 0;

	int drawn = 0;
	for (const char* p = text; *p && col + drawn < QDOS_COLS; p++, drawn++)
		qdos_console_putc(con, col + drawn, row, *p);
	return drawn;
}

void qdos_console_puts_right(qdos_console* con, int row, const char* text) {
	if (!con || !text)
		return;

	const size_t len = strlen(text);
	if (len >= QDOS_COLS) {
		// Keep the tail: that is where the recent characters are.
		qdos_console_puts(con, 0, row, text + (len - QDOS_COLS));
		return;
	}
	qdos_console_puts(con, QDOS_COLS - (int)len, row, text);
}

void qdos_console_invert(qdos_console* con, int col, int row, int count) {
	if (!con || row < 0 || row >= QDOS_ROWS)
		return;

	for (int c = col; c < col + count; c++) {
		if (c < 0 || c >= QDOS_COLS)
			continue;
		for (int y = 0; y < QDOS_FONT_H; y++) {
			uint8_t* line = &con->fb[(size_t)(row * QDOS_FONT_H + y) * QDOS_SCREEN_W + c * QDOS_FONT_W];
			for (int x = 0; x < QDOS_FONT_W; x++)
				line[x] = (uint8_t)(0xFF - line[x]);
		}
	}
}

void qdos_console_rule(qdos_console* con, int row) {
	if (!con || row < 0 || row >= QDOS_ROWS)
		return;

	const int y = row * QDOS_FONT_H + QDOS_FONT_H - 1;
	memset(&con->fb[(size_t)y * QDOS_SCREEN_W], con->ink, QDOS_SCREEN_W);
}

void qdos_console_puts_centered(qdos_console* con, int row, const char* text, int scale) {
	if (!con || !text || scale < 1)
		return;

	const int len = (int)strlen(text);
	const int width = len * QDOS_FONT_W * scale;
	const int x0 = (QDOS_SCREEN_W - width) / 2;
	const int y0 = row * QDOS_FONT_H;

	for (int i = 0; i < len; i++) {
		for (int gy = 0; gy < QDOS_FONT_H; gy++) {
			const uint8_t bits = qdos_font_row(text[i], gy);
			for (int gx = 0; gx < QDOS_FONT_W; gx++) {
				if (!(bits & (1u << gx)))
					continue;

				// One font pixel becomes a scale x scale block
				for (int sy = 0; sy < scale; sy++) {
					const int y = y0 + gy * scale + sy;
					if (y < 0 || y >= QDOS_SCREEN_H)
						continue;
					for (int sx = 0; sx < scale; sx++) {
						const int x = x0 + (i * QDOS_FONT_W + gx) * scale + sx;
						if (x < 0 || x >= QDOS_SCREEN_W)
							continue;
						con->fb[(size_t)y * QDOS_SCREEN_W + x] = con->ink;
					}
				}
			}
		}
	}
}
