/**
 * @file keypad_ui.c
 * @brief The simulator's on-screen keypad
 */

#include "keypad_ui.h"

#include "ui/font16x24.h"

#include <string.h>

#define TXT(l, t) {(l), QDOS_KEY_NONE, (t)}
#define KEY(l, k) {(l), (k), NULL}

static const qdos_pad_button KEYPAD[QDOS_PAD_ROWS][QDOS_PAD_COLS] = {
	{KEY("pwr", QDOS_KEY_POWER), KEY("clr", QDOS_KEY_CLEAR), TXT(":", ":"),
			KEY("tab", QDOS_KEY_TAB), KEY("bks", QDOS_KEY_BACKSPACE)},
	{KEY("dup", QDOS_KEY_DUP), KEY("drp", QDOS_KEY_DROP), KEY("swp", QDOS_KEY_SWAP),
			TXT("ovr", " over "), TXT("rot", " rot ")},
	{KEY("7", QDOS_KEY_7), KEY("8", QDOS_KEY_8), KEY("9", QDOS_KEY_9),
			KEY("/", QDOS_KEY_DIV), TXT("%", " % ")},
	{KEY("4", QDOS_KEY_4), KEY("5", QDOS_KEY_5), KEY("6", QDOS_KEY_6),
			KEY("*", QDOS_KEY_MUL), TXT("neg", " neg ")},
	{KEY("1", QDOS_KEY_1), KEY("2", QDOS_KEY_2), KEY("3", QDOS_KEY_3),
			KEY("-", QDOS_KEY_SUB), TXT("and", " and ")},
	{KEY("0", QDOS_KEY_0), KEY(".", QDOS_KEY_DOT), KEY("ent", QDOS_KEY_ENTER),
			KEY("+", QDOS_KEY_ADD), TXT("or", " or ")},
	{TXT("(", "("), TXT(")", ")"), TXT("{", "{"), TXT("}", "}"), TXT("xor", " xor ")},
	{TXT("<", "<"), TXT(">", ">"), TXT("=", "="), TXT("\"", "\""), TXT("not", " not ")},
};

#undef TXT
#undef KEY

static const uint8_t PAD_BG[3] = {0x22, 0x24, 0x22};
static const uint8_t FACE[3] = {0x45, 0x48, 0x45};
static const uint8_t EDGE[3] = {0x5E, 0x62, 0x5E};
static const uint8_t TEXT[3] = {0xC9, 0xCE, 0xC6};

static void put_pixel(uint8_t* rgb, int stride_px, int x, int y, const uint8_t* c) {
	if (x < 0 || y < 0 || x >= stride_px)
		return;
	uint8_t* p = &rgb[((size_t)y * stride_px + x) * 3];
	p[0] = c[0];
	p[1] = c[1];
	p[2] = c[2];
}

static void fill(uint8_t* rgb, int stride_px, int x0, int y0, int w, int h, const uint8_t* c) {
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			put_pixel(rgb, stride_px, x, y, c);
}

static void label(uint8_t* rgb, int stride_px, int x0, int y0, const char* text) {
	for (int i = 0; text[i] != '\0'; i++) {
		for (int gy = 0; gy < QDOS_FONT_H; gy++) {
			const uint16_t bits = qdos_font_row(text[i], gy);
			for (int gx = 0; gx < QDOS_FONT_W; gx++) {
				if (bits & (uint16_t)(1u << gx))
					put_pixel(rgb, stride_px, x0 + i * QDOS_FONT_W + gx, y0 + gy, TEXT);
			}
		}
	}
}

void qdos_pad_draw(uint8_t* rgb, int stride_px, int y0) {
	fill(rgb, stride_px, 0, y0, stride_px, QDOS_PAD_H, PAD_BG);

	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = &KEYPAD[row][col];
			const int x = col * QDOS_PAD_BUTTON_W;
			const int y = y0 + row * QDOS_PAD_BUTTON_H;

			fill(rgb, stride_px, x + 2, y + 2, QDOS_PAD_BUTTON_W - 4, QDOS_PAD_BUTTON_H - 4, EDGE);
			fill(rgb, stride_px, x + 3, y + 3, QDOS_PAD_BUTTON_W - 6, QDOS_PAD_BUTTON_H - 6, FACE);

			const int len = (int)strlen(b->label);
			label(rgb, stride_px, x + (QDOS_PAD_BUTTON_W - len * QDOS_FONT_W) / 2,
					y + (QDOS_PAD_BUTTON_H - QDOS_FONT_H) / 2, b->label);
		}
	}
}

const qdos_pad_button* qdos_pad_at(int x, int y) {
	const int py = y - QDOS_SCREEN_H;
	if (py < 0 || x < 0 || x >= QDOS_SCREEN_W || py >= QDOS_PAD_H)
		return NULL;

	return &KEYPAD[py / QDOS_PAD_BUTTON_H][x / QDOS_PAD_BUTTON_W];
}

const qdos_pad_button* qdos_pad_button_at(int col, int row) {
	if (col < 0 || row < 0 || col >= QDOS_PAD_COLS || row >= QDOS_PAD_ROWS)
		return NULL;
	return &KEYPAD[row][col];
}
