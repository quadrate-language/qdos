/**
 * @file test_shell.c
 * @brief End-to-end shell test over a scripted backend
 *
 * Drives the real shell with a HAL that replays a fixed key sequence and keeps
 * the last framebuffer, which exercises the whole path — key handling, the
 * evaluator, and rendering — without a display. Reading results back out of the
 * pixels is the point: it is the only way to test what the user actually sees.
 */

#include "check.h"

#include "../src/ui/console.h"

#include <qdos/hal.h>
#include <qdos/shell.h>

#include <stdlib.h>

typedef struct {
	const qdos_key_event* script;
	size_t count;
	size_t next;
	uint8_t last_fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	int presents;
} stub_state;

static int stub_init(qdos_hal* hal) {
	(void)hal;
	return 0;
}

static void stub_shutdown(qdos_hal* hal) {
	(void)hal;
}

static void stub_present(qdos_hal* hal, const uint8_t* fb) {
	stub_state* st = hal->impl;
	memcpy(st->last_fb, fb, sizeof(st->last_fb));
	st->presents++;
}

static bool stub_poll_key(qdos_hal* hal, qdos_key_event* out) {
	stub_state* st = hal->impl;
	if (st->next >= st->count)
		return false;
	*out = st->script[st->next++];
	return true;
}

static bool stub_running(qdos_hal* hal) {
	stub_state* st = hal->impl;
	return st->next < st->count;
}

static void stub_idle(qdos_hal* hal) {
	(void)hal;
}

/* In-memory storage, enough slots for registers and a saved session. */
#define STORE_SLOTS 80
#define STORE_BYTES 512

static struct {
	char name[32];
	uint8_t data[STORE_BYTES];
	size_t len;
	bool used;
} g_store[STORE_SLOTS];

static void store_reset(void) {
	memset(g_store, 0, sizeof(g_store));
}

static qdos_store_result stub_read(qdos_hal* h, const char* n, void* b, size_t c, size_t* l) {
	(void)h;
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used && strcmp(g_store[i].name, n) == 0) {
			if (g_store[i].len > c)
				return QDOS_STORE_TOO_BIG;
			memcpy(b, g_store[i].data, g_store[i].len);
			if (l)
				*l = g_store[i].len;
			return QDOS_STORE_OK;
		}
	}
	return QDOS_STORE_NOT_FOUND;
}

static qdos_store_result stub_write(qdos_hal* h, const char* n, const void* b, size_t l) {
	(void)h;
	if (l > STORE_BYTES)
		return QDOS_STORE_TOO_BIG;

	int slot = -1;
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used && strcmp(g_store[i].name, n) == 0) {
			slot = i;
			break;
		}
		if (!g_store[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return QDOS_STORE_IO_ERROR;

	snprintf(g_store[slot].name, sizeof(g_store[slot].name), "%s", n);
	memcpy(g_store[slot].data, b, l);
	g_store[slot].len = l;
	g_store[slot].used = true;
	return QDOS_STORE_OK;
}

static void stub_hal(qdos_hal* hal, stub_state* st) {
	memset(hal, 0, sizeof(*hal));
	hal->init = stub_init;
	hal->shutdown = stub_shutdown;
	hal->present = stub_present;
	hal->poll_key = stub_poll_key;
	hal->running = stub_running;
	hal->idle = stub_idle;
	hal->store_read = stub_read;
	hal->store_write = stub_write;
	hal->impl = st;
}

/**
 * @brief Does the cell at (col,row) hold this character?
 *
 * @param inverted Match against inverted pixels, as the shell draws errors and
 *                 the cursor
 */
static bool cell_is(const uint8_t* fb, int col, int row, char ch, bool inverted) {
	for (int y = 0; y < QDOS_FONT_H; y++) {
		const uint8_t bits = qdos_font_row(ch, y);
		for (int x = 0; x < QDOS_FONT_W; x++) {
			const size_t i = (size_t)(row * QDOS_FONT_H + y) * QDOS_SCREEN_W + col * QDOS_FONT_W + x;
			const bool dark = fb[i] < 0x80;
			const bool lit = inverted ? !dark : dark;
			if (lit != ((bits & (1u << x)) != 0))
				return false;
		}
	}
	return true;
}

/** Read a row of the framebuffer back as text by matching glyphs. */
static void read_row(const uint8_t* fb, int row, char* out, size_t cap) {
	size_t len = 0;
	for (int col = 0; col < QDOS_COLS && len + 1 < cap; col++) {
		char found = ' ';
		for (char ch = '!'; ch <= '~'; ch++) {
			if (cell_is(fb, col, row, ch, false) || cell_is(fb, col, row, ch, true)) {
				found = ch;
				break;
			}
		}
		out[len++] = found;
	}

	// Trim trailing blanks
	while (len > 0 && out[len - 1] == ' ')
		len--;
	out[len] = '\0';
}

/** Run a key script and return the resulting screen. */
static void run_script(const qdos_key_event* script, size_t count, uint8_t* fb_out) {
	stub_state st;
	memset(&st, 0, sizeof(st));
	st.script = script;
	st.count = count;

	qdos_hal hal;
	stub_hal(&hal, &st);

	qdos_shell* sh = qdos_shell_create(&hal);
	CHECK(sh != NULL);
	qdos_shell_run(sh);
	qdos_shell_destroy(sh);

	CHECK(st.presents > 0);
	memcpy(fb_out, st.last_fb, (size_t)QDOS_SCREEN_W * QDOS_SCREEN_H);
}

static void test_types_and_evaluates(void) {
	// "2 3 +" then Enter — the keypad path, not a string handed to the evaluator
	const qdos_key_event script[] = {
		{QDOS_KEY_2, 0},
		{QDOS_KEY_CHAR, ' '},
		{QDOS_KEY_3, 0},
		{QDOS_KEY_ADD, 0},
		{QDOS_KEY_ENTER, 0},
	};

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, sizeof(script) / sizeof(script[0]), fb);

	char row[QDOS_COLS + 1];

	// The header reports one value on the stack
	read_row(fb, 0, row, sizeof(row));
	CHECK(strstr(row, "depth 1") != NULL);

	// The result sits on the bottom stack line, right-aligned
	read_row(fb, QDOS_ROWS - 5, row, sizeof(row));
	CHECK(strstr(row, "1:") != NULL);
	CHECK(strstr(row, "5") != NULL);
}

static void test_division_by_zero_survives(void) {
	// The machine must still be running, and showing an error, afterwards
	const qdos_key_event script[] = {
		{QDOS_KEY_1, 0},
		{QDOS_KEY_CHAR, ' '},
		{QDOS_KEY_0, 0},
		{QDOS_KEY_DIV, 0},
		{QDOS_KEY_ENTER, 0},
	};

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, sizeof(script) / sizeof(script[0]), fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 3, row, sizeof(row));
	CHECK(strstr(row, "Division by zero") != NULL);
}

static void test_backspace_and_clear(void) {
	// Type 99, back over both, then 7 — the line must read just "7"
	const qdos_key_event script[] = {
		{QDOS_KEY_9, 0},
		{QDOS_KEY_9, 0},
		{QDOS_KEY_BACKSPACE, 0},
		{QDOS_KEY_BACKSPACE, 0},
		{QDOS_KEY_7, 0},
		{QDOS_KEY_ENTER, 0},
	};

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, sizeof(script) / sizeof(script[0]), fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 5, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL);
	CHECK(strstr(row, "9") == NULL);
}

/**
 * The memory keys, which are native C functions registered with the
 * interpreter. This is the shell exposing a machine capability to typed
 * Quadrate rather than only evaluating arithmetic — `42 0 sto` reaches the
 * HAL's storage, and `0 rcl` brings it back.
 */
static void test_native_words_reach_the_hal(void) {
	store_reset();

	// "42 0 sto" then "0 rcl" — typed, evaluated, through registered natives
	const qdos_key_event script[] = {
		{QDOS_KEY_4, 0}, {QDOS_KEY_2, 0}, {QDOS_KEY_CHAR, ' '},
		{QDOS_KEY_0, 0}, {QDOS_KEY_CHAR, ' '},
		{QDOS_KEY_CHAR, 's'}, {QDOS_KEY_CHAR, 't'}, {QDOS_KEY_CHAR, 'o'},
		{QDOS_KEY_ENTER, 0},
		{QDOS_KEY_0, 0}, {QDOS_KEY_CHAR, ' '},
		{QDOS_KEY_CHAR, 'r'}, {QDOS_KEY_CHAR, 'c'}, {QDOS_KEY_CHAR, 'l'},
		{QDOS_KEY_ENTER, 0},
	};

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, sizeof(script) / sizeof(script[0]), fb);

	CHECK(g_store[0].used); // the native actually reached the HAL

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 5, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL); // and the value came back

	read_row(fb, 0, row, sizeof(row));
	CHECK(strstr(row, "depth 1") != NULL);
}

/** An empty register must report itself, not crash or return a stale value. */
static void test_recall_of_empty_register(void) {
	store_reset();

	const qdos_key_event script[] = {
		{QDOS_KEY_7, 0}, {QDOS_KEY_CHAR, ' '},
		{QDOS_KEY_CHAR, 'r'}, {QDOS_KEY_CHAR, 'c'}, {QDOS_KEY_CHAR, 'l'},
		{QDOS_KEY_ENTER, 0},
	};

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, sizeof(script) / sizeof(script[0]), fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 3, row, sizeof(row));
	CHECK(strstr(row, "empty") != NULL);
}

/** Typing a sequence of characters as individual key events. */
static void type_into(qdos_key_event* script, size_t* n, const char* text) {
	for (const char* p = text; *p; p++) {
		qdos_key_event ev = {QDOS_KEY_CHAR, *p};
		if (*p >= '0' && *p <= '9')
			ev = (qdos_key_event){(qdos_key)(QDOS_KEY_0 + (*p - '0')), 0};
		script[(*n)++] = ev;
	}
	script[(*n)++] = (qdos_key_event){QDOS_KEY_ENTER, 0};
}

/** A register can be emptied, and reads as empty afterwards. */
static void test_clear_a_register(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_into(script, &n, "5 3 sto");
	type_into(script, &n, "3 clr");
	type_into(script, &n, "3 rcl");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 3, row, sizeof(row));
	CHECK(strstr(row, "empty") != NULL);
}

/** Strings round-trip, not just numbers. */
static void test_store_a_string(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_into(script, &n, "\"pi\" 9 sto");
	type_into(script, &n, "9 rcl");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 5, row, sizeof(row));
	CHECK(strstr(row, "pi") != NULL);

	read_row(fb, 0, row, sizeof(row));
	CHECK(strstr(row, "depth 1") != NULL);
}

/**
 * The stack survives a power cycle.
 *
 * This is what makes the machine feel instant-on despite a real boot: the user
 * gets back what they left, so the seconds spent booting are not also seconds
 * spent re-entering values.
 */
static void test_session_survives_power_cycle(void) {
	store_reset();

	// First run: leave two values on the stack, then power off
	qdos_key_event first[16];
	size_t n = 0;
	type_into(first, &n, "11 31 +");
	first[n++] = (qdos_key_event){QDOS_KEY_POWER, 0};

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(first, n, fb);

	// Second run: a fresh shell, as after a reboot
	qdos_key_event second[8];
	n = 0;
	type_into(second, &n, "2 *");
	run_script(second, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 5, row, sizeof(row));
	CHECK(strstr(row, "84") != NULL); // 42 restored, then doubled
}

int main(void) {
	test_types_and_evaluates();
	test_division_by_zero_survives();
	test_backspace_and_clear();
	test_native_words_reach_the_hal();
	test_recall_of_empty_register();
	test_clear_a_register();
	test_store_a_string();
	test_session_survives_power_cycle();
	return check_report("shell");
}
