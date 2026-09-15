/**
 * @file test_shell.c
 * @brief End-to-end shell test over a scripted backend
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
	qdos_store_scope scope;
	uint8_t data[STORE_BYTES];
	size_t len;
	bool used;
} g_store[STORE_SLOTS];

static void store_reset(void) {
	memset(g_store, 0, sizeof(g_store));
}

static qdos_store_result stub_read(
		qdos_hal* h, qdos_store_scope scope, const char* n, void* b, size_t c, size_t* l) {
	(void)h;
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used && g_store[i].scope == scope && strcmp(g_store[i].name, n) == 0) {
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
		// Writes only ever land in the user store
		if (g_store[i].used && g_store[i].scope == QDOS_SCOPE_USER && strcmp(g_store[i].name, n) == 0) {
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
	g_store[slot].scope = QDOS_SCOPE_USER;
	g_store[slot].used = true;
	return QDOS_STORE_OK;
}

static qdos_store_result stub_list(qdos_hal* hal, qdos_store_scope scope, qdos_store_visit visit, void* user) {
	(void)hal;
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used && g_store[i].scope == scope && !visit(g_store[i].name, user))
			break;
	}
	return QDOS_STORE_OK;
}

/** Put a program in the read-only system store */
static void seed_system(const char* name, const char* source) {
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used)
			continue;
		snprintf(g_store[i].name, sizeof(g_store[i].name), "%s.qd", name);
		memcpy(g_store[i].data, source, strlen(source));
		g_store[i].len = strlen(source);
		g_store[i].scope = QDOS_SCOPE_SYSTEM;
		g_store[i].used = true;
		return;
	}
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
	hal->store_list = stub_list;
	hal->impl = st;
}

/* Where the shell draws things, mirroring shell.c's layout. */
#define SOFT_WIDTH_T (QDOS_COLS / 5)
#define ROW_HEADER_T 0
#define ROW_CONTENT_FIRST_T 2
#define ROW_BOTTOM_RULE_T (QDOS_ROWS - 4)
#define ROW_TOP_VALUE (ROW_BOTTOM_RULE_T - 1)
#define ROW_MESSAGE_LINE (QDOS_ROWS - 3)

/** Press one key. */
static void key(qdos_key_event* script, size_t* n, qdos_key k) {
	script[(*n)++] = (qdos_key_event){k, 0};
}

/** Press the keys for a number, as a keypad would. */
static void digits(qdos_key_event* script, size_t* n, const char* text) {
	for (const char* p = text; *p; p++) {
		if (*p >= '0' && *p <= '9')
			script[(*n)++] = (qdos_key_event){(qdos_key)(QDOS_KEY_0 + (*p - '0')), 0};
		else if (*p == '.')
			script[(*n)++] = (qdos_key_event){QDOS_KEY_DOT, 0};
	}
}

/** Type a whole line in line mode: ':', the text, then Enter. */
static void type_line(qdos_key_event* script, size_t* n, const char* text) {
	script[(*n)++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	for (const char* p = text; *p; p++)
		script[(*n)++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	script[(*n)++] = (qdos_key_event){QDOS_KEY_ENTER, 0};
}

/**
 * Type while already in line mode. ':' is only the way in -- pressing it again
 * just inserts a colon -- so these skip it.
 */
static void type_more(qdos_key_event* script, size_t* n, const char* text) {
	for (const char* p = text; *p; p++)
		script[(*n)++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	script[(*n)++] = (qdos_key_event){QDOS_KEY_ENTER, 0};
}

/** As type_more(), but leaves the line unsubmitted so Tab can be pressed. */
static void type_partial(qdos_key_event* script, size_t* n, const char* text) {
	for (const char* p = text; *p; p++)
		script[(*n)++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
}

/**
 * @brief Does the cell at (col,row) hold this character?
 * @param inverted Match against inverted pixels, as the shell draws errors and
 *                 the cursor
 */
static bool cell_is(const uint8_t* fb, int col, int row, char ch, bool inverted) {
	// A cell is QDOS_FONT_SCALE screen pixels per font pixel, so map back
	for (int y = 0; y < QDOS_CELL_H; y++) {
		const uint16_t bits = qdos_font_row(ch, y / QDOS_FONT_SCALE);
		for (int x = 0; x < QDOS_CELL_W; x++) {
			const size_t i = (size_t)(row * QDOS_CELL_H + y) * QDOS_SCREEN_W + col * QDOS_CELL_W + x;
			const bool dark = fb[i] < 0x80;
			const bool lit = inverted ? !dark : dark;
			if (lit != ((bits & (1u << (x / QDOS_FONT_SCALE))) != 0))
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

/**
 */
static void test_operator_evaluates_immediately(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "6");
	key(script, &n, QDOS_KEY_ENTER); // separates the two numbers, as on an HP
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_MUL); // no Enter: the operator applies

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL);

	CHECK(row[0] == '1' && row[1] == ':'); // one value, so it is row 1
}

/** Digits accumulate until something ends the entry. */
static void test_digits_accumulate(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "123");
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "123") != NULL); // one number, not three
	CHECK(row[0] == '1' && row[1] == ':'); // one value, so it is row 1
}

/** Decimals work, and an operator commits them. */
static void test_decimal_entry(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "1.5");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "2");
	key(script, &n, QDOS_KEY_MUL);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "3") != NULL);
}

/** Enter with nothing typed duplicates the top, as on an RPN calculator. */
static void test_bare_enter_duplicates(void) {
	store_reset();

	// 7 ENTER ENTER * is how such a calculator squares a number
	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_MUL);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "49") != NULL);
}

/** Backspace edits the pending number; clear discards it. */
static void test_entry_editing(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "99");
	key(script, &n, QDOS_KEY_BACKSPACE);
	key(script, &n, QDOS_KEY_BACKSPACE);
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL);
	CHECK(strstr(row, "9") == NULL);
}

/** A failed operation reports itself and leaves the machine usable. */
static void test_operator_error_is_shown(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "1");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "0");
	key(script, &n, QDOS_KEY_DIV);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "Division by zero") != NULL);
}

/** ':' switches to typing Quadrate; the prompt says so. */
static void test_line_mode_prompt(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == ':'); // not '>'
}

/** Escape leaves line mode. */
static void test_escape_leaves_line_mode(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	key(script, &n, QDOS_KEY_CLEAR); // Escape

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == '>'); // back to the calculator
}

/** Line mode stays put, so a word can be defined and then used. */
static void test_line_mode_persists(void) {
	store_reset();

	// One ':' then two lines: the second must still be evaluated as source
	qdos_key_event script[64];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	for (const char* p = "2 3 +"; *p; p++)
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	key(script, &n, QDOS_KEY_ENTER);
	for (const char* p = "10 *"; *p; p++)
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "50") != NULL);

	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == ':'); // still in line mode
}

/** Switching modes commits what was typed rather than losing it. */
static void test_entry_survives_mode_switch(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "12");
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'}; // with 12 half-typed

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "12") != NULL); // committed, not discarded
}

/** Native words reach the HAL, typed in line mode. */
static void test_native_words_reach_the_hal(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "42 0 sto");
	type_line(script, &n, "0 rcl");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(g_store[0].used);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL);
}

/** An empty register says so. */
static void test_recall_of_empty_register(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	type_line(script, &n, "7 rcl");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "EMPTY") != NULL);
}

/** A register can be emptied. */
static void test_clear_a_register(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "5 3 sto");
	type_line(script, &n, "3 clr");
	type_line(script, &n, "3 rcl");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "EMPTY") != NULL);
}

/** Strings round-trip through storage. */
static void test_store_a_string(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "\"pi\" 9 sto");
	type_line(script, &n, "9 rcl");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "pi") != NULL);
}

/** Control flow, typed as a line. */
static void test_control_flow_in_line_mode(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "0 loop { 1 + dup 5 >= if { break } }");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "5") != NULL);
}

/**
 */
static void test_session_survives_power_cycle(void) {
	store_reset();

	qdos_key_event first[32];
	size_t n = 0;
	digits(first, &n, "11");
	key(first, &n, QDOS_KEY_ENTER);
	digits(first, &n, "31");
	key(first, &n, QDOS_KEY_ADD);
	key(first, &n, QDOS_KEY_POWER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(first, n, fb);

	// A fresh shell, as after a reboot
	qdos_key_event second[16];
	n = 0;
	digits(second, &n, "2");
	key(second, &n, QDOS_KEY_MUL);
	run_script(second, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "84") != NULL); // 42 restored, then doubled
}

/**
 */
static void test_open_line_continues(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	for (const char* p = "fn sq(x:i64 -- r:i64) {"; *p; p++)
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	key(script, &n, QDOS_KEY_ENTER); // open: continues
	for (const char* p = "dup *"; *p; p++)
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	key(script, &n, QDOS_KEY_ENTER); // still open
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, '}'};
	key(script, &n, QDOS_KEY_ENTER); // closed: evaluates
	for (const char* p = "7 sq"; *p; p++)
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "49") != NULL); // defined across lines, then called
}

/** While a line is open the prompt says so. */
static void test_continuation_prompt(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, '{'};
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == '.' && row[1] == '.'); // not ':'
}

/** A balanced line still evaluates on Enter. */
static void test_balanced_line_evaluates(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	type_line(script, &n, "223");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "223") != NULL);
	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == ':'); // not continuing
}

/** A brace inside a string is text, not structure. */
static void test_braces_in_strings_do_not_open_a_line(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "\"{\"");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == ':'); // evaluated, not left open

	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(row[0] == '1' && row[1] == ':'); // one value, so it is row 1
}

/** An unmatched closer submits, so the parser can report it. */
static void test_unmatched_closer_still_submits(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	type_line(script, &n, "}");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(row[0] != '\0'); // an error, rather than a line that cannot be sent
	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == ':');
}

/** A word can be declared and then forgotten. */
static void test_forget_a_word(void) {
	store_reset();

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "fn sq(x:i64 -- r:i64) { dup * }");
	type_line(script, &n, "7 sq");
	type_line(script, &n, "\"sq\" forget");
	type_line(script, &n, "clear 7 sq");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "not defined") != NULL); // gone after forgetting
}

/** Forgetting something that was never declared says so. */
static void test_forget_unknown(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "\"nosuch\" forget");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "IS NOT DECLARED") != NULL);
}

/** A declared word is written to the store and comes back after a reboot. */
static void test_program_survives_power_cycle(void) {
	store_reset();

	qdos_key_event first[160];
	size_t n = 0;
	type_line(first, &n, "fn sq(x:i64 -- r:i64) { dup * }");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(first, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "SAVED") != NULL);

	// A fresh shell, as after a reboot: the word is there without redeclaring
	qdos_key_event second[64];
	n = 0;
	type_line(second, &n, "clear 7 sq");
	run_script(second, n, fb);

	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "49") != NULL);
}

/** Forgetting a word also drops it from the store. */
static void test_forget_outlives_the_reboot(void) {
	store_reset();

	qdos_key_event first[160];
	size_t n = 0;
	type_line(first, &n, "fn sq(x:i64 -- r:i64) { dup * }");
	type_line(first, &n, "\"sq\" forget");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(first, n, fb);

	qdos_key_event second[64];
	n = 0;
	type_line(second, &n, "clear 7 sq");
	run_script(second, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "not defined") != NULL);
}

/** Tab completes a declared word, and the completed line then runs. */
static void test_tab_completes_a_word(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "fn wobble( -- r:i64) { 7 }");
	type_partial(script, &n, "clear wob");
	key(script, &n, QDOS_KEY_TAB);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL);
}

/** An ambiguous prefix types the shared part and lists the candidates. */
static void test_tab_lists_ambiguous(void) {
	store_reset();

	qdos_key_event script[240];
	size_t n = 0;
	type_line(script, &n, "fn wobble( -- r:i64) { 1 }");
	type_more(script, &n, "fn wobbly( -- r:i64) { 2 }");
	type_partial(script, &n, "wob");
	key(script, &n, QDOS_KEY_TAB);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "wobbl") != NULL);
}

/** Tab on something that is not a word prefix does nothing. */
static void test_tab_on_no_match(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	type_partial(script, &n, "zzzz");
	key(script, &n, QDOS_KEY_TAB);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "NO MATCH") != NULL);
}

/**
 * Every pixel must sit clearly on one side of the panel's 1-bit cut at 128.
 * A mid-grey would render differently on the Sharp LCD than in the simulator.
 */
static void test_pixels_are_unambiguous(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "fn sq(x:i64 -- r:i64) { dup * }");
	type_more(script, &n, "7 sq");
	type_more(script, &n, "1 0 /"); // an error, so the inverted path is drawn too

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	int ambiguous = 0;
	for (size_t i = 0; i < sizeof(fb); i++) {
		if (fb[i] >= 64 && fb[i] < 192)
			ambiguous++;
	}
	CHECK(ambiguous == 0);
}

/** The list shows the vocabulary, sorted, and picking types the name. */
static void test_list_browses_words(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "APPS") != NULL);

	// Nothing installed, so it says so rather than showing an empty pane
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "NONE") != NULL);
}

/** The installed programs, marked with where each came from. */
static void test_list_shows_apps_and_origin(void) {
	store_reset();
	seed_system("hyp", "fn hyp( -- r:i64) { 5 }");

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "fn mine( -- r:i64) { 1 }");
	key(script, &n, QDOS_KEY_LIST);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "APPS") != NULL);
	CHECK(strstr(row, "/2") != NULL);

	// Sorted: hyp then mine
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "hyp") != NULL);
	CHECK(strstr(row, "SYS") != NULL);

	read_row(fb, ROW_CONTENT_FIRST_T + 1, row, sizeof(row));
	CHECK(strstr(row, "mine") != NULL);
	CHECK(strstr(row, "YOURS") != NULL);
}

/** Tab widens from the installed programs to the whole vocabulary. */
static void test_list_toggles_to_all_words(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);
	key(script, &n, QDOS_KEY_TAB);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "WORDS") != NULL);

	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(row[0] != '\0');
}

static void test_list_moves_and_picks(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);
	key(script, &n, QDOS_KEY_TAB); // no programs installed, so widen to all words
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_UP);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// Picking leaves line mode with the name typed
	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == ':');
	CHECK(strlen(row) > 2);
}

static void test_list_exits(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "42");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_LIST);
	key(script, &n, QDOS_KEY_CLEAR);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// Back to the calculator, stack intact
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL);
}

/** A shipped program cannot be forgotten, only overridden. */
static void test_forget_refuses_system(void) {
	store_reset();
	seed_system("hyp", "fn hyp( -- r:i64) { 5 }");

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "\"hyp\" forget");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "IS BUILT IN") != NULL);
}

/** Forgetting an override restores the shipped version. */
static void test_forget_reverts_to_system(void) {
	store_reset();
	seed_system("hyp", "fn hyp( -- r:i64) { 5 }");

	qdos_key_event script[240];
	size_t n = 0;
	type_line(script, &n, "fn hyp( -- r:i64) { 99 }");
	type_more(script, &n, "\"hyp\" forget");
	type_more(script, &n, "clear hyp");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "5") != NULL);
	CHECK(strstr(row, "99") == NULL);
}

/** edit opens the program in the editor pane, source intact. */
static void test_edit_opens_editor(void) {
	store_reset();
	seed_system("hyp", "fn hyp( -- r:i64) {\n\t5\n}");

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "\"hyp\" edit");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "hyp") != NULL);

	// The body is laid out over the stack area, a line per row
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "fn hyp") != NULL);
	read_row(fb, ROW_CONTENT_FIRST_T + 1, row, sizeof(row));
	CHECK(strstr(row, "5") != NULL);
}

/** An unknown name starts a new program rather than failing. */
static void test_edit_starts_new(void) {
	store_reset();

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "\"fresh\" edit");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "fn fresh") != NULL);
}

/** Saving evaluates and stores; the program then runs. */
static void test_edit_saves(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"two\" edit");
	type_partial(script, &n, "2");
	key(script, &n, QDOS_KEY_SAVE);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "SAVED") != NULL);
}

/** A body that does not parse keeps you in the editor. */
static void test_edit_refuses_broken(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"bad\" edit");
	type_partial(script, &n, "{{{");
	key(script, &n, QDOS_KEY_SAVE);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// Still showing the editor header, not the stack
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "bad") != NULL);
}

/** check compiles and says so, without leaving the editor. */
static void test_edit_check_reports_ok(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"two\" edit");
	type_partial(script, &n, "2");
	key(script, &n, QDOS_KEY_CHECK);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "COMPILES") != NULL);

	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "two") != NULL);
}

/** A body that does not parse is reported as an error, still in the editor. */
static void test_edit_check_reports_error(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"bad\" edit");
	type_partial(script, &n, "{{{");
	key(script, &n, QDOS_KEY_CHECK);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(row[0] != '\0');
	CHECK(strstr(row, "COMPILES") == NULL);
	CHECK(cell_is(fb, 0, ROW_MESSAGE_LINE, row[0], true)); // inverted, so flagged

	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "bad") != NULL);
}

/**
 * A check installs nothing. Saving the same edit would leave the word callable,
 * so the absent 42 is the difference between the two.
 */
static void test_check_leaves_session_alone(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"solo\" edit");
	type_partial(script, &n, "42");
	key(script, &n, QDOS_KEY_CHECK);
	key(script, &n, QDOS_KEY_CLEAR);
	type_line(script, &n, "solo");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") == NULL);
}

/** A space is what keeps two numbers from merging into one. */
static void test_space_separates_numbers(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	key(script, &n, QDOS_KEY_7);
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ' '};
	key(script, &n, QDOS_KEY_8);
	key(script, &n, QDOS_KEY_ADD);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "15") != NULL); // 78 + would have been an error
}

/** clr leaves without writing. */
static void test_edit_discards(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"gone\" edit");
	key(script, &n, QDOS_KEY_CLEAR);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "NOT SAVED") != NULL);
}

/** Arrows move the cursor, so text can be inserted rather than only appended. */
static void test_cursor_inserts(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	type_partial(script, &n, "13");
	key(script, &n, QDOS_KEY_LEFT);
	type_partial(script, &n, "2");
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// "13" with the cursor moved left then "2" typed gives 123
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "123") != NULL);
}

static void test_backspace_at_cursor(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	type_partial(script, &n, "1X23");
	key(script, &n, QDOS_KEY_LEFT);
	key(script, &n, QDOS_KEY_LEFT);
	key(script, &n, QDOS_KEY_BACKSPACE);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "123") != NULL);
}

/** Soft key labels follow the mode. */
static void test_soft_labels_follow_mode(void) {
	store_reset();

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	qdos_key_event script[64];
	size_t n = 0;
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "APPS") != NULL);
	CHECK(strstr(row, "OFF") != NULL);

	n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	run_script(script, n, fb);
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "COMP") != NULL);
	CHECK(strstr(row, "ESC") != NULL);

	// f1 is the way out of every mode, whatever it is called there
	CHECK(strstr(row, "ESC") == strchr(row, 'E'));
	CHECK((size_t)(strstr(row, "ESC") - row) < SOFT_WIDTH_T);
}

/** A soft key does what its label says for the current mode. */
static void test_soft_key_opens_apps(void) {
	store_reset();

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT2);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "APPS") != NULL);

	// and in the list its labels have changed
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "DOWN") != NULL);
	CHECK(strstr(row, "EDIT") != NULL);
}

/** open in the list edits the selected program. */
static void test_soft_open_edits_selection(void) {
	store_reset();
	seed_system("hyp", "fn hyp( -- r:i64) { 5 }");

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT2); // apps
	key(script, &n, QDOS_KEY_SOFT5); // edit

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "hyp") != NULL);
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "SAVE") != NULL);
}

/** f2 is down and f3 is up, so the pair reads like vim's j and k. */
static void test_soft_down_then_up(void) {
	store_reset();
	seed_system("aaa", "fn aaa( -- r:i64) { 1 }");
	seed_system("bbb", "fn bbb( -- r:i64) { 2 }");

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT2); // apps
	key(script, &n, QDOS_KEY_SOFT2); // down

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "2/2") != NULL);

	n = 0;
	key(script, &n, QDOS_KEY_SOFT2); // apps
	key(script, &n, QDOS_KEY_SOFT2); // down
	key(script, &n, QDOS_KEY_SOFT3); // up
	run_script(script, n, fb);
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "1/2") != NULL);
}

/** Whatever the mode, f1 backs out of it. */
static void test_f1_is_always_the_way_out(void) {
	store_reset();
	seed_system("hyp", "fn hyp( -- r:i64) { 5 }");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	qdos_key_event script[32];
	char row[QDOS_COLS + 1];

	// Out of the apps menu
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT2);
	key(script, &n, QDOS_KEY_SOFT1);
	run_script(script, n, fb);
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "OFF") != NULL);

	// Out of the editor, without saving
	n = 0;
	key(script, &n, QDOS_KEY_SOFT2);
	key(script, &n, QDOS_KEY_SOFT5); // edit
	key(script, &n, QDOS_KEY_SOFT1);
	run_script(script, n, fb);
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "OFF") != NULL);

	// Out of line mode
	n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	key(script, &n, QDOS_KEY_SOFT1);
	run_script(script, n, fb);
	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == '>');
}

int main(void) {
	test_operator_evaluates_immediately();
	test_digits_accumulate();
	test_decimal_entry();
	test_bare_enter_duplicates();
	test_entry_editing();
	test_operator_error_is_shown();
	test_line_mode_prompt();
	test_escape_leaves_line_mode();
	test_line_mode_persists();
	test_open_line_continues();
	test_continuation_prompt();
	test_balanced_line_evaluates();
	test_braces_in_strings_do_not_open_a_line();
	test_unmatched_closer_still_submits();
	test_forget_a_word();
	test_forget_unknown();
	test_entry_survives_mode_switch();
	test_native_words_reach_the_hal();
	test_recall_of_empty_register();
	test_clear_a_register();
	test_store_a_string();
	test_control_flow_in_line_mode();
	test_session_survives_power_cycle();
	test_program_survives_power_cycle();
	test_forget_outlives_the_reboot();
	test_tab_completes_a_word();
	test_tab_lists_ambiguous();
	test_tab_on_no_match();
	test_pixels_are_unambiguous();
	test_list_browses_words();
	test_list_shows_apps_and_origin();
	test_list_toggles_to_all_words();
	test_list_moves_and_picks();
	test_list_exits();
	test_forget_refuses_system();
	test_forget_reverts_to_system();
	test_edit_opens_editor();
	test_edit_starts_new();
	test_edit_saves();
	test_edit_refuses_broken();
	test_edit_discards();
	test_space_separates_numbers();
	test_edit_check_reports_ok();
	test_edit_check_reports_error();
	test_check_leaves_session_alone();
	test_soft_labels_follow_mode();
	test_soft_key_opens_apps();
	test_soft_open_edits_selection();
	test_soft_down_then_up();
	test_f1_is_always_the_way_out();
	test_cursor_inserts();
	test_backspace_at_cursor();
	return check_report("shell");
}
