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
 * the bottom where a thumb expects them. The menu keys stay under the display,
 * and the three rows between are Quadrate's, which no calculator has.
 * Unshifted is the calculator; shifted is the rest of the language.
 */
static const qdos_pad_button KEYPAD[QDOS_PAD_ROWS][QDOS_PAD_COLS] = {
	{{KEY("F1", QDOS_KEY_SOFT1), NONE},
			{KEY("F2", QDOS_KEY_SOFT2), NONE},
			{KEY("F3", QDOS_KEY_SOFT3), NONE},
			{KEY("F4", QDOS_KEY_SOFT4), NONE},
			{KEY("F5", QDOS_KEY_SOFT5), NONE}},

	{{TXT("(", "("), TXT("[", "[")},
			{TXT(")", ")"), TXT("]", "]")},
			{TXT("\"", "\""), TXT("'", "'")},
			{TXT("{", "{"), TXT("FN", "fn ")},
			{TXT("}", "}"), TXT("--", " -- ")}},

	{{TXT("<", "<"), TXT("I64", "i64")},
			{TXT(">", ">"), TXT("F64", "f64")},
			{TXT("=", "="), TXT("STR", "str")},
			{TXT("%", " % "), TXT("NL", " nl ")},
			{TXT("_", "_"), TXT("SET", " set ")}},

	// Two numbers in a row need a separator; operators and words carry their own
	{{TXT("SPC", " "), TXT("PRT", " print ")},
			{TXT("CLR", " clear "), TXT("ERR", " err ")},
			{TXT("IF", "if "), TXT("ELS", " else ")},
			{TXT("LOP", "loop "), NONE},
			{TXT("BRK", " break "), NONE}},

	// Where the DM42 keeps sto, rcl and roll down
	{{KEY("DUP", QDOS_KEY_DUP), TXT("NIP", " nip ")},
			{KEY("DRP", QDOS_KEY_DROP), TXT("PIK", " pick ")},
			{TXT("OVR", " over "), TXT("DP2", " dup2 ")},
			{TXT("ROT", " rot "), TXT("DEP", " depth ")},
			{KEY("LST", QDOS_KEY_LIST), KEY("SAV", QDOS_KEY_SAVE)}},

	// enter, x<>y, +/-, e, backspace
	{{KEY("ENT", QDOS_KEY_ENTER), TXT("APP", " append ")},
			{KEY("SWP", QDOS_KEY_SWAP), TXT("ROL", " roll ")},
			{TXT("NEG", " neg "), TXT("FRE", " free ")},
			{KEY("TAB", QDOS_KEY_TAB), TXT(",", ",")},
			{KEY("BKS", QDOS_KEY_BACKSPACE), NONE}},

	{{KEY("UP", QDOS_KEY_UP), KEY("LT", QDOS_KEY_LEFT)},
			{KEY("7", QDOS_KEY_7), TXT("AND", " and ")},
			{KEY("8", QDOS_KEY_8), TXT("OR", " or ")},
			{KEY("9", QDOS_KEY_9), TXT("XOR", " xor ")},
			{KEY("/", QDOS_KEY_DIV), TXT("SHL", " shl ")}},

	{{KEY("DN", QDOS_KEY_DOWN), KEY("RT", QDOS_KEY_RIGHT)},
			{KEY("4", QDOS_KEY_4), TXT("NOT", " not ")},
			{KEY("5", QDOS_KEY_5), TXT("==", " == ")},
			{KEY("6", QDOS_KEY_6), TXT("!=", " != ")},
			{KEY("*", QDOS_KEY_MUL), TXT("SHR", " shr ")}},

	{{KEY("SHF", QDOS_KEY_NONE), NONE},
			{KEY("1", QDOS_KEY_1), TXT("<=", " <= ")},
			{KEY("2", QDOS_KEY_2), TXT(">=", " >= ")},
			{KEY("3", QDOS_KEY_3), TXT("WTH", " within ")},
			{KEY("-", QDOS_KEY_SUB), TXT("INC", " ++ ")}},

	// exit, with off on its shifted layer as on the DM42
	{{KEY("ESC", QDOS_KEY_CLEAR), KEY("PWR", QDOS_KEY_POWER)},
			{KEY("0", QDOS_KEY_0), TXT("LEN", " len ")},
			{KEY(".", QDOS_KEY_DOT), TXT("NTH", " nth ")},
			{TXT(":", ":"), TXT(";", ";")},
			{KEY("+", QDOS_KEY_ADD), TXT("DEC", " -- ")}},
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

bool qdos_pad_is_shift(const qdos_pad_button* b) {
	return b != NULL && b->plain.label != NULL && strcmp(b->plain.label, "SHF") == 0;
}

const qdos_pad_action* qdos_pad_action_for(const qdos_pad_button* b, bool shifted) {
	if (b == NULL)
		return NULL;
	if (shifted && b->shifted.label != NULL)
		return &b->shifted;
	return shifted ? NULL : &b->plain;
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

void qdos_pad_draw(uint8_t* rgb, int stride_px, int y0, bool shifted) {
	fill(rgb, stride_px, 0, y0, stride_px, QDOS_PAD_H, PAD_BG);

	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = &KEYPAD[row][col];
			const int x = col * QDOS_PAD_BUTTON_W;
			const int y = y0 + row * QDOS_PAD_BUTTON_H;

			const bool live = !shifted || b->shifted.label != NULL || qdos_pad_is_shift(b);
			const uint8_t* face = (shifted && live && !qdos_pad_is_shift(b)) ? FACE_SHIFTED : FACE;

			fill(rgb, stride_px, x + 2, y + 2, QDOS_PAD_BUTTON_W - 4, QDOS_PAD_BUTTON_H - 4, EDGE);
			fill(rgb, stride_px, x + 3, y + 3, QDOS_PAD_BUTTON_W - 6, QDOS_PAD_BUTTON_H - 6, face);

			const char* text = (shifted && b->shifted.label != NULL) ? b->shifted.label : b->plain.label;
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
