/**
 * @file test_console.c
 * @brief Console and font tests
 */

#include "check.h"

#include "../src/ui/font16x24.h"

#include "../src/ui/console.h"
#include "../src/ui/splash.h"

#include <stdlib.h>

/** Count lit pixels in a character cell. */
static int ink_pixels(const qdos_console* con, int col, int row) {
	int count = 0;
	for (int y = 0; y < QDOS_CELL_H; y++)
		for (int x = 0; x < QDOS_CELL_W; x++) {
			const size_t i = (size_t)(row * QDOS_CELL_H + y) * QDOS_SCREEN_W + col * QDOS_CELL_W + x;
			if (con->fb[i] == con->ink)
				count++;
		}
	return count;
}

static void test_clear(void) {
	qdos_console con;
	qdos_console_init(&con);

	CHECK(con.fb[0] == con.paper);
	CHECK(con.fb[sizeof(con.fb) - 1] == con.paper);
	CHECK(ink_pixels(&con, 0, 0) == 0);
}

static void test_putc(void) {
	qdos_console con;
	qdos_console_init(&con);

	// Every printable character must draw something, or a glyph is missing
	for (char ch = '!'; ch <= '~'; ch++) {
		qdos_console_putc(&con, 0, 0, ch);
		CHECK(ink_pixels(&con, 0, 0) > 0);
	}

	// Space must draw nothing
	qdos_console_putc(&con, 0, 0, ' ');
	CHECK(ink_pixels(&con, 0, 0) == 0);
}

static void test_putc_out_of_bounds(void) {
	qdos_console con;
	qdos_console_init(&con);

	// Must not write outside the framebuffer
	qdos_console_putc(&con, -1, 0, 'X');
	qdos_console_putc(&con, 0, -1, 'X');
	qdos_console_putc(&con, QDOS_COLS, 0, 'X');
	qdos_console_putc(&con, 0, QDOS_ROWS, 'X');

	for (size_t i = 0; i < sizeof(con.fb); i++)
		if (con.fb[i] != con.paper) {
			CHECK(0 && "out-of-bounds putc touched the framebuffer");
			return;
		}
	CHECK(1);
}

static void test_puts_clips(void) {
	qdos_console con;
	qdos_console_init(&con);

	char long_line[QDOS_COLS * 2];
	for (size_t i = 0; i < sizeof(long_line) - 1; i++)
		long_line[i] = 'W';
	long_line[sizeof(long_line) - 1] = '\0';

	const int drawn = qdos_console_puts(&con, 0, 0, long_line);
	CHECK(drawn == QDOS_COLS);
}

static void test_puts_right(void) {
	qdos_console con;
	qdos_console_init(&con);

	qdos_console_puts_right(&con, 0, "42");
	CHECK(ink_pixels(&con, QDOS_COLS - 1, 0) > 0); // '2' in the last cell
	CHECK(ink_pixels(&con, QDOS_COLS - 2, 0) > 0); // '4' beside it
	CHECK(ink_pixels(&con, 0, 0) == 0);			   // nothing at the left edge
}

/** Does this cell hold this character? */
static bool cell_holds(const qdos_console* con, int col, int row, char ch) {
	for (int y = 0; y < QDOS_CELL_H; y++) {
		const uint16_t bits = qdos_font_row(ch, y);
		for (int x = 0; x < QDOS_CELL_W; x++) {
			const size_t i = (size_t)(row * QDOS_CELL_H + y) * QDOS_SCREEN_W + col * QDOS_CELL_W + x;
			if ((con->fb[i] == con->ink) != ((bits & (1u << x)) != 0))
				return false;
		}
	}
	return true;
}

/**
 * Too long to fit keeps the head, and says it was cut.
 *
 * It used to keep the tail, which on a number means dropping the leading digits
 * and leaving something that still reads as an answer.
 */
static void test_puts_right_keeps_the_head(void) {
	qdos_console con;
	qdos_console_init(&con);

	char wide[QDOS_COLS + 8];
	memset(wide, 'a', sizeof(wide));
	wide[0] = 'H';
	wide[sizeof(wide) - 1] = '\0';

	qdos_console_puts_right(&con, 0, wide);
	CHECK(cell_holds(&con, 0, 0, 'H'));
	CHECK(cell_holds(&con, QDOS_COLS - 1, 0, QDOS_ELIDED));
}

/** Within a narrower run, so a value keeps off the label beside it. */
static void test_puts_right_within(void) {
	qdos_console con;
	qdos_console_init(&con);

	qdos_console_puts_right_within(&con, 0, 3, "42");
	CHECK(cell_holds(&con, QDOS_COLS - 1, 0, '2'));
	CHECK(ink_pixels(&con, 0, 0) == 0);

	// Twenty-five characters no longer fit, because three columns are spoken for
	char wide[QDOS_COLS + 1];
	memset(wide, 'a', QDOS_COLS);
	wide[0] = 'H';
	wide[QDOS_COLS] = '\0';

	qdos_console_init(&con);
	qdos_console_puts_right_within(&con, 0, 3, wide);
	CHECK(ink_pixels(&con, 0, 0) == 0); // the first three cells are untouched
	CHECK(ink_pixels(&con, 2, 0) == 0);
	CHECK(cell_holds(&con, 3, 0, 'H'));
	CHECK(cell_holds(&con, QDOS_COLS - 1, 0, QDOS_ELIDED));
}

static void test_invert(void) {
	qdos_console con;
	qdos_console_init(&con);

	const uint8_t before = con.fb[0];
	qdos_console_invert(&con, 0, 0, 1);
	CHECK(con.fb[0] == (uint8_t)(0xFF - before));

	// Inverting twice restores the original
	qdos_console_invert(&con, 0, 0, 1);
	CHECK(con.fb[0] == before);
}

static void test_rule(void) {
	qdos_console con;
	qdos_console_init(&con);

	qdos_console_rule(&con, 2);
	const size_t y = (size_t)(2 * QDOS_CELL_H + QDOS_CELL_H - 1);
	CHECK(con.fb[y * QDOS_SCREEN_W] == con.ink);
	CHECK(con.fb[y * QDOS_SCREEN_W + QDOS_SCREEN_W - 1] == con.ink);
}

/** Print every glyph as pixel art for visual inspection. */
static void dump_font(void) {
	for (char ch = ' '; ch <= '~'; ch++) {
		printf("'%c' (%d)\n", ch, (int)ch);
		for (int row = 0; row < QDOS_CELL_H; row++) {
			const uint16_t bits = qdos_font_row(ch, row);
			for (int col = 0; col < QDOS_CELL_W; col++)
				putchar((bits & (1u << col)) ? '#' : '.');
			putchar('\n');
		}
		putchar('\n');
	}
}

static uint8_t g_presented[QDOS_SCREEN_W * QDOS_SCREEN_H];
static int g_present_calls;

static void capture(qdos_hal* hal, const uint8_t* fb) {
	(void)hal;
	memcpy(g_presented, fb, sizeof(g_presented));
	g_present_calls++;
}

/** The startup screen draws something, and says what machine this is. */
static void test_splash(void) {
	qdos_hal hal;
	memset(&hal, 0, sizeof(hal));
	hal.present = capture;
	g_present_calls = 0;

	qdos_splash_draw(&hal);
	CHECK(g_present_calls == 1);

	int lit = 0;
	for (size_t i = 0; i < sizeof(g_presented); i++)
		if (g_presented[i] < 0x80)
			lit++;
	CHECK(lit > 500); // not a blank screen

	// A backend with no display must be survivable
	qdos_splash_draw(NULL);
	memset(&hal, 0, sizeof(hal));
	qdos_splash_draw(&hal);
	CHECK(1);
}

/** Enlarged text is centred and actually larger. */
static void test_scaled_text(void) {
	qdos_console con;
	qdos_console_init(&con);

	qdos_console_puts_centered(&con, 2, "X", 3);

	int lit = 0, min_x = QDOS_SCREEN_W, max_x = 0;
	for (int y = 0; y < QDOS_SCREEN_H; y++)
		for (int x = 0; x < QDOS_SCREEN_W; x++)
			if (con.fb[(size_t)y * QDOS_SCREEN_W + x] == con.ink) {
				lit++;
				if (x < min_x) min_x = x;
				if (x > max_x) max_x = x;
			}

	CHECK(lit > 0);
	CHECK(max_x - min_x > QDOS_FONT_W); // wider than one unscaled glyph

	// Centred: the margins either side should match within a glyph
	const int left = min_x;
	const int right = QDOS_SCREEN_W - 1 - max_x;
	CHECK(left - right < QDOS_FONT_W * 3 && right - left < QDOS_FONT_W * 3);

	qdos_console_puts_centered(&con, 0, NULL, 3); // must not crash
	qdos_console_puts_centered(&con, 0, "X", 0);
	CHECK(1);
}

/** Glyphs the typeface draws symmetric must survive thresholding that way. */
static void test_font_symmetry(void) {
	for (const char* ch = "AHIMOTUVWXYouvwx8*+=-_|\"^"; *ch; ch++) {
		for (int row = 0; row < QDOS_FONT_H; row++) {
			const uint16_t bits = qdos_font_row(*ch, row);
			uint16_t mirror = 0;
			for (int col = 0; col < QDOS_FONT_W; col++)
				if (bits & (1u << col))
					mirror |= (uint16_t)(1u << (QDOS_FONT_W - 1 - col));
			CHECK(bits == mirror);
		}
	}
}

/** Letters and digits sit on one baseline, whatever shape their terminal is. */
/** How many unlit pixels lie between the leftmost and rightmost ink of a row */
static int interior_gaps(char ch) {
	int gaps = 0;
	for (int row = 0; row < QDOS_FONT_H; row++) {
		const uint16_t bits = qdos_font_row(ch, row);
		if (bits == 0)
			continue;

		int first = -1, last = -1;
		for (int col = 0; col < QDOS_FONT_W; col++) {
			if (bits & (1u << col)) {
				if (first < 0)
					first = col;
				last = col;
			}
		}
		for (int col = first; col < last; col++)
			if (!(bits & (1u << col)))
				gaps++;
	}
	return gaps;
}

/**
 * The zero's slash must not close its counter.
 *
 * Telling O from 0 is the discrimination a calculator most needs, and the
 * slash was once rasterised thick enough to fill most of the bowl, leaving the
 * zero reading as the heavier shape -- backwards, and hard to read at a glance.
 */
static void test_zero_is_not_a_blob(void) {
	const int zero = interior_gaps('0');
	const int oh = interior_gaps('O');

	// The slash costs some of the counter, but nothing like most of it
	CHECK(zero > (oh * 3) / 4);

	// And it is still a slash: the two must not be the same glyph
	bool differs = false;
	for (int row = 0; row < QDOS_FONT_H; row++)
		if (qdos_font_row('0', row) != qdos_font_row('O', row))
			differs = true;
	CHECK(differs);
}

static void test_font_baseline(void) {
	const int baseline = 21;
	for (const char* ch = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
						  "abcdefghijklmnopqrstuvwxyz0123456789";
		 *ch; ch++) {
		int bottom = -1;
		for (int row = 0; row < QDOS_FONT_H; row++)
			if (qdos_font_row(*ch, row))
				bottom = row;
		CHECK(bottom == baseline);
	}
}

int main(int argc, char** argv) {
	if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
		dump_font();
		return 0;
	}

	test_clear();
	test_putc();
	test_putc_out_of_bounds();
	test_puts_clips();
	test_puts_right();
	test_puts_right_keeps_the_head();
	test_puts_right_within();
	test_invert();
	test_rule();
	test_splash();
	test_scaled_text();
	test_font_symmetry();
	test_font_baseline();
	test_zero_is_not_a_blob();
	return check_report("console");
}
