/**
 * @file keypad_ui.c
 * @brief The simulator's on-screen keypad
 */

#include "keypad_ui.h"

#include "padfont.h"
#include "ui/font16x24.h" // for the QDOS_GLYPH_* keycap strings

#include <string.h>

/* One action, on one layer */
#define KEY(l, k) {(l), (k), NULL}
#define TXT(l, t) {(l), QDOS_KEY_NONE, (t)}
#define NONE {NULL, QDOS_KEY_NONE, NULL}

/* One button: the three layers, and whether it is a key or a modifier. The
 * arguments are brace lists, so they cannot be parenthesised. */
#define BTN(p, a, s) {p, a, s, QDOS_PAD_PLAIN}
#define MOD(cap, layer) {KEY(cap, QDOS_KEY_NONE), NONE, NONE, layer}

/*
 * The shift cap is blank. The colour says what it is, and everything it does is
 * printed in that same colour above the keys it does it to, so a word on the
 * key itself would be the one piece of writing on the pad that names a key
 * rather than an action.
 */
#define CAP_ALPHA "ALPHA"
#define CAP_SYMBOL ""

/*
 * Laid out the way a scientific calculator is: the entry row, then digits in a
 * 3x3 block with the operators down the right, Enter at the foot of them, all
 * at the bottom where a thumb expects them. The menu keys stay under the
 * display, and the arrows sit at the right of the function rows, as on a
 * graphing calculator, where they are found without looking.
 *
 * A cap in lower case is exactly the word it stands for, so it can be typed as
 * it is written; a cap in capitals is QDOS's own -- an abbreviation too long to
 * print in full, or a key the language has no name for.
 *
 * Three faces, because one modifier is not enough for a keypad this size.
 * Plain is a calculator. ALPHA is the letters, without which no name or string
 * can be typed at all, and it locks, because words are more than one letter
 * long. The shift layer is Quadrate's syntax, which cannot be reached any other
 * way; it lasts one press and hands back to whichever layer was showing. Words
 * that are neither stay off the keypad entirely -- CAT lists every one.
 */
static const qdos_pad_button KEYPAD[QDOS_PAD_ROWS][QDOS_PAD_COLS] = {
		/*
		 * Blank caps. These are the soft keys, and what they do is printed on the
		 * display directly above them, changing with the mode -- so inscribing them
		 * would be printing a name that is wrong most of the time.
		 */
		// Off lives in the far corner from ESC. It used to be ESC's own shift, and
		// ESC is the key you reach for to back out of a shift pressed by mistake.
		{BTN(KEY("", QDOS_KEY_SOFT1), NONE, NONE), BTN(KEY("", QDOS_KEY_SOFT2), NONE, NONE),
				BTN(KEY("", QDOS_KEY_SOFT3), NONE, NONE), BTN(KEY("", QDOS_KEY_SOFT4), NONE, NONE),
				BTN(KEY("", QDOS_KEY_SOFT5), NONE, KEY("PWR", QDOS_KEY_POWER))},

		/*
		 * Six across from here to the numeric block. The arrows are keys of the
		 * matrix like any other, set as an inverted T at the right the way an HP
		 * 48 has them: up over down, left and right either side. No shift on them,
		 * and ALPHA leaves them be, so a locked layer can still move.
		 */
		{BTN(KEY("1/x", QDOS_KEY_INV), TXT("A", "a"), TXT("(", "(")),
				BTN(KEY(QDOS_GLYPH_SQRT, QDOS_KEY_SQRT), TXT("B", "b"), TXT(")", ")")),
				BTN(KEY("x" QDOS_PAD_GLYPH_SQUARED, QDOS_KEY_SQ), TXT("C", "c"), TXT("\"", "\"")),
				BTN(KEY("ln", QDOS_KEY_LN), TXT("D", "d"), TXT("{", "{")),
				BTN(KEY(QDOS_GLYPH_UP, QDOS_KEY_UP), NONE, NONE),
				BTN(KEY("log", QDOS_KEY_LOG), TXT("E", "e"), TXT("}", "}"))},

		// The trigonometry, beside the rest of the arrows
		{BTN(KEY("sin", QDOS_KEY_SIN), TXT("F", "f"), TXT("fn", "fn ")),
				BTN(KEY("cos", QDOS_KEY_COS), TXT("G", "g"), TXT("--", " -- ")),
				BTN(KEY("tan", QDOS_KEY_TAN), TXT("H", "h"), TXT("i64", "i64")),
				BTN(KEY(QDOS_GLYPH_LEFT, QDOS_KEY_LEFT), NONE, NONE),
				BTN(KEY(QDOS_GLYPH_DOWN, QDOS_KEY_DOWN), NONE, NONE),
				BTN(KEY(QDOS_GLYPH_RIGHT, QDOS_KEY_RIGHT), NONE, NONE)},

		// The inverses under the trigonometry, e^x for ln, then powers and the
		// remainder, with control flow printed above
		{BTN(KEY("asin", QDOS_KEY_ASIN), TXT("I", "i"), TXT("f64", "f64")),
				BTN(KEY("acos", QDOS_KEY_ACOS), TXT("J", "j"), TXT("str", "str")),
				BTN(KEY("atan", QDOS_KEY_ATAN), TXT("K", "k"), TXT("if", "if ")),
				BTN(KEY("exp", QDOS_KEY_EXP), TXT("L", "l"), TXT("else", " else ")),
				BTN(KEY("pow", QDOS_KEY_POW), TXT("M", "m"), TXT("loop", "loop ")),
				BTN(KEY("%", QDOS_KEY_MOD), TXT("N", "n"), TXT("BRK", " break "))},

		/*
		 * The stack, which is Quadrate's own row. Above it, in yellow, is what else
		 * can be done to the stack: shuffling, naming and undoing.
		 *
		 * `->` sits where `roll` did, which is where the language went: the deep
		 * shuffling words were removed in favour of naming what you took. So were
		 * over and rot, to CAT, and abs and round have their keys.
		 */
		{BTN(KEY("dup", QDOS_KEY_DUP), TXT("O", "o"), TXT("nip", " nip ")),
				BTN(KEY("drop", QDOS_KEY_DROP), TXT("P", "p"), KEY("UNDO", QDOS_KEY_UNDO)),
				BTN(KEY("swap", QDOS_KEY_SWAP), TXT("Q", "q"), TXT("->", " -> ")),
				BTN(KEY("abs", QDOS_KEY_ABS), TXT("R", "r"), KEY("CPLX", QDOS_KEY_COMPLEX)),
				BTN(KEY("round", QDOS_KEY_ROUND), TXT("S", "s"), KEY("i", QDOS_KEY_I)),
				BTN(KEY("DEL", QDOS_KEY_BACKSPACE), NONE, TXT("nl", " nl "))},

		/*
		 * Five across again, for the numeric block. Entry and editing: ALPHA sits
		 * beside space, being the other thing you reach for mid-word. DEL is
		 * above the operator column, where the right hand already is. DEL rather
		 * than an arrow: the arrows move the cursor, and one glyph cannot mean both.
		 */
		{BTN(TXT(QDOS_PAD_GLYPH_SPACE, " "), NONE, TXT("print", " print ")), MOD(CAP_ALPHA, QDOS_PAD_ALPHA),
				BTN(KEY(QDOS_GLYPH_PLUSMINUS, QDOS_KEY_NEG), TXT("T", "t"), TXT("[", "[")),
				BTN(KEY("TAB", QDOS_KEY_TAB), TXT("U", "u"), TXT("]", "]")),
				BTN(KEY(QDOS_GLYPH_DIVIDE, QDOS_KEY_DIV), TXT("V", "v"), TXT("shl", " shl "))},

		/*
		 * The digits, and nothing but the digits, on every layer. A name has
		 * numbers in it -- i64, log10 -- so a locked ALPHA that took the number
		 * keys away meant leaving the layer to finish the word. The letters that
		 * used to sit here are down the operator column, which is idle while a
		 * name is being typed.
		 *
		 * The registers down the left. STO and RCL take the digit after them, and
		 * that digit is right beside them.
		 */
		{BTN(KEY("STO", QDOS_KEY_STO), NONE, NONE), BTN(KEY("7", QDOS_KEY_7), NONE, TXT("and", " and ")),
				BTN(KEY("8", QDOS_KEY_8), NONE, TXT("or", " or ")),
				BTN(KEY("9", QDOS_KEY_9), NONE, TXT("shr", " shr ")),
				BTN(KEY(QDOS_GLYPH_TIMES, QDOS_KEY_MUL), TXT("W", "w"), TXT("<", "<"))},

		// Underscore on shift-minus, where both keyboards this reads from put it
		{BTN(KEY("RCL", QDOS_KEY_RCL), NONE, NONE), BTN(KEY("4", QDOS_KEY_4), NONE, TXT("not", " not ")),
				BTN(KEY("5", QDOS_KEY_5), NONE, TXT("==", " == ")), BTN(KEY("6", QDOS_KEY_6), NONE, TXT("!=", " != ")),
				BTN(KEY("-", QDOS_KEY_SUB), TXT("X", "x"), TXT("_", "_"))},

		// The shift key, in the left column of the numeric block under the thumb,
		// between the registers and the way out
		{MOD(CAP_SYMBOL, QDOS_PAD_SYMBOL), BTN(KEY("1", QDOS_KEY_1), NONE, TXT(">", ">")),
				BTN(KEY("2", QDOS_KEY_2), NONE, TXT("<=", " <= ")), BTN(KEY("3", QDOS_KEY_3), NONE, TXT(">=", " >= ")),
				BTN(KEY("+", QDOS_KEY_ADD), TXT("Y", "y"), TXT("for", " for "))},

		/*
		 * Enter in the corner under the thumb, at the foot of the operator column,
		 * where every calculator puts it -- it is pressed once per value entered
		 * and used to be the furthest key on the pad from the digits.
		 *
		 * MODE on the soft row is the way into a line, so ':' is only shift-point:
		 * where a Swedish keyboard keeps it, and the one piece of Quadrate syntax
		 * common enough to need reaching without unlocking. Its old key is pi, e on shift.
		 */
		{BTN(KEY("ESC", QDOS_KEY_CLEAR), NONE, NONE), BTN(KEY("0", QDOS_KEY_0), NONE, TXT("len", " len ")),
				BTN(KEY(".", QDOS_KEY_DOT), NONE, TXT(":", ":")),
				BTN(KEY(QDOS_GLYPH_PI, QDOS_KEY_PI), TXT("Z", "z"), KEY("e", QDOS_KEY_E)),
				BTN(KEY("ENTER", QDOS_KEY_ENTER), NONE, TXT("=", "="))},
};

#undef KEY
#undef TXT
#undef NONE

/* ---------------------------------------------------------------------------
 * Appearance
 *
 * The panel above is the hardware and is drawn as the hardware draws it. Below
 * it is a case, and a case can be made to look like something. The colours are
 * a neutral dark with a trace of the panel's green in them, so the two read as
 * one object rather than a screenshot with a toolbar under it.
 *
 * Keys are lit from above: a gradient down the face, a highlight on the top
 * edge, a shade on the bottom, and a shadow cast below. One accent only -- the
 * yellow shift, which is yellow to match the legends it switches on -- because
 * a second accent would leave neither of them reading as one.
 * ------------------------------------------------------------------------- */

/* The shell the whole thing is moulded from, top to bottom */
static const uint8_t FRAME_TOP[3] = {0x33, 0x36, 0x33};
static const uint8_t FRAME_BOTTOM[3] = {0x1C, 0x1E, 0x1C};

/* The keywell floor, which sits deeper than the shell around it */
static const uint8_t CASE_TOP[3] = {0x26, 0x28, 0x26};
static const uint8_t CASE_BOTTOM[3] = {0x15, 0x16, 0x15};

/* The seam where the case meets the glass, and the chamfer under it */
static const uint8_t BEZEL_SEAM[3] = {0x08, 0x09, 0x08};
static const uint8_t BEZEL_LIGHT[3] = {0x3C, 0x3F, 0x3C};

/*
 * A key face, top to bottom. The range has to be wide enough to see: a couple
 * of levels across 26 rows is a flat surface with extra arithmetic.
 */
static const uint8_t FACE_TOP[3] = {0x5B, 0x5F, 0x5A};
static const uint8_t FACE_BOTTOM[3] = {0x2F, 0x32, 0x2F};

/* The digits carry the most traffic, so they are the lightest thing here */
static const uint8_t DIGIT_TOP[3] = {0x6A, 0x6E, 0x68};
static const uint8_t DIGIT_BOTTOM[3] = {0x3B, 0x3F, 0x3A};

/* The soft row is a menu strip under the display, not a key in the hand */
static const uint8_t SOFT_TOP[3] = {0x44, 0x48, 0x46};
static const uint8_t SOFT_BOTTOM[3] = {0x25, 0x28, 0x27};

/* Arithmetic, cooled off so the eye finds the column without being told */
static const uint8_t OP_TOP[3] = {0x4C, 0x54, 0x5D};
static const uint8_t OP_BOTTOM[3] = {0x2A, 0x2F, 0x35};

/*
 * The shift key, and the colour of everything it does.
 *
 * Exactly one key on the pad is coloured and it is this one: yellow, blank,
 * and the same yellow as the second function printed above every key it
 * reaches.
 * The colour is the cross-reference -- it says "the yellow writing is what
 * this key gets you" without a word of explanation.
 */
static const uint8_t SHIFT_TOP[3] = {0xF2, 0xC0, 0x30};
static const uint8_t SHIFT_BOTTOM[3] = {0xB0, 0x86, 0x10};
static const uint8_t SHIFT_INK[3] = {0xE8, 0xB4, 0x2E};
static const uint8_t SHIFT_INK_DIM[3] = {0x7A, 0x60, 0x20};

/*
 * Enter. Raised a little off the plain face and no more. A wider key would say
 * it better, and a fixed grid has no width to give. It used to be brass, which
 * cannot stay -- two warm accents and neither reads as one, and the yellow has
 * a job that a decoration would get in the way of.
 */
static const uint8_t ENTER_TOP[3] = {0x6C, 0x70, 0x6B};
static const uint8_t ENTER_BOTTOM[3] = {0x3C, 0x40, 0x3B};

/* A modifier while its layer is the one showing */
static const uint8_t LIVE_TOP[3] = {0x37, 0x6C, 0x60};
static const uint8_t LIVE_BOTTOM[3] = {0x1A, 0x38, 0x31};

/* The name on the case: the panel's own silver, so the two belong together */
static const uint8_t NAMEPLATE_INK[3] = {0xA6, 0xAC, 0xA4};

static const uint8_t KEYLINE[3] = {0x0D, 0x0E, 0x0D};
static const uint8_t TEXT[3] = {0xE8, 0xEB, 0xE4};
static const uint8_t TEXT_DIM[3] = {0x6B, 0x6F, 0x6A};

/* How far the top three rows of the pad are given over to the bezel */
#define BEZEL_H 3

/* The rim down each side and along the bottom, where the case turns away */
#define RIM_W 3

/*
 * Corner rounding, as an inset per row from the nearest end. Drawn by hand
 * rather than computed: at a radius this small a circle lands a pixel either
 * side of where it looks right, which is the same reason the font is a bitmap.
 */
static const int CORNER[] = {3, 1, 0};
#define CORNER_ROWS ((int)(sizeof(CORNER) / sizeof(*CORNER)))

bool qdos_pad_modifier(const qdos_pad_button* b, qdos_pad_layer* selects) {
	if (b == NULL || b->selects == QDOS_PAD_PLAIN) {
		return false;
	}

	*selects = b->selects;
	return true;
}

const qdos_pad_action* qdos_pad_action_for(const qdos_pad_button* b, qdos_pad_layer layer) {
	if (b == NULL) {
		return NULL;
	}

	if (layer == QDOS_PAD_SYMBOL) {
		return b->symbol.label != NULL ? &b->symbol : NULL;
	}

	// Alpha only replaces the buttons carrying a letter; the rest keep working,
	// or a locked layer would leave you with no Enter and no backspace
	if (layer == QDOS_PAD_ALPHA && b->alpha.label != NULL) {
		return &b->alpha;
	}

	return b->plain.label != NULL ? &b->plain : NULL;
}

/** @brief What this button shows on @p layer, and whether it does anything there */
static const qdos_pad_action* face_of(const qdos_pad_button* b, qdos_pad_layer layer) {
	qdos_pad_layer ignored;
	if (qdos_pad_modifier(b, &ignored)) {
		return &b->plain;
	}
	return qdos_pad_action_for(b, layer);
}

/**
 * @brief Where the pad may draw
 *
 * Everything clips to this, so no amount of shadow spread can reach the panel
 * above or run off the end of the buffer.
 */
typedef struct {
	uint8_t* rgb;
	int stride; ///< Pixels per row of the target
	int left;	///< First column the pad owns
	int right;	///< One past the last
	int top;	///< First row the pad owns
	int bottom; ///< One past the last
} canvas;

static bool inside(const canvas* cv, int x, int y) {
	return x >= cv->left && x < cv->right && y >= cv->top && y < cv->bottom;
}

static void px(const canvas* cv, int x, int y, const uint8_t* c) {
	if (!inside(cv, x, y)) {
		return;
	}

	uint8_t* p = &cv->rgb[((size_t)y * cv->stride + x) * 3];
	p[0] = c[0];
	p[1] = c[1];
	p[2] = c[2];
}

/** @brief Darken whatever is already there, which is what a shadow does */
static void px_shade(const canvas* cv, int x, int y, int percent) {
	if (!inside(cv, x, y)) {
		return;
	}

	uint8_t* p = &cv->rgb[((size_t)y * cv->stride + x) * 3];
	for (int i = 0; i < 3; i++) {
		p[i] = (uint8_t)(p[i] * percent / 100);
	}
}

/** @brief @p num /@p den of the way from @p a to @p b */
static void mix(const uint8_t a[3], const uint8_t b[3], int num, int den, uint8_t out[3]) {
	for (int i = 0; i < 3; i++) {
		out[i] = (uint8_t)(a[i] + (b[i] - a[i]) * num / den);
	}
}

static void fill(const canvas* cv, int x0, int y0, int w, int h, const uint8_t* c) {
	for (int y = y0; y < y0 + h; y++) {
		for (int x = x0; x < x0 + w; x++) {
			px(cv, x, y, c);
		}
	}
}

/** @brief A vertical gradient, which is how a surface says it is lit from above */
static void gradient(const canvas* cv, int x0, int y0, int w, int h, const uint8_t top[3], const uint8_t bottom[3]) {
	for (int y = 0; y < h; y++) {
		uint8_t c[3];
		mix(top, bottom, y, (h > 1) ? h - 1 : 1, c);
		fill(cv, x0, y0 + y, w, 1, c);
	}
}

/** @brief How far in this row of a rounded rect starts, for corner rounding */
static int inset_at(int row, int h) {
	const int from_top = row;
	const int from_bottom = h - 1 - row;
	const int nearest = (from_top < from_bottom) ? from_top : from_bottom;
	return (nearest < CORNER_ROWS) ? CORNER[nearest] : 0;
}

/** @brief A rounded rectangle, filled with a vertical gradient */
static void rounded_gradient(
		const canvas* cv, int x0, int y0, int w, int h, const uint8_t top[3], const uint8_t bottom[3]) {
	for (int y = 0; y < h; y++) {
		const int in = inset_at(y, h);
		uint8_t c[3];
		mix(top, bottom, y, (h > 1) ? h - 1 : 1, c);
		fill(cv, x0 + in, y0 + y, w - 2 * in, 1, c);
	}
}

static void rounded_outline(const canvas* cv, int x0, int y0, int w, int h, const uint8_t* c) {
	for (int y = 0; y < h; y++) {
		const int in = inset_at(y, h);
		const int above = inset_at(y - 1, h);

		// A row wider than the one above it caps the gap between them
		if (y == 0 || above > in) {
			fill(cv, x0 + in, y0 + y, w - 2 * in, 1, c);
		}

		const int below = inset_at(y + 1, h);
		if (y == h - 1 || below > in) {
			fill(cv, x0 + in, y0 + y, w - 2 * in, 1, c);
			continue;
		}

		px(cv, x0 + in, y0 + y, c);
		px(cv, x0 + w - 1 - in, y0 + y, c);
	}
}

static void rounded_shade(const canvas* cv, int x0, int y0, int w, int h, int percent) {
	for (int y = 0; y < h; y++) {
		const int in = inset_at(y, h);
		for (int x = x0 + in; x < x0 + w - in; x++) {
			px_shade(cv, x, y0 + y, percent);
		}
	}
}

/** @brief Lay @p c over what is there, @p alpha of 255 of the way */
static void px_blend(const canvas* cv, int x, int y, const uint8_t* c, int alpha) {
	if (alpha <= 0 || !inside(cv, x, y)) {
		return;
	}
	if (alpha >= 255) {
		px(cv, x, y, c);
		return;
	}

	uint8_t* p = &cv->rgb[((size_t)y * cv->stride + x) * 3];
	for (int i = 0; i < 3; i++) {
		p[i] = (uint8_t)((c[i] * alpha + p[i] * (255 - alpha)) / 255);
	}
}

/**
 * @brief Set @p text with its pen starting at @p x0 on baseline @p baseline
 *
 * The keycap font is antialiased, so each pixel arrives as a coverage value
 * and is blended rather than written. That is the whole reason a face this
 * small is readable: at 13px, one bit per pixel would lose the difference
 * between an 'a' and an 'o'.
 */
static void label(const canvas* cv, qdos_padface face, int x0, int baseline, const char* text, const uint8_t* c) {
	const uint8_t* coverage = qdos_padfont_coverage(face);
	int pen = x0;

	for (const char* p = text; *p != '\0'; p++) {
		const qdos_padglyph* g = qdos_padfont_glyph(face, (unsigned char)*p);
		if (g == NULL) {
			continue;
		}

		for (int gy = 0; gy < g->h; gy++) {
			for (int gx = 0; gx < g->w; gx++) {
				px_blend(cv, pen + g->bx + gx, baseline + g->by + gy, c, coverage[g->offset + (size_t)gy * g->w + gx]);
			}
		}
		pen += g->advance;
	}
}

/** @brief Which family a key belongs to, which is all its colour says */
typedef enum {
	GROUP_PLAIN,
	GROUP_SOFT,	 ///< The menu strip under the display
	GROUP_DIGIT, ///< Digits and the point
	GROUP_OP,	 ///< Arithmetic
	GROUP_ENTER,
	GROUP_SHIFT ///< The one coloured key on the pad
} key_group;

static key_group group_of(const qdos_pad_button* b, int row) {
	if (row == 0) {
		return GROUP_SOFT;
	}

	qdos_pad_layer selects;
	if (qdos_pad_modifier(b, &selects) && selects == QDOS_PAD_SYMBOL) {
		return GROUP_SHIFT;
	}

	switch (b->plain.key) {
	case QDOS_KEY_0:
	case QDOS_KEY_1:
	case QDOS_KEY_2:
	case QDOS_KEY_3:
	case QDOS_KEY_4:
	case QDOS_KEY_5:
	case QDOS_KEY_6:
	case QDOS_KEY_7:
	case QDOS_KEY_8:
	case QDOS_KEY_9:
	case QDOS_KEY_DOT:
		return GROUP_DIGIT;

	case QDOS_KEY_ADD:
	case QDOS_KEY_SUB:
	case QDOS_KEY_MUL:
	case QDOS_KEY_DIV:
		return GROUP_OP;

	case QDOS_KEY_ENTER:
		return GROUP_ENTER;

	default:
		return GROUP_PLAIN;
	}
}

/** @brief How much of the layer's colour a key carries */
typedef enum {
	TINT_NONE,
	TINT_SOFT, ///< This layer gives the key a different meaning
	TINT_FULL  ///< The modifier that switched the layer on
} key_tint;

static void face_colours(key_group group, key_tint tint, uint8_t top[3], uint8_t bottom[3]) {
	const uint8_t *base_top, *base_bottom;
	switch (group) {
	case GROUP_SOFT:
		base_top = SOFT_TOP;
		base_bottom = SOFT_BOTTOM;
		break;
	case GROUP_DIGIT:
		base_top = DIGIT_TOP;
		base_bottom = DIGIT_BOTTOM;
		break;
	case GROUP_OP:
		base_top = OP_TOP;
		base_bottom = OP_BOTTOM;
		break;
	case GROUP_ENTER:
		base_top = ENTER_TOP;
		base_bottom = ENTER_BOTTOM;
		break;
	case GROUP_SHIFT:
		base_top = SHIFT_TOP;
		base_bottom = SHIFT_BOTTOM;
		break;
	default:
		base_top = FACE_TOP;
		base_bottom = FACE_BOTTOM;
		break;
	}

	if (tint == TINT_NONE) {
		memcpy(top, base_top, 3);
		memcpy(bottom, base_bottom, 3);
		return;
	}

	// The shift key is already the colour of the layer it switches on, so
	// holding it down brightens rather than recolours: turning it teal would
	// break the one thing its colour is there to say
	if (group == GROUP_SHIFT) {
		static const uint8_t WHITE[3] = {0xFF, 0xFF, 0xFF};
		mix(base_top, WHITE, 1, 3, top);
		mix(base_bottom, WHITE, 1, 3, bottom);
		return;
	}

	// A quarter of the way over for a key the layer re-labels, all the way for
	// the modifier itself. Tinting everything the layer reaches turns the pad
	// one colour, which says nothing; what is worth pointing at is the
	// modifier that is down and the keys whose meaning just changed.
	const int den = (tint == TINT_FULL) ? 1 : 4;
	mix(base_top, LIVE_TOP, 1, den, top);
	mix(base_bottom, LIVE_BOTTOM, 1, den, bottom);
}

/** @brief The keywell the keys are set into */
static void draw_case(const canvas* cv) {
	gradient(cv, cv->left, cv->top + BEZEL_H, QDOS_PAD_W, QDOS_PAD_H - BEZEL_H, CASE_TOP, CASE_BOTTOM);

	// The seam against the glass, then a chamfer catching the light. Without
	// this the panel looks pasted on rather than set in.
	fill(cv, cv->left, cv->top, QDOS_PAD_W, 1, BEZEL_SEAM);
	fill(cv, cv->left, cv->top + 1, QDOS_PAD_W, 1, BEZEL_LIGHT);
	uint8_t c[3];
	mix(BEZEL_LIGHT, CASE_TOP, 1, 2, c);
	fill(cv, cv->left, cv->top + 2, QDOS_PAD_W, 1, c);

	// The keywell floor turning up to meet the case on three sides, so the
	// outer keys sit in something rather than stopping at an edge
	for (int i = 0; i < RIM_W; i++) {
		const int shade = 62 + i * 12;
		for (int y = cv->top + BEZEL_H; y < cv->bottom; y++) {
			px_shade(cv, cv->left + i, y, shade);
			px_shade(cv, cv->left + QDOS_PAD_W - 1 - i, y, shade);
		}
		for (int x = cv->left; x < cv->left + QDOS_PAD_W; x++) {
			px_shade(cv, x, cv->bottom - 1 - i, shade);
		}
	}
}

/**
 * @brief One key: shadow beneath, gradient face, bevel, keyline
 *
 * @param y Already moved down by QDOS_KEY_TRAVEL if this key is held
 *
 * Held, everything about the lighting turns over. A key standing proud casts a
 * shadow below it and catches the light on its top edge; pressed, it is down in
 * the well with the case shading its top, so the gradient runs the other way
 * and the bevel swaps ends. That reversal is what reads as a press -- moving it
 * two pixels on its own would just look like a misdraw.
 */
static void draw_key(const canvas* cv, int x, int y, int w, int h, key_group group, key_tint tint, bool pressed) {
	if (pressed) {
		// Down on its own shadow, so what is left is contact, not cast
		rounded_shade(cv, x, y + 1, w, h, 88);
	} else {
		// Two offset shades rather than one, so the edge falls off instead of
		// stopping. They multiply where they overlap, the darkest part.
		rounded_shade(cv, x - 1, y + 3, w + 2, h, 84);
		rounded_shade(cv, x, y + 2, w, h, 66);
	}

	uint8_t face_top[3], face_bottom[3];
	face_colours(group, tint, face_top, face_bottom);

	static const uint8_t WHITE[3] = {0xFF, 0xFF, 0xFF};
	uint8_t grad_top[3], grad_bottom[3], edge_top[3], edge_bottom[3];

	if (pressed) {
		mix(face_bottom, KEYLINE, 1, 5, grad_top);
		mix(face_top, KEYLINE, 2, 5, grad_bottom);
		mix(grad_top, KEYLINE, 1, 2, edge_top);		// the well's shadow across it
		mix(grad_bottom, WHITE, 1, 8, edge_bottom); // a faint catch at the foot
	} else {
		memcpy(grad_top, face_top, 3);
		memcpy(grad_bottom, face_bottom, 3);
		mix(face_top, WHITE, 1, 4, edge_top);
		mix(face_bottom, KEYLINE, 1, 3, edge_bottom);
	}

	rounded_outline(cv, x, y, w, h, KEYLINE);
	rounded_gradient(cv, x + 1, y + 1, w - 2, h - 2, grad_top, grad_bottom);

	const int inset = CORNER[1];
	fill(cv, x + 1 + inset, y + 1, w - 2 - 2 * inset, 1, edge_top);
	fill(cv, x + 1 + inset, y + h - 2, w - 2 - 2 * inset, 1, edge_bottom);
}

/* Keys per row: the soft row, four function rows of six, the numeric block */
static const int ROW_COLS[QDOS_PAD_ROWS] = {5, 6, 6, 6, 6, 5, 5, 5, 5, 5};

int qdos_pad_row_cols(int row) {
	return (row < 0 || row >= QDOS_PAD_ROWS) ? 0 : ROW_COLS[row];
}

// Rounded up, so that a pixel's column is simply x * cols / width
int qdos_pad_cell_left(int col, int row) {
	const int cols = ROW_COLS[row];
	return (col * QDOS_PAD_W + cols - 1) / cols;
}

void qdos_pad_key_rect(const qdos_pad_button* b, int* x, int* y, int* w, int* h) {
	const int i = (int)(b - &KEYPAD[0][0]);
	const int col = i % QDOS_PAD_COLS, row = i / QDOS_PAD_COLS;
	const int left = qdos_pad_cell_left(col, row);

	// One width for the whole row, though its cells differ by a pixel
	*w = QDOS_PAD_W / ROW_COLS[row] - 2 * QDOS_KEY_INSET_X;
	*h = QDOS_KEY_H;
	*x = left + (qdos_pad_cell_left(col + 1, row) - left - *w) / 2;
	*y = row * QDOS_PAD_BUTTON_H + QDOS_KEY_INSET_TOP;
}

void qdos_pad_draw(uint8_t* rgb, int stride_px, int x0, int y0, qdos_pad_layer layer, const qdos_pad_button* pressed) {
	const canvas cv = {
			.rgb = rgb,
			.stride = stride_px,
			.left = x0,
			.right = x0 + QDOS_PAD_W,
			.top = y0,
			.bottom = y0 + QDOS_PAD_H,
	};

	draw_case(&cv);

	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < ROW_COLS[row]; col++) {
			const qdos_pad_button* b = &KEYPAD[row][col];
			const bool down = (b == pressed);
			int x, y, kw, kh;
			qdos_pad_key_rect(b, &x, &y, &kw, &kh);
			x += x0;
			y += y0 + (down ? QDOS_KEY_TRAVEL : 0);

			qdos_pad_layer selects;
			const bool is_modifier = qdos_pad_modifier(b, &selects);
			const qdos_pad_action* shown = face_of(b, layer);
			const bool live = shown != NULL;

			// The modifier that switched the layer on is lit outright, so what
			// is switched on is never in doubt. Everything else is tinted only
			// where this layer actually gave it a different meaning.
			const bool changed = (layer == QDOS_PAD_ALPHA && b->alpha.label != NULL) ||
								 (layer == QDOS_PAD_SYMBOL && b->symbol.label != NULL);

			key_tint tint = TINT_NONE;
			if (is_modifier && selects == layer) {
				tint = TINT_FULL;
			} else if (changed) {
				tint = TINT_SOFT;
			}

			// The shift legend, printed on the case above the key rather than
			// on it. Always there, whichever layer is showing: the point of
			// printing it is to answer "what does shift do here" without
			// having to press shift and look.
			if (b->symbol.label != NULL) {
				const int sw = qdos_padfont_advance(QDOS_PADFACE_SHIFT, b->symbol.label);
				const int left = qdos_pad_cell_left(col, row);
				const int sx = x0 + left + (qdos_pad_cell_left(col + 1, row) - left - sw) / 2;
				label(&cv, QDOS_PADFACE_SHIFT, sx, y0 + row * QDOS_PAD_BUTTON_H + QDOS_SHIFT_LABEL_BASELINE,
						b->symbol.label, layer == QDOS_PAD_SYMBOL ? SHIFT_INK : SHIFT_INK_DIM);
			}

			draw_key(&cv, x, y, kw, kh, group_of(b, row), tint, down);

			const char* text = live ? shown->label : b->plain.label;
			if (text == NULL) {
				continue;
			}

			// Centred on the cap box rather than the font's full height, so a
			// cap with a descender in it sits where the eye expects instead of
			// riding high to make room below
			const int width = qdos_padfont_advance(QDOS_PADFACE_CAP, text);
			const int cap = qdos_padfont_cap_height(QDOS_PADFACE_CAP);
			label(&cv, QDOS_PADFACE_CAP, x + (kw - width) / 2, y + (kh + cap) / 2, text, live ? TEXT : TEXT_DIM);
		}
	}
}

/**
 * @brief The case, drawn around the panel and the keypad
 *
 * Lit from the top left, as the keys are: the top and left edges catch the
 * light, the bottom and right fall away, and a dark line runs immediately
 * around the glass so the panel reads as inset rather than painted on.
 */
void qdos_frame_draw(uint8_t* rgb, int stride_px) {
	const canvas cv = {
			.rgb = rgb,
			.stride = stride_px,
			.left = 0,
			.right = QDOS_WINDOW_W,
			.top = 0,
			.bottom = QDOS_WINDOW_H,
	};

	// The recess the panel and the keypad sit in, one pixel larger than both
	const int rx = QDOS_PANEL_X - 1, ry = QDOS_PANEL_Y - 1;
	const int rw = QDOS_SCREEN_W + 2, rh = QDOS_SCREEN_H + QDOS_PAD_H + 2;

	// Only the border is ours. The panel is written straight from the
	// framebuffer and the keypad draws itself, so painting across either would
	// erase whichever of them ran first.
	for (int y = 0; y < QDOS_WINDOW_H; y++) {
		uint8_t c[3];
		mix(FRAME_TOP, FRAME_BOTTOM, y, QDOS_WINDOW_H - 1, c);

		if (y < ry || y >= ry + rh) {
			fill(&cv, 0, y, QDOS_WINDOW_W, 1, c);
			continue;
		}
		fill(&cv, 0, y, rx, 1, c);
		fill(&cv, rx + rw, y, QDOS_WINDOW_W - rx - rw, 1, c);
	}

	// Lit from the top left, as the keys are: the top and left edges of the
	// shell catch the light and the other two fall away
	uint8_t c[3];
	static const uint8_t WHITE[3] = {0xFF, 0xFF, 0xFF};
	mix(FRAME_TOP, WHITE, 1, 5, c);
	fill(&cv, 0, 0, QDOS_WINDOW_W, 1, c);
	fill(&cv, 0, 0, 1, QDOS_WINDOW_H, c);

	mix(FRAME_BOTTOM, BEZEL_SEAM, 1, 2, c);
	fill(&cv, 0, QDOS_WINDOW_H - 1, QDOS_WINDOW_W, 1, c);
	fill(&cv, QDOS_WINDOW_W - 1, 0, 1, QDOS_WINDOW_H, c);

	// A dark line all the way round the recess, so the glass reads as set into
	// the case rather than printed on it. Every side of it is border, not panel.
	fill(&cv, rx, ry, rw, 1, BEZEL_SEAM);
	fill(&cv, rx, ry + rh - 1, rw, 1, BEZEL_SEAM);
	fill(&cv, rx, ry, 1, rh, BEZEL_SEAM);
	fill(&cv, rx + rw - 1, ry, 1, rh, BEZEL_SEAM);

	/*
	 * The name, above the glass and flush with its left edge, so the two line
	 * up rather than the name floating loose over the middle of the panel.
	 *
	 * Centred on the whole of the case above the glass rather than on its own
	 * band: there is no seam between the band and the border over it, so the
	 * eye reads all of it as one space and text centred in the band alone sits
	 * visibly low in it.
	 *
	 * Set in the keycap face but a shade duller -- it is printed on the case,
	 * not moulded into a key, and should not compete with one.
	 */
	const int cap = qdos_padfont_cap_height(QDOS_PADFACE_CAP);
	label(&cv, QDOS_PADFACE_CAP, QDOS_PANEL_X, (QDOS_PANEL_Y - 1 + cap) / 2, QDOS_NAMEPLATE, NAMEPLATE_INK);
}

const qdos_pad_button* qdos_pad_at(int x, int y) {
	const int px_ = x - QDOS_PAD_X;
	const int py = y - QDOS_PAD_Y;
	if (px_ < 0 || py < 0 || px_ >= QDOS_PAD_W || py >= QDOS_PAD_H) {
		return NULL;
	}

	const int row = py / QDOS_PAD_BUTTON_H;
	return &KEYPAD[row][px_ * ROW_COLS[row] / QDOS_PAD_W];
}

const qdos_pad_button* qdos_pad_button_at(int col, int row) {
	if (col < 0 || row < 0 || row >= QDOS_PAD_ROWS || col >= ROW_COLS[row]) {
		return NULL;
	}
	return &KEYPAD[row][col];
}
