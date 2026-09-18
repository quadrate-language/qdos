/**
 * @file test_padui.c
 * @brief The simulator's on-screen keypad
 */

#include "check.h"

#include "../src/hal/sim/keypad_ui.h"
#include "../src/hal/sim/padfont.h"

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

			// Every button does something on the plain layer, so it has a
			// label -- though a soft key's is empty, the display naming it
			// instead. NULL is the one thing it may not be.
			CHECK(b->plain.label != NULL);

			const qdos_pad_action* layers[2] = {&b->plain, &b->symbol};
			for (int l = 0; l < 2; l++) {
				const qdos_pad_action* a = layers[l];
				if (a->label == NULL) {
					CHECK(a->key == QDOS_KEY_NONE && a->text == NULL);
					continue;
				}
				// shf is the only button that is neither a key nor text
				const bool is_shift = (l == 0 && is_any_modifier(b));
				if (!is_shift) {
					CHECK((a->key != QDOS_KEY_NONE) != (a->text != NULL));
				}
			}
		}
	}
	CHECK(qdos_pad_button_at(QDOS_PAD_COLS, 0) == NULL);
	CHECK(qdos_pad_button_at(0, QDOS_PAD_ROWS) == NULL);
}

/**
 * Measured in the font that actually draws them, against the face of the key
 * rather than the whole cell. Counting characters says nothing once the font
 * is proportional: 'l' and 'W' are not the same width.
 */
static void test_labels_fit_their_button(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			const qdos_pad_action* layers[3] = {&b->plain, &b->alpha, &b->symbol};

			for (int l = 0; l < 3; l++) {
				if (layers[l]->label == NULL) {
					continue;
				}

				const int w = qdos_padfont_advance(QDOS_PADFACE_CAP, layers[l]->label);
				if (w > QDOS_KEY_LABEL_W) {
					fprintf(stderr, "  '%s' sets %dpx, key holds %d\n", layers[l]->label, w, QDOS_KEY_LABEL_W);
				}
				CHECK(w <= QDOS_KEY_LABEL_W);

				// A cap that measures nothing draws nothing, which is only
				// right for the soft keys
				CHECK(w > 0 || layers[l]->label[0] == '\0');
			}
		}
	}
}

/**
 * The shift legends fit their cell and clear the key below them.
 *
 * They are printed on the case, not on a key, so they have the whole cell to
 * set in -- but a descender that reaches the key's top edge looks like a
 * printing fault, and one wider than the cell runs into its neighbour.
 */
static void test_shift_legends_fit_above_their_key(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const char* legend = qdos_pad_button_at(col, row)->symbol.label;
			if (legend == NULL) {
				continue;
			}

			const int w = qdos_padfont_advance(QDOS_PADFACE_SHIFT, legend);
			if (w > QDOS_PAD_BUTTON_W - 4) {
				fprintf(stderr, "  legend '%s' sets %dpx, cell holds %d\n", legend, w, QDOS_PAD_BUTTON_W - 4);
			}
			CHECK(w > 0 && w <= QDOS_PAD_BUTTON_W - 4);

			// Every glyph, top and bottom, against the band it has to sit in
			for (const char* p = legend; *p; p++) {
				const qdos_padglyph* g = qdos_padfont_glyph(QDOS_PADFACE_SHIFT, (unsigned char)*p);
				CHECK(g != NULL);
				if (g == NULL || g->h == 0) {
					continue;
				}

				const int top = QDOS_SHIFT_LABEL_BASELINE + g->by;
				const int bottom = top + g->h - 1;
				if (bottom >= QDOS_KEY_INSET_TOP) {
					fprintf(stderr, "  legend '%s' reaches row %d, key starts at %d\n", legend, bottom,
							QDOS_KEY_INSET_TOP);
				}
				CHECK(top >= 0);
				CHECK(bottom < QDOS_KEY_INSET_TOP);
			}
		}
	}
}

/** A legend belongs to the key under it, so it has to sit nearer that one. */
static void test_a_legend_sits_nearer_its_own_key(void) {
	const int cap = qdos_padfont_cap_height(QDOS_PADFACE_SHIFT);

	// Gap from the legend down to its own key, and up to the key above it
	const int below = QDOS_KEY_INSET_TOP - QDOS_SHIFT_LABEL_BASELINE;
	const int above = (QDOS_SHIFT_LABEL_BASELINE - cap) + QDOS_KEY_INSET_BOTTOM;

	CHECK(below > 0);
	CHECK(below < above);
}

/** Every character a keycap uses has a glyph, or the cap comes out short. */
static void test_the_font_has_every_keycap_character(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			const qdos_pad_action* layers[3] = {&b->plain, &b->alpha, &b->symbol};

			for (int l = 0; l < 3; l++) {
				const char* label = layers[l]->label;
				if (label == NULL) {
					continue;
				}

				for (const char* p = label; *p; p++) {
					const qdos_padglyph* g = qdos_padfont_glyph(QDOS_PADFACE_CAP, (unsigned char)*p);
					if (g == NULL) {
						fprintf(stderr, "  no glyph for 0x%02x in '%s'\n", (unsigned char)*p, label);
					}
					CHECK(g != NULL);
					if (g == NULL) {
						continue;
					}

					// Space is the only one allowed to be blank
					if (*p != ' ') {
						CHECK(g->w > 0 && g->h > 0);
					}
					CHECK(g->advance > 0);
				}
			}
		}
	}
}

/**
 * The soft keys carry no inscription, and they are the only ones that do not.
 *
 * What they do is printed on the display directly above them and changes with
 * the mode, so a moulded name would be wrong most of the time. A blank cap
 * looks like an oversight, which is exactly why it is asserted here.
 */
static void test_the_soft_keys_have_blank_caps(void) {
	static const qdos_key SOFT[QDOS_PAD_COLS] = {
			QDOS_KEY_SOFT1, QDOS_KEY_SOFT2, QDOS_KEY_SOFT3, QDOS_KEY_SOFT4, QDOS_KEY_SOFT5};

	for (int col = 0; col < QDOS_PAD_COLS; col++) {
		const qdos_pad_button* b = qdos_pad_button_at(col, 0);

		// Blank, but not absent: it still sends its key
		CHECK(b->plain.label != NULL);
		CHECK(b->plain.label[0] == '\0');
		CHECK(b->plain.key == SOFT[col]);
		CHECK(qdos_pad_action_for(b, QDOS_PAD_PLAIN) == &b->plain);
		CHECK(qdos_padfont_advance(QDOS_PADFACE_CAP, b->plain.label) == 0);
	}

	// The shift key is blank too, for its own reason: its colour says what it
	// is, and every use of it is printed in that colour above another key.
	// Between them that is the whole of the blank caps -- anything else blank
	// is a cap that has gone missing.
	int blank = 0, blank_soft = 0, blank_shift = 0;
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (b->plain.label[0] != '\0') {
				continue;
			}

			blank++;
			if (row == 0) {
				blank_soft++;
			} else if (is_modifier(b, QDOS_PAD_SYMBOL)) {
				blank_shift++;
			}
		}
	}
	CHECK(blank_soft == QDOS_PAD_COLS);
	CHECK(blank_shift == 1);
	CHECK(blank == QDOS_PAD_COLS + 1);
}

/**
 * A modifier is known by what it does, not by what it says.
 *
 * It used to be told from a key by comparing its cap, which held only while no
 * two caps matched -- and blanking the shift key put it level with five soft
 * keys that are blank for an entirely different reason.
 */
static void test_a_modifier_is_not_known_by_its_cap(void) {
	const qdos_pad_button* shift = qdos_pad_button_at(0, 8);
	const qdos_pad_button* soft = qdos_pad_button_at(0, 0);

	// Same cap, opposite answers
	CHECK_STR(shift->plain.label, soft->plain.label);
	CHECK(is_modifier(shift, QDOS_PAD_SYMBOL));
	CHECK(!is_any_modifier(soft));

	// And a soft key still sends its key rather than switching a layer
	CHECK(soft->plain.key == QDOS_KEY_SOFT1);
	CHECK(shift->plain.key == QDOS_KEY_NONE);
}

/** A blank cap really does leave the key face clear of ink. */
static void test_a_blank_cap_draws_nothing(void) {
	const int w = QDOS_WINDOW_W, h = QDOS_WINDOW_H;
	const size_t bytes = (size_t)w * h * 3;
	uint8_t* buf = calloc(bytes, 1);

	qdos_pad_draw(buf, w, QDOS_PAD_X, QDOS_PAD_Y, QDOS_PAD_PLAIN, NULL);

	// The middle of a soft key against the middle of the key below it. A cap
	// is the brightest thing on a face, so its absence shows in the maximum.
	int soft_max = 0, labelled_max = 0;
	for (int y = 4; y < QDOS_KEY_H - 4; y++) {
		for (int x = 8; x < QDOS_KEY_W - 8; x++) {
			const int sx = QDOS_PAD_X + QDOS_KEY_INSET_X + x;
			const size_t soft = ((size_t)(QDOS_PAD_Y + QDOS_KEY_INSET_TOP + y) * w + sx) * 3;
			const size_t below = ((size_t)(QDOS_PAD_Y + QDOS_PAD_BUTTON_H + QDOS_KEY_INSET_TOP + y) * w + sx) * 3;

			if (buf[soft] > soft_max) {
				soft_max = buf[soft];
			}
			if (buf[below] > labelled_max) {
				labelled_max = buf[below];
			}
		}
	}

	// The labelled key reaches the cap's brightness; the blank one stays face
	CHECK(labelled_max > 200);
	CHECK(soft_max < 120);

	free(buf);
}

/**
 * Window coordinates, so the case border is part of the sum. A click landing
 * one key out is a worse fault than a keypad that looks plain.
 */
static void test_hit_testing(void) {
	// The panel and the case around it are not the keypad
	CHECK(qdos_pad_at(10, 0) == NULL);
	CHECK(qdos_pad_at(10, QDOS_PAD_Y - 1) == NULL);
	CHECK(qdos_pad_at(QDOS_PAD_X - 1, QDOS_PAD_Y + 5) == NULL);
	CHECK(qdos_pad_at(QDOS_PAD_X + QDOS_PAD_W, QDOS_PAD_Y + 5) == NULL);
	CHECK(qdos_pad_at(5, QDOS_PAD_Y + QDOS_PAD_H) == NULL);
	CHECK(qdos_pad_at(-1, QDOS_PAD_Y + 5) == NULL);

	// The very first and very last pixel of the pad belong to the corner keys
	CHECK(qdos_pad_at(QDOS_PAD_X, QDOS_PAD_Y) == qdos_pad_button_at(0, 0));
	CHECK(qdos_pad_at(QDOS_PAD_X + QDOS_PAD_W - 1, QDOS_PAD_Y + QDOS_PAD_H - 1) ==
			qdos_pad_button_at(QDOS_PAD_COLS - 1, QDOS_PAD_ROWS - 1));

	// Every cell, hit at its centre
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const int x = QDOS_PAD_X + col * QDOS_PAD_BUTTON_W + QDOS_PAD_BUTTON_W / 2;
			const int y = QDOS_PAD_Y + row * QDOS_PAD_BUTTON_H + QDOS_PAD_BUTTON_H / 2;
			CHECK(qdos_pad_at(x, y) == qdos_pad_button_at(col, row));
		}
	}

	// And at both edges of a cell, since a boundary off by one is invisible
	for (int col = 0; col < QDOS_PAD_COLS; col++) {
		const int left = QDOS_PAD_X + col * QDOS_PAD_BUTTON_W;
		CHECK(qdos_pad_at(left, QDOS_PAD_Y + 5) == qdos_pad_button_at(col, 0));
		CHECK(qdos_pad_at(left + QDOS_PAD_BUTTON_W - 1, QDOS_PAD_Y + 5) == qdos_pad_button_at(col, 0));
	}
}

/** The case surrounds the panel on every side, and the panel is still 1:1. */
static void test_window_geometry(void) {
	CHECK(QDOS_WINDOW_W == QDOS_SCREEN_W + 2 * QDOS_FRAME);

	// Border, nameplate, glass, keypad, border -- and nothing unaccounted for
	CHECK(QDOS_WINDOW_H == QDOS_FRAME + QDOS_NAMEPLATE_H + QDOS_SCREEN_H + QDOS_PAD_H + QDOS_FRAME);

	// The name sits above the glass, never on it
	CHECK(QDOS_NAMEPLATE_H > 0);
	CHECK(QDOS_PANEL_Y == QDOS_FRAME + QDOS_NAMEPLATE_H);

	// The keypad sits directly under the panel, both inset by the same border
	CHECK(QDOS_PAD_X == QDOS_PANEL_X);
	CHECK(QDOS_PAD_Y == QDOS_PANEL_Y + QDOS_SCREEN_H);

	// Nothing overhangs the window
	CHECK(QDOS_PAD_X + QDOS_PAD_W + QDOS_FRAME == QDOS_WINDOW_W);
	CHECK(QDOS_PAD_Y + QDOS_PAD_H + QDOS_FRAME == QDOS_WINDOW_H);
}

/**
 * Shadows spread and gradients round off, so the drawing has to be clipped
 * rather than merely careful. Drawn into the middle of a window-sized buffer,
 * with the margin left blank: anything that leaks shows up as a stray pixel.
 */
static void test_draw_stays_in_bounds(void) {
	const int w = QDOS_WINDOW_W, h = QDOS_WINDOW_H;
	const size_t bytes = (size_t)w * h * 3;

	uint8_t* buf = calloc(bytes + 16, 1);
	memset(buf + bytes, 0xAA, 16); // canary

	const qdos_pad_layer layers[3] = {QDOS_PAD_PLAIN, QDOS_PAD_ALPHA, QDOS_PAD_SYMBOL};
	for (int l = 0; l < 3; l++) {
		memset(buf, 0, bytes);
		qdos_pad_draw(buf, w, QDOS_PAD_X, QDOS_PAD_Y, layers[l], NULL);

		int painted = 0, leaked = 0;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const uint8_t* p = &buf[((size_t)y * w + x) * 3];
				const bool ink = p[0] || p[1] || p[2];
				if (!ink) {
					continue;
				}

				const bool in_pad = x >= QDOS_PAD_X && x < QDOS_PAD_X + QDOS_PAD_W && y >= QDOS_PAD_Y &&
									y < QDOS_PAD_Y + QDOS_PAD_H;
				if (in_pad) {
					painted++;
				} else {
					leaked++;
				}
			}
		}
		CHECK(painted > 0);
		CHECK(leaked == 0);
	}

	for (int i = 0; i < 16; i++) {
		CHECK(buf[bytes + i] == 0xAA);
	}

	free(buf);
}

/**
 * A held key looks different, and only that key does.
 *
 * It is the simulator's only acknowledgement of a click -- there is no key
 * under the pointer to move on its own -- so it has to be visible, has to be
 * confined to the one key, and has to stay inside the pad while it sinks.
 */
static void test_a_pressed_key_is_drawn_sunk(void) {
	const int w = QDOS_WINDOW_W, h = QDOS_WINDOW_H;
	const size_t bytes = (size_t)w * h * 3;

	uint8_t* loose = calloc(bytes, 1);
	uint8_t* held = calloc(bytes + 16, 1);
	memset(held + bytes, 0xAA, 16);

	// The last row, where a key that sinks has the least room left under it
	const qdos_pad_button* key = qdos_pad_button_at(2, QDOS_PAD_ROWS - 1);
	CHECK(key != NULL);

	qdos_pad_draw(loose, w, QDOS_PAD_X, QDOS_PAD_Y, QDOS_PAD_PLAIN, NULL);
	qdos_pad_draw(held, w, QDOS_PAD_X, QDOS_PAD_Y, QDOS_PAD_PLAIN, key);

	for (int i = 0; i < 16; i++) {
		CHECK(held[bytes + i] == 0xAA);
	}

	const int cx = QDOS_PAD_X + 2 * QDOS_PAD_BUTTON_W;
	const int cy = QDOS_PAD_Y + (QDOS_PAD_ROWS - 1) * QDOS_PAD_BUTTON_H;

	int changed_inside = 0, changed_outside = 0, leaked = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const size_t i = ((size_t)y * w + x) * 3;
			const bool differs = memcmp(&loose[i], &held[i], 3) != 0;

			const bool in_pad =
					x >= QDOS_PAD_X && x < QDOS_PAD_X + QDOS_PAD_W && y >= QDOS_PAD_Y && y < QDOS_PAD_Y + QDOS_PAD_H;
			if (!in_pad && (held[i] || held[i + 1] || held[i + 2])) {
				leaked++;
			}
			if (!differs) {
				continue;
			}

			const bool in_cell = x >= cx && x < cx + QDOS_PAD_BUTTON_W && y >= cy && y < cy + QDOS_PAD_BUTTON_H;
			if (in_cell) {
				changed_inside++;
			} else {
				changed_outside++;
			}
		}
	}

	CHECK(leaked == 0);
	CHECK(changed_outside == 0); // pressing one key does not redraw its neighbours
	CHECK(changed_inside > 100); // and it is a change you could actually see

	free(loose);
	free(held);
}

/** Every key can be held, including the ones in the corners. */
static void test_every_key_can_be_pressed(void) {
	const int w = QDOS_WINDOW_W, h = QDOS_WINDOW_H;
	const size_t bytes = (size_t)w * h * 3;
	uint8_t* buf = calloc(bytes + 16, 1);
	memset(buf + bytes, 0xAA, 16);

	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			memset(buf, 0, bytes);
			qdos_pad_draw(buf, w, QDOS_PAD_X, QDOS_PAD_Y, QDOS_PAD_PLAIN, qdos_pad_button_at(col, row));

			int leaked = 0;
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					const size_t i = ((size_t)y * w + x) * 3;
					const bool in_pad = x >= QDOS_PAD_X && x < QDOS_PAD_X + QDOS_PAD_W && y >= QDOS_PAD_Y &&
										y < QDOS_PAD_Y + QDOS_PAD_H;
					if (!in_pad && (buf[i] || buf[i + 1] || buf[i + 2])) {
						leaked++;
					}
				}
			}
			CHECK(leaked == 0);
		}
	}

	for (int i = 0; i < 16; i++) {
		CHECK(buf[bytes + i] == 0xAA);
	}

	free(buf);
}

/** The sink has to fit in the gutter, or the bottom row runs off the pad. */
static void test_key_travel_fits_the_gutter(void) {
	CHECK(QDOS_KEY_TRAVEL > 0);
	CHECK(QDOS_KEY_TRAVEL < QDOS_KEY_INSET_BOTTOM);
}

/**
 * The name is printed on the case above the glass, and stays there.
 *
 * It is the one piece of writing on the machine that is neither a keycap nor
 * something the display said, so nothing else would notice if it drifted onto
 * the panel or off the end of the window.
 */
static void test_the_nameplate_is_printed_on_the_case(void) {
	const int w = QDOS_WINDOW_W, h = QDOS_WINDOW_H;
	uint8_t* buf = calloc((size_t)w * h * 3, 1);

	// Set from the panel's left edge, so it has that much of the width to run
	// into before it reaches the border on the far side
	const int width = qdos_padfont_advance(QDOS_PADFACE_CAP, QDOS_NAMEPLATE);
	CHECK(width > 0);
	CHECK(QDOS_PANEL_X + width <= QDOS_WINDOW_W - QDOS_FRAME);

	// And its cap has to fit the case above the glass, or it collides with it
	const int cap = qdos_padfont_cap_height(QDOS_PADFACE_CAP);
	CHECK(cap < QDOS_PANEL_Y);

	qdos_frame_draw(buf, w);

	// Where the ink actually landed, and how far it is from each end of the
	// case above the glass. Centred means those two are within a pixel.
	int top = -1, bottom = -1, left = -1, right = -1;
	for (int y = 0; y < QDOS_PANEL_Y; y++) {
		for (int x = 0; x < w; x++) {
			if (buf[((size_t)y * w + x) * 3] <= 0x90) {
				continue;
			}

			if (top < 0) {
				top = y;
			}
			bottom = y;
			if (left < 0 || x < left) {
				left = x;
			}
			if (x > right) {
				right = x;
			}
		}
	}

	CHECK(top >= 0); // the name is there at all

	// Flush with the glass below it, give or take the first glyph's bearing
	CHECK(left >= QDOS_PANEL_X);
	CHECK(left <= QDOS_PANEL_X + 2);
	CHECK(right < QDOS_WINDOW_W - QDOS_FRAME);

	const int above = top;
	const int below = QDOS_PANEL_Y - 1 - bottom;
	if (above > below + 2 || below > above + 2) {
		fprintf(stderr, "  nameplate sits %d from the top, %d from the glass\n", above, below);
	}
	CHECK(above <= below + 2);
	CHECK(below <= above + 2);

	free(buf);
}

/**
 * The case paints the border and nothing else.
 *
 * The panel is written straight out of the framebuffer and the keypad draws
 * itself, so a case that painted across either would erase whichever ran
 * first -- which it did, and the display came out blank.
 */
static void test_frame_paints_only_the_border(void) {
	const int w = QDOS_WINDOW_W, h = QDOS_WINDOW_H;
	const size_t bytes = (size_t)w * h * 3;

	uint8_t* buf = calloc(bytes + 16, 1);
	memset(buf + bytes, 0xAA, 16);

	qdos_frame_draw(buf, w);

	for (int i = 0; i < 16; i++) {
		CHECK(buf[bytes + i] == 0xAA);
	}

	int border_blank = 0, panel_painted = 0, pad_painted = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t* p = &buf[((size_t)y * w + x) * 3];
			const bool ink = p[0] || p[1] || p[2];

			const bool in_panel = x >= QDOS_PANEL_X && x < QDOS_PANEL_X + QDOS_SCREEN_W && y >= QDOS_PANEL_Y &&
								  y < QDOS_PANEL_Y + QDOS_SCREEN_H;
			const bool in_pad =
					x >= QDOS_PAD_X && x < QDOS_PAD_X + QDOS_PAD_W && y >= QDOS_PAD_Y && y < QDOS_PAD_Y + QDOS_PAD_H;

			if (in_panel && ink) {
				panel_painted++;
			} else if (in_pad && ink) {
				pad_painted++;
			} else if (!in_panel && !in_pad && !ink) {
				border_blank++;
			}
		}
	}

	CHECK(panel_painted == 0); // the display is the display's business
	CHECK(pad_painted == 0);
	CHECK(border_blank == 0); // and the border is entirely the case's

	free(buf);
}

static void test_shift_layer(void) {
	// Left column of the numeric block, between the down arrow and the way
	// out, under the thumb
	CHECK(is_modifier(qdos_pad_button_at(0, 8), QDOS_PAD_SYMBOL));

	// Alpha sits beside delete, the other thing reached for mid-word
	CHECK(is_modifier(qdos_pad_button_at(1, 5), QDOS_PAD_ALPHA));

	// One key per layer and no more, or the state machine has two masters
	int alpha = 0, symbol = 0;
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (is_modifier(b, QDOS_PAD_ALPHA)) {
				alpha++;
			}
			if (is_modifier(b, QDOS_PAD_SYMBOL)) {
				symbol++;
			}
		}
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
 * The numeric block: navigation down the left, the digits in a 3x3, the
 * operators down the right, and enter at the head of the left column -- all of
 * it at the bottom of the pad, with nothing below the numpad.
 */
static void test_numeric_block(void) {
	static const qdos_key DIGITS[3][3] = {
			{QDOS_KEY_7, QDOS_KEY_8, QDOS_KEY_9},
			{QDOS_KEY_4, QDOS_KEY_5, QDOS_KEY_6},
			{QDOS_KEY_1, QDOS_KEY_2, QDOS_KEY_3},
	};
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			CHECK(qdos_pad_button_at(c + 1, r + 6)->plain.key == DIGITS[r][c]);
		}
	}

	CHECK(qdos_pad_button_at(1, 9)->plain.key == QDOS_KEY_0);
	CHECK(qdos_pad_button_at(2, 9)->plain.key == QDOS_KEY_DOT);

	// Enter at the foot of the operator column, where the thumb already is
	static const qdos_key OPS[4] = {QDOS_KEY_DIV, QDOS_KEY_MUL, QDOS_KEY_SUB, QDOS_KEY_ADD};
	for (int r = 0; r < 4; r++) {
		CHECK(qdos_pad_button_at(4, r + 5)->plain.key == OPS[r]);
	}

	CHECK(qdos_pad_button_at(4, 9)->plain.key == QDOS_KEY_ENTER);

	CHECK(qdos_pad_button_at(0, 5)->plain.key == QDOS_KEY_BACKSPACE);
	CHECK(qdos_pad_button_at(0, 6)->plain.key == QDOS_KEY_UP);
	CHECK(qdos_pad_button_at(0, 7)->plain.key == QDOS_KEY_DOWN);
	CHECK(is_modifier(qdos_pad_button_at(0, 8), QDOS_PAD_SYMBOL));
	CHECK(qdos_pad_button_at(0, 9)->plain.key == QDOS_KEY_CLEAR);

	// Off is the far corner from ESC, which is what backs out of a stray shift
	CHECK(qdos_pad_button_at(4, 0)->symbol.key == QDOS_KEY_POWER);
	CHECK(qdos_pad_button_at(0, 9)->symbol.label == NULL);

	// The arrows there is no room for on the face
	CHECK(qdos_pad_button_at(0, 6)->symbol.key == QDOS_KEY_LEFT);
	CHECK(qdos_pad_button_at(0, 7)->symbol.key == QDOS_KEY_RIGHT);
}

/** Every digit types a digit on every layer: names have numbers in them. */
static void test_digits_survive_every_layer(void) {
	static const qdos_key DIGITS[] = {QDOS_KEY_0, QDOS_KEY_1, QDOS_KEY_2, QDOS_KEY_3, QDOS_KEY_4, QDOS_KEY_5,
			QDOS_KEY_6, QDOS_KEY_7, QDOS_KEY_8, QDOS_KEY_9, QDOS_KEY_DOT};

	int found = 0;
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);

			bool is_digit = false;
			for (size_t i = 0; i < sizeof(DIGITS) / sizeof(*DIGITS); i++) {
				is_digit = is_digit || b->plain.key == DIGITS[i];
			}
			if (!is_digit) {
				continue;
			}

			found++;
			const qdos_pad_action* alpha = qdos_pad_action_for(b, QDOS_PAD_ALPHA);
			CHECK(alpha == &b->plain);
		}
	}
	CHECK(found == (int)(sizeof(DIGITS) / sizeof(*DIGITS)));
}

/** All twenty-six of them, once each, or a name cannot be typed. */
static void test_the_alphabet_is_complete(void) {
	int seen[26] = {0};

	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_action* a = &qdos_pad_button_at(col, row)->alpha;
			if (a->label == NULL || a->text == NULL) {
				continue;
			}

			// One lower-case letter, typed as it is printed
			CHECK(strlen(a->text) == 1);
			const char ch = a->text[0];
			CHECK(ch >= 'a' && ch <= 'z');
			seen[ch - 'a']++;
		}
	}

	for (int i = 0; i < 26; i++) {
		CHECK(seen[i] == 1);
	}
}

/** The letters displaced ':' and '_', which live inside names, onto shift. */
static void test_what_the_letters_displaced_is_still_reachable(void) {
	const char* wanted[] = {":", "_"};

	for (size_t w = 0; w < sizeof(wanted) / sizeof(*wanted); w++) {
		int found = 0;
		for (int row = 0; row < QDOS_PAD_ROWS; row++) {
			for (int col = 0; col < QDOS_PAD_COLS; col++) {
				const qdos_pad_action* s = &qdos_pad_button_at(col, row)->symbol;
				if (s->text != NULL && strcmp(s->text, wanted[w]) == 0) {
					found++;
				}
			}
		}
		CHECK(found == 1);
	}
}

/** Without a space key, two numbers typed in a row become one. */
static void test_a_space_is_reachable(void) {
	int spaces = 0;
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (b->plain.text != NULL && strcmp(b->plain.text, " ") == 0) {
				spaces++;
			}
		}
	}
	CHECK(spaces == 1);
}

/**
 * The default layer is a calculator: what you can see without reaching for
 * shift is arithmetic, not syntax.
 */
static void test_default_layer_is_the_calculator(void) {
	int functions = 0, syntax = 0;
	for (int row = 1; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (b->plain.key >= QDOS_KEY_FN_FIRST && b->plain.key <= QDOS_KEY_FN_LAST) {
				functions++;
			}
			if (b->symbol.text != NULL) {
				syntax++;
			}
		}
	}
	CHECK(functions >= 12); // sin through mod
	CHECK(syntax >= 20);

	// The catalog needs no key of its own: it is on the soft row
	CHECK(is_modifier(qdos_pad_button_at(0, 8), QDOS_PAD_SYMBOL));
}

/**
 * The default layer is the calculator's, so its buttons must reach it. Typed
 * text only arrives in line mode, which would make such a button do nothing
 * where it is most wanted.
 */
static void test_default_layer_reaches_the_calculator(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (b->plain.text == NULL) {
				continue;
			}

			// A space means nothing to the calculator, and ':' is how you leave it
			const bool allowed = strcmp(b->plain.text, " ") == 0 || strcmp(b->plain.text, ":") == 0;
			CHECK(allowed);
		}
	}
}

/** Every letter, exactly once, or something cannot be typed at all. */
static void test_alpha_layer_has_the_alphabet(void) {
	int seen[26] = {0};
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_action* a = &qdos_pad_button_at(col, row)->alpha;
			if (a->label == NULL || a->text == NULL) {
				continue;
			}
			const char c = a->text[0];
			if (c >= 'a' && c <= 'z' && strlen(a->text) == 1) {
				seen[c - 'a']++;
			}
		}
	}

	for (int i = 0; i < 26; i++) {
		CHECK(seen[i] == 1);
	}
}

/**
 * Alpha locks, so the keys you need while typing must survive it. Without this
 * a locked layer has no Enter, no backspace and no way to move.
 */
static void test_alpha_keeps_the_editing_keys(void) {
	static const qdos_key NEEDED[] = {QDOS_KEY_ENTER, QDOS_KEY_BACKSPACE, QDOS_KEY_CLEAR, QDOS_KEY_UP, QDOS_KEY_DOWN};

	for (size_t i = 0; i < sizeof(NEEDED) / sizeof(*NEEDED); i++) {
		bool found = false;
		for (int row = 0; row < QDOS_PAD_ROWS && !found; row++) {
			for (int col = 0; col < QDOS_PAD_COLS && !found; col++) {
				const qdos_pad_button* b = qdos_pad_button_at(col, row);
				if (b->plain.key != NEEDED[i]) {
					continue;
				}
				const qdos_pad_action* a = qdos_pad_action_for(b, QDOS_PAD_ALPHA);
				found = (a != NULL && a->key == NEEDED[i]);
			}
		}
		CHECK(found);
	}
}

/** A modifier is not a key: pressing it must never type anything. */
static void test_modifiers_send_nothing(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			if (!is_any_modifier(b)) {
				continue;
			}
			CHECK(b->plain.key == QDOS_KEY_NONE && b->plain.text == NULL);
		}
	}
}

/** @brief The label with no surrounding spaces, as the cap prints it */
static void trimmed(const char* text, char* out, size_t cap) {
	while (*text == ' ') {
		text++;
	}
	size_t len = strlen(text);
	while (len > 0 && text[len - 1] == ' ') {
		len--;
	}
	if (len >= cap) {
		len = cap - 1;
	}
	memcpy(out, text, len);
	out[len] = '\0';
}

/**
 * Case carries meaning: a lower-case cap is exactly the word, so it can be
 * typed as printed, and a capitalised one is QDOS's own shorthand. A cap that
 * is neither says nothing.
 */
static void test_cap_case_says_whether_it_is_the_word(void) {
	for (int row = 0; row < QDOS_PAD_ROWS; row++) {
		for (int col = 0; col < QDOS_PAD_COLS; col++) {
			const qdos_pad_button* b = qdos_pad_button_at(col, row);
			const qdos_pad_action* layers[3] = {&b->plain, &b->alpha, &b->symbol};

			for (int l = 0; l < 3; l++) {
				const char* label = layers[l]->label;
				if (label == NULL || (unsigned char)label[0] < 32) {
					continue;
				}

				int lower = 0, upper = 0;
				for (const char* c = label; *c; c++) {
					if (*c >= 'a' && *c <= 'z') {
						lower++;
					}
					if (*c >= 'A' && *c <= 'Z') {
						upper++;
					}
				}
				CHECK(lower == 0 || upper == 0); // never a mixture

				// The alphabet is a keyboard, which prints capitals and types small
				const bool is_letter_key = (l == 1);
				if (lower == 0 || layers[l]->text == NULL || is_letter_key) {
					continue;
				}

				char want[32];
				trimmed(layers[l]->text, want, sizeof(want));
				if (strcmp(label, want) != 0) {
					fprintf(stderr, "  cap '%s' types '%s'\n", label, want);
				}
				CHECK(strcmp(label, want) == 0);
			}
		}
	}
}

/**
 * @brief Write the keypad as a PPM, for looking at it
 *
 * The pad is drawn without SDL so it can be tested; the same property means it
 * can be rendered to a file and inspected, which is how it gets designed.
 */
static int dump_ppm(const char* path, qdos_pad_layer layer, const qdos_pad_button* pressed) {
	const int w = QDOS_WINDOW_W, h = QDOS_WINDOW_H;
	uint8_t* buf = calloc((size_t)w * h * 3, 1);
	if (buf == NULL) {
		return 1;
	}

	qdos_frame_draw(buf, w);
	qdos_pad_draw(buf, w, QDOS_PAD_X, QDOS_PAD_Y, layer, pressed);

	FILE* f = fopen(path, "wb");
	if (f == NULL) {
		free(buf);
		return 1;
	}
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(buf, 1, (size_t)w * h * 3, f);
	fclose(f);
	free(buf);
	return 0;
}

int main(int argc, char** argv) {
	if (argc > 2 && strcmp(argv[1], "--dump") == 0) {
		const qdos_pad_layer layer = (argc > 3 && strcmp(argv[3], "alpha") == 0)	? QDOS_PAD_ALPHA
									 : (argc > 3 && strcmp(argv[3], "symbol") == 0) ? QDOS_PAD_SYMBOL
																					: QDOS_PAD_PLAIN;
		// A fourth argument sinks one key, so the press can be looked at too
		const qdos_pad_button* pressed =
				(argc > 4) ? qdos_pad_button_at(atoi(argv[4]) % QDOS_PAD_COLS, atoi(argv[4]) / QDOS_PAD_COLS) : NULL;
		return dump_ppm(argv[2], layer, pressed);
	}

	test_every_slot_is_filled();
	test_labels_fit_their_button();
	test_the_font_has_every_keycap_character();
	test_shift_legends_fit_above_their_key();
	test_a_legend_sits_nearer_its_own_key();
	test_the_soft_keys_have_blank_caps();
	test_a_modifier_is_not_known_by_its_cap();
	test_a_blank_cap_draws_nothing();
	test_hit_testing();
	test_window_geometry();
	test_a_pressed_key_is_drawn_sunk();
	test_every_key_can_be_pressed();
	test_key_travel_fits_the_gutter();
	test_the_nameplate_is_printed_on_the_case();
	test_frame_paints_only_the_border();
	test_shift_layer();
	test_numeric_block();
	test_digits_survive_every_layer();
	test_the_alphabet_is_complete();
	test_what_the_letters_displaced_is_still_reachable();
	test_a_space_is_reachable();
	test_default_layer_is_the_calculator();
	test_default_layer_reaches_the_calculator();
	test_alpha_layer_has_the_alphabet();
	test_alpha_keeps_the_editing_keys();
	test_modifiers_send_nothing();
	test_cap_case_says_whether_it_is_the_word();
	test_draw_stays_in_bounds();
	return check_report("padui");
}
