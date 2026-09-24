/**
 * @file console.c
 * @brief Character console over a grayscale framebuffer
 */

#include "console.h"

#include <stdbool.h>
#include <string.h>

/** Default levels: dark text on a light ground, like a reflective LCD. */
#define DEFAULT_INK 0x10
#define DEFAULT_PAPER 0xD8

void qdos_console_init(qdos_console* con) {
	if (!con) {
		return;
	}
	con->ink = DEFAULT_INK;
	con->paper = DEFAULT_PAPER;
	qdos_console_clear(con);
}

void qdos_console_clear(qdos_console* con) {
	if (!con) {
		return;
	}
	memset(con->fb, con->paper, sizeof(con->fb));
}

/**
 * @brief Draw one glyph at a pixel position, each font pixel a scale x scale block
 * @param paper_too Fill the background as well, so cells overwrite cleanly
 */
static void blit_glyph(qdos_console* con, int x0, int y0, char ch, int scale, bool paper_too) {
	for (int gy = 0; gy < QDOS_FONT_H; gy++) {
		const uint16_t bits = qdos_font_row(ch, gy);
		for (int gx = 0; gx < QDOS_FONT_W; gx++) {
			const bool on = (bits & (1u << gx)) != 0;
			if (!on && !paper_too) {
				continue;
			}

			const uint8_t level = on ? con->ink : con->paper;
			for (int sy = 0; sy < scale; sy++) {
				const int y = y0 + gy * scale + sy;
				if (y < 0 || y >= QDOS_SCREEN_H) {
					continue;
				}
				for (int sx = 0; sx < scale; sx++) {
					const int x = x0 + gx * scale + sx;
					if (x < 0 || x >= QDOS_SCREEN_W) {
						continue;
					}
					con->fb[(size_t)y * QDOS_SCREEN_W + x] = level;
				}
			}
		}
	}
}

void qdos_console_putc(qdos_console* con, int col, int row, char ch) {
	if (!con || col < 0 || row < 0 || col >= QDOS_COLS || row >= QDOS_ROWS) {
		return;
	}

	blit_glyph(con, col * QDOS_CELL_W, row * QDOS_CELL_H, ch, QDOS_FONT_SCALE, true);
}

int qdos_console_puts(qdos_console* con, int col, int row, const char* text) {
	if (!con || !text) {
		return 0;
	}

	int drawn = 0;
	for (const char* p = text; *p && col + drawn < QDOS_COLS; p++, drawn++) {
		qdos_console_putc(con, col + drawn, row, *p);
	}
	return drawn;
}

void qdos_console_puts_right_within(qdos_console* con, int row, int from, const char* text) {
	if (!con || !text || from < 0 || from >= QDOS_COLS) {
		return;
	}

	const int room = QDOS_COLS - from;
	const size_t len = strlen(text);

	// The tail is the wrong end of a number to keep: dropping the leading
	// digits leaves something that still reads as an answer.
	if (len > (size_t)room) {
		char cut[QDOS_COLS + 1];
		const int keep = room - 1;
		memcpy(cut, text, (size_t)keep);
		cut[keep] = QDOS_ELIDED;
		cut[room] = '\0';
		qdos_console_puts(con, from, row, cut);
		return;
	}
	qdos_console_puts(con, QDOS_COLS - (int)len, row, text);
}

void qdos_console_puts_right(qdos_console* con, int row, const char* text) {
	qdos_console_puts_right_within(con, row, 0, text);
}

void qdos_console_invert(qdos_console* con, int col, int row, int count) {
	if (!con || row < 0 || row >= QDOS_ROWS) {
		return;
	}

	for (int c = col; c < col + count; c++) {
		if (c < 0 || c >= QDOS_COLS) {
			continue;
		}
		for (int y = 0; y < QDOS_CELL_H; y++) {
			uint8_t* line = &con->fb[(size_t)(row * QDOS_CELL_H + y) * QDOS_SCREEN_W + c * QDOS_CELL_W];
			for (int x = 0; x < QDOS_CELL_W; x++) {
				line[x] = (uint8_t)(0xFF - line[x]);
			}
		}
	}
}

void qdos_console_rule(qdos_console* con, int row) {
	if (!con || row < 0 || row >= QDOS_ROWS) {
		return;
	}

	const int y = row * QDOS_CELL_H + QDOS_CELL_H - 1;
	memset(&con->fb[(size_t)y * QDOS_SCREEN_W], con->ink, QDOS_SCREEN_W);
}

void qdos_console_putc_small(qdos_console* con, int x, int y, char ch) {
	if (!con) {
		return;
	}

	for (int gy = 0; gy < QDOS_SMALL_FONT_H; gy++) {
		const int py = y + gy;
		if (py < 0 || py >= QDOS_SCREEN_H) {
			continue;
		}
		const uint8_t bits = qdos_small_font_row(ch, gy);
		for (int gx = 0; gx < QDOS_SMALL_FONT_W; gx++) {
			const int px = x + gx;
			if (px < 0 || px >= QDOS_SCREEN_W) {
				continue;
			}
			con->fb[(size_t)py * QDOS_SCREEN_W + px] = (bits & (1u << gx)) ? con->ink : con->paper;
		}
	}
}

void qdos_console_puts_small(qdos_console* con, int x, int y, const char* text) {
	if (!con || !text) {
		return;
	}

	for (const char* p = text; *p && x + QDOS_SMALL_FONT_W <= QDOS_SCREEN_W; p++, x += QDOS_SMALL_FONT_W) {
		qdos_console_putc_small(con, x, y, *p);
	}
}

void qdos_console_invert_rect(qdos_console* con, int x, int y, int w, int h) {
	if (!con) {
		return;
	}

	for (int py = y; py < y + h; py++) {
		if (py < 0 || py >= QDOS_SCREEN_H) {
			continue;
		}
		for (int px = x; px < x + w; px++) {
			if (px < 0 || px >= QDOS_SCREEN_W) {
				continue;
			}
			uint8_t* p = &con->fb[(size_t)py * QDOS_SCREEN_W + px];
			*p = (uint8_t)(0xFF - *p);
		}
	}
}

void qdos_console_puts_at(qdos_console* con, int x, int y, const char* text, int scale) {
	if (!con || !text || scale < 1) {
		return;
	}
	for (const char* p = text; *p; p++, x += QDOS_FONT_W * scale) {
		blit_glyph(con, x, y, *p, scale, false);
	}
}

void qdos_console_puts_centered(qdos_console* con, int row, const char* text, int scale) {
	if (!con || !text || scale < 1) {
		return;
	}

	const int len = (int)strlen(text);
	const int x0 = (QDOS_SCREEN_W - len * QDOS_FONT_W * scale) / 2;

	for (int i = 0; i < len; i++) {
		blit_glyph(con, x0 + i * QDOS_FONT_W * scale, row * QDOS_CELL_H, text[i], scale, false);
	}
}
