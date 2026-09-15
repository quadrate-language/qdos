/**
 * @file test_padui.c
 * @brief The simulator's on-screen keypad
 */

#include "check.h"

#include "../src/hal/sim/keypad_ui.h"
#include "../src/ui/font16x24.h"

#include <stdlib.h>
#include <string.h>

static void test_every_slot_is_filled(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			CHECK(b != NULL);
			CHECK(b->plain.label != NULL && b->plain.label[0] != '\0');

			const qdos_pad_action* layers[2] = {&b->plain, &b->shifted};
			for (int l = 0; l < 2; l++) {
				const qdos_pad_action* a = layers[l];
				if (a->label == NULL) {
					CHECK(a->key == QDOS_KEY_NONE && a->text == NULL);
					continue;
				}
				// shf is the only button that is neither a key nor text
				const bool is_shift = (l == 0 && qdos_pad_is_shift(b));
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
			if (b->shifted.label != NULL)
				CHECK((int)strlen(b->shifted.label) * QDOS_FONT_W <= QDOS_PAD_BUTTON_W);
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

	qdos_pad_draw(buf, w, 0, false);
	qdos_pad_draw(buf, w, 0, true);

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
	CHECK(qdos_pad_is_shift(shift));

	// Exactly one shift button, or the state machine has two masters
	int shifts = 0;
	for (int row = 0; row < QDOS_PAD_ROWS; row++)
		for (int col = 0; col < QDOS_PAD_COLS; col++)
			if (qdos_pad_is_shift(qdos_pad_button_at(col, row)))
				shifts++;
	CHECK(shifts == 1);

	// A button with no shifted label sends nothing while shift is held
	const qdos_pad_button* bks = qdos_pad_button_at(4, 5);
	CHECK(bks->shifted.label == NULL);
	CHECK(qdos_pad_action_for(bks, false) == &bks->plain);
	CHECK(qdos_pad_action_for(bks, true) == NULL);

	const qdos_pad_button* seven = qdos_pad_button_at(1, 6);
	CHECK(qdos_pad_action_for(seven, false)->key == QDOS_KEY_7);
	CHECK(qdos_pad_action_for(seven, true)->text != NULL);

	CHECK(qdos_pad_action_for(NULL, false) == NULL);
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
	CHECK(qdos_pad_is_shift(qdos_pad_button_at(0, 8)));
	CHECK(qdos_pad_button_at(0, 9)->plain.key == QDOS_KEY_CLEAR);

	// Off is shift-exit, so it cannot be hit by accident
	CHECK(qdos_pad_button_at(0, 9)->shifted.key == QDOS_KEY_POWER);

	// The arrows the DM42 has no room for
	CHECK(qdos_pad_button_at(0, 6)->shifted.key == QDOS_KEY_LEFT);
	CHECK(qdos_pad_button_at(0, 7)->shifted.key == QDOS_KEY_RIGHT);
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

int main(void) {
	test_every_slot_is_filled();
	test_labels_fit_their_button();
	test_hit_testing();
	test_shift_layer();
	test_dm42_block();
	test_a_space_is_reachable();
	test_draw_stays_in_bounds();
	return check_report("padui");
}
