/**
 * @file test_console.c
 * @brief Console and font tests
 *
 * Run with --dump to print the whole font as pixel art. A bitmap font is the
 * kind of data where a wrong pixel passes every automated check and still looks
 * wrong, so the eyeball test is part of the tooling rather than a one-off.
 */

#include "check.h"

#include "../src/ui/console.h"

#include <stdlib.h>

/** Count lit pixels in a character cell. */
static int ink_pixels(const qdos_console* con, int col, int row) {
	int count = 0;
	for (int y = 0; y < QDOS_FONT_H; y++)
		for (int x = 0; x < QDOS_FONT_W; x++) {
			const size_t i = (size_t)(row * QDOS_FONT_H + y) * QDOS_SCREEN_W + col * QDOS_FONT_W + x;
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
	const size_t y = (size_t)(2 * QDOS_FONT_H + QDOS_FONT_H - 1);
	CHECK(con.fb[y * QDOS_SCREEN_W] == con.ink);
	CHECK(con.fb[y * QDOS_SCREEN_W + QDOS_SCREEN_W - 1] == con.ink);
}

/** Print every glyph as pixel art for visual inspection. */
static void dump_font(void) {
	for (char ch = ' '; ch <= '~'; ch++) {
		printf("'%c' (%d)\n", ch, (int)ch);
		for (int row = 0; row < QDOS_FONT_H; row++) {
			const uint8_t bits = qdos_font_row(ch, row);
			for (int col = 0; col < QDOS_FONT_W; col++)
				putchar((bits & (1u << col)) ? '#' : '.');
			putchar('\n');
		}
		putchar('\n');
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
	test_invert();
	test_rule();
	return check_report("console");
}
