/**
 * @file keypad_ui.c
 * @brief The simulator's on-screen keypad
 */

#include "keypad_ui.h"

#include "ui/font16x24.h"

#include <string.h>

#define KEY(l, k) {(l), (k), NULL}
#define TXT(l, t) {(l), QDOS_KEY_NONE, (t)}
#define NONE {NULL, QDOS_KEY_NONE, NULL}

/*
 * Laid out like a SwissMicros DM42: the enter row, then digits in a 3x3 block
 * with the operators down the right and navigation down the left, sitting at
 * the bottom where a thumb expects them. The menu keys stay under the display.
 *
 * A cap in lower case is exactly the word it stands for, so it can be typed as
 * it is written; a cap in capitals is QDOS's own -- an abbreviation too long to
 * print in full, or a key the language has no name for.
 *
 * Three faces, as a TI-83 has two modifiers rather than one. Plain is a
 * calculator. ALP is the letters, without which no name or string can be typed
 * at all, and it locks, because words are more than one letter long. SYM is
 * Quadrate's syntax, which cannot be reached any other way; it lasts one press
 * and hands back to whichever layer was showing. Words that are neither stay
 * off the keypad entirely -- CAT lists every one of them.
 */
static const qdos_pad_button KEYPAD[QDOS_PAD_ROWS][QDOS_PAD_COLS] = {
	{{KEY("F1", QDOS_KEY_SOFT1), NONE, NONE},
			{KEY("F2", QDOS_KEY_SOFT2), NONE, NONE},
			{KEY("F3", QDOS_KEY_SOFT3), NONE, NONE},
			{KEY("F4", QDOS_KEY_SOFT4), NONE, NONE},
			{KEY("F5", QDOS_KEY_SOFT5), NONE, NONE}},

	{{KEY("sin", QDOS_KEY_SIN), TXT("A", "a"), TXT("(", "(")},
			{KEY("cos", QDOS_KEY_COS), TXT("B", "b"), TXT(")", ")")},
			{KEY("tan", QDOS_KEY_TAN), TXT("C", "c"), TXT("{", "{")},
			{KEY("ln", QDOS_KEY_LN), TXT("D", "d"), TXT("}", "}")},
			{KEY("log", QDOS_KEY_LOG), TXT("E", "e"), TXT("\"", "\"")}},

	{{KEY(QDOS_GLYPH_SQRT, QDOS_KEY_SQRT), TXT("F", "f"), TXT("fn", "fn ")},
			{KEY("sq", QDOS_KEY_SQ), TXT("G", "g"), TXT("--", " -- ")},
			{KEY("pow", QDOS_KEY_POW), TXT("H", "h"), TXT("i64", "i64")},
			{KEY("inv", QDOS_KEY_INV), TXT("I", "i"), TXT("f64", "f64")},
			{KEY("abs", QDOS_KEY_ABS), TXT("J", "j"), TXT("str", "str")}},

	{{KEY("FLR", QDOS_KEY_FLOOR), TXT("K", "k"), TXT("if", "if ")},
			{KEY("ceil", QDOS_KEY_CEIL), TXT("L", "l"), TXT("else", " else ")},
			{KEY("RND", QDOS_KEY_ROUND), TXT("M", "m"), TXT("loop", "loop ")},
			{TXT("SPC", " "), NONE, TXT("BRK", " break ")},
			{KEY("SYM", QDOS_KEY_NONE), NONE, NONE}},

	// Where the DM42 keeps sto, rcl and roll down
	{{KEY("dup", QDOS_KEY_DUP), TXT("N", "n"), TXT("nip", " nip ")},
			{KEY("drop", QDOS_KEY_DROP), TXT("O", "o"), KEY("UNDO", QDOS_KEY_UNDO)},
			{KEY("over", QDOS_KEY_OVER), TXT("P", "p"), TXT("[", "[")},
			{KEY("rot", QDOS_KEY_ROT), TXT("Q", "q"), TXT("]", "]")},
			{KEY("mod", QDOS_KEY_MOD), TXT("R", "r"), TXT("=", "=")}},

	// enter, x<>y, +/-, e, backspace
	{{KEY("ENT", QDOS_KEY_ENTER), NONE, TXT("APP", " append ")},
			{KEY("swap", QDOS_KEY_SWAP), TXT("S", "s"), TXT("roll", " roll ")},
			{KEY(QDOS_GLYPH_PLUSMINUS, QDOS_KEY_NEG), TXT("T", "t"), TXT("free", " free ")},
			{KEY("TAB", QDOS_KEY_TAB), TXT("U", "u"), TXT("<", "<")},
			{KEY("BKS", QDOS_KEY_BACKSPACE), NONE, TXT(">", ">")}},

	{{KEY(QDOS_GLYPH_UP, QDOS_KEY_UP), NONE, KEY(QDOS_GLYPH_LEFT, QDOS_KEY_LEFT)},
			{KEY("7", QDOS_KEY_7), TXT("V", "v"), TXT("and", " and ")},
			{KEY("8", QDOS_KEY_8), TXT("W", "w"), TXT("or", " or ")},
			{KEY("9", QDOS_KEY_9), TXT("X", "x"), TXT("xor", " xor ")},
			{KEY(QDOS_GLYPH_DIVIDE, QDOS_KEY_DIV), TXT("Y", "y"), TXT("shl", " shl ")}},

	{{KEY(QDOS_GLYPH_DOWN, QDOS_KEY_DOWN), NONE, KEY(QDOS_GLYPH_RIGHT, QDOS_KEY_RIGHT)},
			{KEY("4", QDOS_KEY_4), TXT("Z", "z"), TXT("not", " not ")},
			{KEY("5", QDOS_KEY_5), TXT("_", "_"), TXT("==", " == ")},
			{KEY("6", QDOS_KEY_6), NONE, TXT("!=", " != ")},
			{KEY(QDOS_GLYPH_TIMES, QDOS_KEY_MUL), NONE, TXT(",", ",")}},

	{{KEY("ALP", QDOS_KEY_NONE), NONE, NONE},
			{KEY("1", QDOS_KEY_1), NONE, TXT("<=", " <= ")},
			{KEY("2", QDOS_KEY_2), NONE, TXT(">=", " >= ")},
			{KEY("3", QDOS_KEY_3), NONE, TXT(QDOS_GLYPH_PI, "pi ")},
			{KEY("-", QDOS_KEY_SUB), NONE, TXT("INC", " ++ ")}},

	// exit, with off on its symbol layer as on the DM42
	{{KEY("ESC", QDOS_KEY_CLEAR), NONE, KEY("PWR", QDOS_KEY_POWER)},
			{KEY("0", QDOS_KEY_0), NONE, TXT("len", " len ")},
			{KEY(".", QDOS_KEY_DOT), NONE, TXT("nth", " nth ")},
			{TXT(":", ":"), NONE, TXT(";", ";")},
			{KEY("+", QDOS_KEY_ADD), NONE, TXT("DEC", " -- ")}},
};

#undef KEY
#undef TXT
#undef NONE

static const uint8_t PAD_BG[3] = {0x22, 0x24, 0x22};
static const uint8_t FACE[3] = {0x45, 0x48, 0x45};
static const uint8_t FACE_SHIFTED[3] = {0x3A, 0x4E, 0x42};
static const uint8_t EDGE[3] = {0x5E, 0x62, 0x5E};
static const uint8_t TEXT[3] = {0xC9, 0xCE, 0xC6};
static const uint8_t TEXT_DIM[3] = {0x76, 0x7A, 0x74};

bool qdos_pad_modifier(const qdos_pad_button* b, qdos_pad_layer* selects) {
	if (b == NULL || b->plain.label == NULL)
		return false;

	if (strcmp(b->plain.label, "ALP") == 0) {
		*selects = QDOS_PAD_ALPHA;
		return true;
	}
	if (strcmp(b->plain.label, "SYM") == 0) {
		*selects = QDOS_PAD_SYMBOL;
		return true;
	}
	return false;
}

const qdos_pad_action* qdos_pad_action_for(const qdos_pad_button* b, qdos_pad_layer layer) {
	if (b == NULL)
		return NULL;

	if (layer == QDOS_PAD_SYMBOL)
		return b->symbol.label != NULL ? &b->symbol : NULL;

	// Alpha only replaces the buttons carrying a letter; the rest keep working,
	// or a locked layer would leave you with no Enter and no backspace
	if (layer == QDOS_PAD_ALPHA && b->alpha.label != NULL)
		return &b->alpha;

	return b->plain.label != NULL ? &b->plain : NULL;
}

/** @brief What this button shows on @p layer, and whether it does anything there */
static const qdos_pad_action* face_of(const qdos_pad_button* b, qdos_pad_layer layer) {
	qdos_pad_layer ignored;
	if (qdos_pad_modifier(b, &ignored))
		return &b->plain;
	return qdos_pad_action_for(b, layer);
}

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

static void label(uint8_t* rgb, int stride_px, int x0, int y0, const char* text, const uint8_t* c) {
	for (int i = 0; text[i] != '\0'; i++) {
		for (int gy = 0; gy < QDOS_FONT_H; gy++) {
			const uint16_t bits = qdos_font_row(text[i], gy);
			for (int gx = 0; gx < QDOS_FONT_W; gx++) {
				if (bits & (uint16_t)(1u << gx))
					put_pixel(rgb, stride_px, x0 + i * QDOS_FONT_W + gx, y0 + gy, c);
			}
		}
	}
}

void qdos_pad_draw(uint8_t* rgb, int stride_px, int y0, qdos_pad_layer layer) {
	fill(rgb, stride_px, 0, y0, stride_px, QDOS_PAD_H, PAD_BG);

	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = &KEYPAD[row][col];
			const int x = col * QDOS_PAD_BUTTON_W;
			const int y = y0 + row * QDOS_PAD_BUTTON_H;

			qdos_pad_layer selects;
			const bool is_modifier = qdos_pad_modifier(b, &selects);
			const qdos_pad_action* shown = face_of(b, layer);
			const bool live = shown != NULL;

			// The layer's own key is lit, so it is clear what is switched on
			const bool tinted = (layer != QDOS_PAD_PLAIN) && (is_modifier ? selects == layer : live);
			const uint8_t* face = tinted ? FACE_SHIFTED : FACE;

			fill(rgb, stride_px, x + 2, y + 2, QDOS_PAD_BUTTON_W - 4, QDOS_PAD_BUTTON_H - 4, EDGE);
			fill(rgb, stride_px, x + 3, y + 3, QDOS_PAD_BUTTON_W - 6, QDOS_PAD_BUTTON_H - 6, face);

			const char* text = live ? shown->label : b->plain.label;
			if (text == NULL)
				continue;

			const int len = (int)strlen(text);
			label(rgb, stride_px, x + (QDOS_PAD_BUTTON_W - len * QDOS_FONT_W) / 2,
					y + (QDOS_PAD_BUTTON_H - QDOS_FONT_H) / 2, text, live ? TEXT : TEXT_DIM);
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
