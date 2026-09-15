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
			CHECK(b->label != NULL && b->label[0] != '\0');
			CHECK((b->key != QDOS_KEY_NONE) != (b->text != NULL));
		}
	}
	CHECK(qdos_pad_button_at(QDOS_PAD_COLS, 0) == NULL);
	CHECK(qdos_pad_button_at(0, QDOS_PAD_ROWS) == NULL);
}

static void test_labels_fit_their_button(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			CHECK((int)strlen(b->label) * QDOS_FONT_W <= QDOS_PAD_BUTTON_W);
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

	qdos_pad_draw(buf, w, 0);

	for (int i = 0; i < 16; i++)
		CHECK(buf[bytes + i] == 0xAA);

	int painted = 0;
	for (size_t i = 0; i < bytes; i++)
		if (buf[i] != 0)
			painted++;
	CHECK(painted > 0);

	free(buf);
}

int main(void) {
	test_every_slot_is_filled();
	test_labels_fit_their_button();
	test_hit_testing();
	test_draw_stays_in_bounds();
	return check_report("padui");
}
