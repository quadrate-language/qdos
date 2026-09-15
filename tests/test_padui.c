/**
 * @file test_padui.c
 * @brief The simulator's on-screen keypad
 */

#include "check.h"

#include "../src/hal/sim/keypad_ui.h"
#include "../src/ui/font16x24.h"

#include <stdlib.h>

static bool is_modifier(const qdos_pad_button* b, qdos_pad_layer want) {
	qdos_pad_layer selects;
	return qdos_pad_modifier(b, &selects) && selects == want;
}

static bool is_any_modifier(const qdos_pad_button* b) {
	qdos_pad_layer selects;
	return qdos_pad_modifier(b, &selects);
}
#include <string.h>

static void test_every_slot_is_filled(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			CHECK(b != NULL);
			CHECK(b->plain.label != NULL && b->plain.label[0] != '\0');

			const qdos_pad_action* layers[2] = {&b->plain, &b->symbol};
			for (int l = 0; l < 2; l++) {
				const qdos_pad_action* a = layers[l];
				if (a->label == NULL) {
					CHECK(a->key == QDOS_KEY_NONE && a->text == NULL);
					continue;
				}
				// shf is the only button that is neither a key nor text
				const bool is_shift = (l == 0 && is_any_modifier(b));
				if (!is_shift)
					CHECK((a->key != QDOS_KEY_NONE) != (a->text != NULL));
			}
		}
	}
	CHECK(qdos_pad_button_at(QDOS_PAD_COLS, 0) == NULL);
	CHECK(qdos_pad_button_at(0, QDOS_PAD_ROWS) == NULL);
}

static void test_labels_fit_their_button(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			CHECK((int)strlen(b->plain.label) * QDOS_FONT_W <= QDOS_PAD_BUTTON_W);
			if (b->symbol.label != NULL)
				CHECK((int)strlen(b->symbol.label) * QDOS_FONT_W <= QDOS_PAD_BUTTON_W);
		}
	}
}

static void test_hit_testing(void) {
	CHECK(qdos_pad_at(10, 0) == NULL);
	CHECK(qdos_pad_at(10, QDOS_SCREEN_H - 1) == NULL);

	const qdos_pad_button* first = qdos_pad_at(1, QDOS_SCREEN_H + 1);
	CHECK(first == qdos_pad_button_at(0, 0));

	const qdos_pad_button* last =
			qdos_pad_at(QDOS_SCREEN_W - 1, QDOS_SCREEN_H + QDOS_PAD_H - 1);
	CHECK(last == qdos_pad_button_at(QDOS_PAD_COLS - 1, QDOS_PAD_ROWS - 1));

	for (int col = 0; col < QDOS_PAD_COLS; col++) {
		const int x = col * QDOS_PAD_BUTTON_W + QDOS_PAD_BUTTON_W / 2;
		CHECK(qdos_pad_at(x, QDOS_SCREEN_H + 5) == qdos_pad_button_at(col, 0));
	}
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		const int y = QDOS_SCREEN_H + row * QDOS_PAD_BUTTON_H + QDOS_PAD_BUTTON_H / 2;
		CHECK(qdos_pad_at(5, y) == qdos_pad_button_at(0, row));
	}

	CHECK(qdos_pad_at(-1, QDOS_SCREEN_H + 5) == NULL);
	CHECK(qdos_pad_at(QDOS_SCREEN_W, QDOS_SCREEN_H + 5) == NULL);
	CHECK(qdos_pad_at(5, QDOS_SCREEN_H + QDOS_PAD_H) == NULL);
}

static void test_draw_stays_in_bounds(void) {
	const int w = QDOS_SCREEN_W, h = QDOS_PAD_H;
	const size_t bytes = (size_t)w * h * 3;

	uint8_t* buf = malloc(bytes + 16);
	memset(buf + bytes, 0xAA, 16); // canary

	qdos_pad_draw(buf, w, 0, QDOS_PAD_PLAIN);
	qdos_pad_draw(buf, w, 0, QDOS_PAD_ALPHA);
	qdos_pad_draw(buf, w, 0, QDOS_PAD_SYMBOL);

	for (int i = 0; i < 16; i++)
		CHECK(buf[bytes + i] == 0xAA);

	int painted = 0;
	for (size_t i = 0; i < bytes; i++)
		if (buf[i] != 0)
			painted++;
	CHECK(painted > 0);

	free(buf);
}

static void test_shift_layer(void) {
	const qdos_pad_button* shift = qdos_pad_button_at(0, 8);
	CHECK(is_modifier(shift, QDOS_PAD_ALPHA));

	// One key per layer and no more, or the state machine has two masters
	int alpha = 0, symbol = 0;
	for (int row = 0; row < QDOS_PAD_ROWS; row++)
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (is_modifier(b, QDOS_PAD_ALPHA))
				alpha++;
			if (is_modifier(b, QDOS_PAD_SYMBOL))
				symbol++;
		}
	CHECK(alpha == 1);
	CHECK(symbol == 1);

	// A button with no shifted label sends nothing while shift is held
	const qdos_pad_button* f1 = qdos_pad_button_at(0, 0);
	CHECK(f1->symbol.label == NULL);
	CHECK(qdos_pad_action_for(f1, QDOS_PAD_PLAIN) == &f1->plain);
	CHECK(qdos_pad_action_for(f1, QDOS_PAD_SYMBOL) == NULL);

	const qdos_pad_button* seven = qdos_pad_button_at(1, 6);
	CHECK(qdos_pad_action_for(seven, QDOS_PAD_PLAIN)->key == QDOS_KEY_7);
	CHECK(qdos_pad_action_for(seven, QDOS_PAD_SYMBOL)->text != NULL);

	CHECK(qdos_pad_action_for(NULL, QDOS_PAD_PLAIN) == NULL);
}

/**
 * The DM42 block: navigation down the left, the digits in a 3x3, the operators
 * down the right, and enter at the head of the left column -- all of it at the
 * bottom of the pad, with nothing below the numpad.
 */
static void test_dm42_block(void) {
	static const qdos_key DIGITS[3][3] = {
		{QDOS_KEY_7, QDOS_KEY_8, QDOS_KEY_9},
		{QDOS_KEY_4, QDOS_KEY_5, QDOS_KEY_6},
		{QDOS_KEY_1, QDOS_KEY_2, QDOS_KEY_3},
	};
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			CHECK(qdos_pad_button_at(c + 1, r + 6)->plain.key == DIGITS[r][c]);

	CHECK(qdos_pad_button_at(1, 9)->plain.key == QDOS_KEY_0);
	CHECK(qdos_pad_button_at(2, 9)->plain.key == QDOS_KEY_DOT);

	static const qdos_key OPS[4] = {QDOS_KEY_DIV, QDOS_KEY_MUL, QDOS_KEY_SUB, QDOS_KEY_ADD};
	for (int r = 0; r < 4; r++)
		CHECK(qdos_pad_button_at(4, r + 6)->plain.key == OPS[r]);

	CHECK(qdos_pad_button_at(0, 5)->plain.key == QDOS_KEY_ENTER);
	CHECK(qdos_pad_button_at(0, 6)->plain.key == QDOS_KEY_UP);
	CHECK(qdos_pad_button_at(0, 7)->plain.key == QDOS_KEY_DOWN);
	CHECK(is_modifier(qdos_pad_button_at(0, 8), QDOS_PAD_ALPHA));
	CHECK(qdos_pad_button_at(0, 9)->plain.key == QDOS_KEY_CLEAR);

	// Off is shift-exit, so it cannot be hit by accident
	CHECK(qdos_pad_button_at(0, 9)->symbol.key == QDOS_KEY_POWER);

	// The arrows the DM42 has no room for
	CHECK(qdos_pad_button_at(0, 6)->symbol.key == QDOS_KEY_LEFT);
	CHECK(qdos_pad_button_at(0, 7)->symbol.key == QDOS_KEY_RIGHT);
}

/** Without a space key, two numbers typed in a row become one. */
static void test_a_space_is_reachable(void) {
	int spaces = 0;
	for (int row = 0; row < QDOS_PAD_ROWS; row++)
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (b->plain.text != NULL && strcmp(b->plain.text, " ") == 0)
				spaces++;
		}
	CHECK(spaces == 1);
}

/**
 * The default layer is a calculator: what you can see without reaching for
 * shift is arithmetic, not syntax.
 */
static void test_default_layer_is_the_calculator(void) {
	int functions = 0, syntax = 0;
	for (int row = 1; row < QDOS_PAD_ROWS; row++)
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (b->plain.key >= QDOS_KEY_FN_FIRST && b->plain.key <= QDOS_KEY_FN_LAST)
				functions++;
			if (b->symbol.text != NULL)
				syntax++;
		}
	CHECK(functions >= 12); // sin through mod
	CHECK(syntax >= 20);

	// The catalog needs no key of its own: it is on the soft row
	CHECK(is_modifier(qdos_pad_button_at(4, 3), QDOS_PAD_SYMBOL));
}

/**
 * The default layer is the calculator's, so its buttons must reach it. Typed
 * text only arrives in line mode, which would make such a button do nothing
 * where it is most wanted.
 */
static void test_default_layer_reaches_the_calculator(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++)
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (b->plain.text == NULL)
				continue;

			// A space means nothing to the calculator, and ':' is how you leave it
			const bool allowed = strcmp(b->plain.text, " ") == 0 || strcmp(b->plain.text, ":") == 0;
			CHECK(allowed);
		}
}

/** Every letter, exactly once, or something cannot be typed at all. */
static void test_alpha_layer_has_the_alphabet(void) {
	int seen[26] = {0};
	for (int row = 0; row < QDOS_PAD_ROWS; row++)
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_action* a = &qdos_pad_button_at(col, row)->alpha;
			if (a->label == NULL || a->text == NULL)
				continue;
			const char c = a->text[0];
			if (c >= 'a' && c <= 'z' && strlen(a->text) == 1)
				seen[c - 'a']++;
		}

	for (int i = 0; i < 26; i++)
		CHECK(seen[i] == 1);
}

/**
 * Alpha locks, so the keys you need while typing must survive it. Without this
 * a locked layer has no Enter, no backspace and no way to move.
 */
static void test_alpha_keeps_the_editing_keys(void) {
	static const qdos_key NEEDED[] = {
			QDOS_KEY_ENTER, QDOS_KEY_BACKSPACE, QDOS_KEY_CLEAR, QDOS_KEY_UP, QDOS_KEY_DOWN};

	for (size_t i = 0; i < sizeof(NEEDED) / sizeof(*NEEDED); i++) {
		bool found = false;
		for (int row = 0; row < QDOS_PAD_ROWS && !found; row++)
			for (int col = 0; col < QDOS_PAD_COLS && !found; col++) {
				const qdos_pad_button* b = qdos_pad_button_at(col, row);
				if (b->plain.key != NEEDED[i])
					continue;
				const qdos_pad_action* a = qdos_pad_action_for(b, QDOS_PAD_ALPHA);
				found = (a != NULL && a->key == NEEDED[i]);
			}
		CHECK(found);
	}
}

/** A modifier is not a key: pressing it must never type anything. */
static void test_modifiers_send_nothing(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++)
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (!is_any_modifier(b))
				continue;
			CHECK(b->plain.key == QDOS_KEY_NONE && b->plain.text == NULL);
		}
}

int main(void) {
	test_every_slot_is_filled();
	test_labels_fit_their_button();
	test_hit_testing();
	test_shift_layer();
	test_dm42_block();
	test_a_space_is_reachable();
	test_default_layer_is_the_calculator();
	test_default_layer_reaches_the_calculator();
	test_alpha_layer_has_the_alphabet();
	test_alpha_keeps_the_editing_keys();
	test_modifiers_send_nothing();
	test_draw_stays_in_bounds();
	return check_report("padui");
}
