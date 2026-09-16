/**
 * @file test_shell.c
 * @brief End-to-end shell test over a scripted backend
 */

#include "check.h"

#include "../src/ui/console.h"
#include "../src/ui/font16x24.h"

#include <qdos/hal.h>
#include <qdos/shell.h>

#include "qdos_version.h"
#include "shell/mathwords.h"
#include "shell/storage.h"

#include <stdlib.h>

typedef struct {
	const qdos_key_event* script;
	size_t count;
	size_t next;
	bool served; ///< A key has already gone out on this pass
	uint8_t last_fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	int presents;

	/** Where to stop, so a screen part-way through a script can be read */
	size_t stop_after;
	uint8_t stopped_fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	bool stopped;

	/*
	 * A clock that stands still unless a test winds it on, so every script that
	 * is not about the blink sees the cursor exactly where it always was.
	 */
	uint32_t ms;	  ///< Fake monotonic clock
	uint32_t ms_step; ///< What each wait() adds: idling is what passes time

	size_t waits;	   ///< wait() calls so far
	size_t wait_budget; ///< Passes to keep running for after the script is spent
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

	// Keep the screen as it stood after stop_after keys. The final frame is a
	// different mode by then, so a page a script only passes through cannot be
	// read any other way.
	if (st->stop_after > 0 && !st->stopped && st->next >= st->stop_after) {
		memcpy(st->stopped_fb, fb, sizeof(st->stopped_fb));
		st->stopped = true;
	}
}

/**
 * One key per pass, because the shell renders once per drain of the queue. A
 * keypad does not hand over the whole script at once, and handing it over that
 * way would mean no test ever saw a screen between the first key and the last.
 */
static bool stub_poll_key(qdos_hal* hal, qdos_key_event* out) {
	stub_state* st = hal->impl;
	if (st->served) {
		st->served = false;
		return false;
	}
	if (st->next >= st->count)
		return false;

	*out = st->script[st->next++];
	st->served = true;
	return true;
}

static bool stub_running(qdos_hal* hal) {
	stub_state* st = hal->impl;
	return st->next < st->count || st->waits < st->wait_budget;
}

static uint32_t stub_ticks_ms(qdos_hal* hal) {
	return ((stub_state*)hal->impl)->ms;
}

static void stub_wait(qdos_hal* hal, int timeout_ms) {
	(void)timeout_ms;
	stub_state* st = hal->impl;
	st->waits++;
	st->ms += st->ms_step;
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

/*
 * A machine with a USB gadget, which only some backends have. Off by default,
 * so the settings page has the two rows every other test expects.
 */
static bool g_usb_supported;
static bool g_usb_shared;
static bool g_usb_fails; ///< A gadget that refuses to switch
static int g_usb_calls;

static void store_reset(void) {
	memset(g_store, 0, sizeof(g_store));
	g_usb_supported = false;
	g_usb_shared = false;
	g_usb_fails = false; // or one failing-gadget test poisons every later one
	g_usb_calls = 0;
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

/** Put a program in one of the read-only stores */
static void seed_scope(qdos_store_scope scope, const char* name, const char* source) {
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used)
			continue;
		snprintf(g_store[i].name, sizeof(g_store[i].name), "%s.qd", name);
		memcpy(g_store[i].data, source, strlen(source));
		g_store[i].len = strlen(source);
		g_store[i].scope = scope;
		g_store[i].used = true;
		return;
	}
}

static void seed_system(const char* name, const char* source) {
	seed_scope(QDOS_SCOPE_SYSTEM, name, source);
}

/** As if a PC had dropped the file on the card */
static void seed_inbox(const char* name, const char* source) {
	seed_scope(QDOS_SCOPE_INBOX, name, source);
}

/** Put a setting straight into the store, as other firmware might have left it */
static void seed_setting(const char* key, int64_t number) {
	qdos_value value;
	memset(&value, 0, sizeof(value));
	value.type = QDOS_VALUE_INT;
	value.i = number;

	uint8_t buf[QDOS_VALUE_ENCODED_MAX];
	size_t len = 0;
	CHECK(qdos_value_encode(&value, buf, &len));

	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used)
			continue;
		snprintf(g_store[i].name, sizeof(g_store[i].name), "%s", key);
		memcpy(g_store[i].data, buf, len);
		g_store[i].len = len;
		g_store[i].scope = QDOS_SCOPE_USER;
		g_store[i].used = true;
		return;
	}
}

static int stub_usb_export(qdos_hal* hal, bool on) {
	(void)hal;
	g_usb_calls++;
	if (g_usb_fails)
		return -1;

	g_usb_shared = on;
	return 0;
}

static void stub_hal(qdos_hal* hal, stub_state* st) {
	memset(hal, 0, sizeof(*hal));
	hal->init = stub_init;
	hal->shutdown = stub_shutdown;
	hal->present = stub_present;
	hal->poll_key = stub_poll_key;
	hal->running = stub_running;
	hal->ticks_ms = stub_ticks_ms;
	hal->wait = stub_wait;
	hal->store_read = stub_read;
	hal->store_write = stub_write;
	hal->store_list = stub_list;
	hal->usb_export = g_usb_supported ? stub_usb_export : NULL;
	hal->impl = st;
}

/* Where the shell draws things, mirroring shell.c's layout. */
#define SOFT_WIDTH_T (QDOS_COLS / 5)
#define ROW_HEADER_T 0
#define ROW_CONTENT_FIRST_T 1
#define ROW_CONTENT_LAST_T (QDOS_ROWS - 3)
#define ROW_TOP_VALUE ROW_CONTENT_LAST_T
/* A message takes the input line rather than a row of its own */
#define ROW_INPUT_LINE (QDOS_ROWS - 2)
#define ROW_MESSAGE_LINE ROW_INPUT_LINE

/* Settings rows, in the order the page lists them */
#define ROW_SETTING_ANGLE ROW_CONTENT_FIRST_T
#define ROW_SETTING_DECIMALS (ROW_CONTENT_FIRST_T + 1)
#define ROW_SETTING_AUTO_OFF (ROW_CONTENT_FIRST_T + 2)
#define ROW_SETTING_USB (ROW_CONTENT_FIRST_T + 3)

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
	// A cell is QDOS_FONT_SCALE screen pixels per font pixel, so map back.
	// The last pixel row is skipped: a rule underlines a row of content there,
	// and no glyph reaches it, so it says nothing about which character this is.
	for (int y = 0; y < QDOS_CELL_H - 1; y++) {
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

		// The drawn symbols read back as themselves, so a cap showing an arrow
		// can still be checked
		for (char ch = QDOS_GLYPH_FIRST; found == ' ' && ch <= QDOS_GLYPH_LAST; ch++) {
			if (cell_is(fb, col, row, ch, false) || cell_is(fb, col, row, ch, true))
				found = ch;
		}
		out[len++] = found;
	}

	// Trim trailing blanks
	while (len > 0 && out[len - 1] == ' ')
		len--;
	out[len] = '\0';
}


/**
 * @brief Run a key script and keep two screens
 * @param stop_after Which key to also keep the screen after, or 0 for none
 * @param fb_out     The final screen
 * @param mid_out    The screen after @p stop_after keys, or NULL
 */
static void run_script_capturing(const qdos_key_event* script, size_t count, size_t stop_after, uint8_t* fb_out,
		uint8_t* mid_out) {
	stub_state st;
	memset(&st, 0, sizeof(st));
	st.script = script;
	st.count = count;
	st.stop_after = stop_after;

	qdos_hal hal;
	stub_hal(&hal, &st);

	qdos_shell* sh = qdos_shell_create(&hal);
	CHECK(sh != NULL);
	qdos_shell_run(sh);
	qdos_shell_destroy(sh);

	CHECK(st.presents > 0);
	memcpy(fb_out, st.last_fb, (size_t)QDOS_SCREEN_W * QDOS_SCREEN_H);

	if (mid_out != NULL) {
		CHECK(st.stopped);
		memcpy(mid_out, st.stopped_fb, (size_t)QDOS_SCREEN_W * QDOS_SCREEN_H);
	}
}

/** Run a key script and return the resulting screen. */
static void run_script(const qdos_key_event* script, size_t count, uint8_t* fb_out) {
	run_script_capturing(script, count, 0, fb_out, NULL);
}

/**
 * @brief Run a script, then keep idling with the clock running
 * @param step	 Milliseconds each idle pass takes
 * @param passes Idle passes to allow once the script is spent
 * @param presents_out Repaints over the whole run, or NULL
 */
static size_t run_script_idling(const qdos_key_event* script, size_t count, uint32_t step, size_t passes,
		uint8_t* fb_out, int* presents_out) {
	stub_state st;
	memset(&st, 0, sizeof(st));
	st.script = script;
	st.count = count;
	st.ms_step = step;

	// The script spends one pass per key, so the budget has to cover it first
	st.wait_budget = count + passes;

	qdos_hal hal;
	stub_hal(&hal, &st);

	qdos_shell* sh = qdos_shell_create(&hal);
	CHECK(sh != NULL);
	qdos_shell_run(sh);
	qdos_shell_destroy(sh);

	memcpy(fb_out, st.last_fb, (size_t)QDOS_SCREEN_W * QDOS_SCREEN_H);
	if (presents_out != NULL)
		*presents_out = st.presents;

	// Short of the budget means the shell stopped of its own accord
	return st.waits;
}

/** Does any content row of this screen hold this text? */
static bool page_has(const uint8_t* fb, const char* text) {
	char row[QDOS_COLS + 1];
	for (int r = ROW_CONTENT_FIRST_T; r <= ROW_CONTENT_LAST_T; r++) {
		read_row(fb, r, row, sizeof(row));
		if (strstr(row, text) != NULL)
			return true;
	}
	return false;
}

/** As run_script(), but also returns the screen part-way through. */
static void run_script_mid(
		const qdos_key_event* script, size_t count, size_t stop_after, uint8_t* fb_out, uint8_t* mid_out) {
	run_script_capturing(script, count, stop_after, fb_out, mid_out);
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
	CHECK(strstr(row, "ZERO DIVISOR") != NULL);
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

	// The error is on the input line, so the prompt is not: still line mode
	// underneath, which the next test checks by typing on through it
	CHECK(row[0] != ':');
}

/** A word can be declared and then forgotten. */
static void test_forget_a_word(void) {
	store_reset();

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "fn sqr(x:i64 -- r:i64) { dup * }");
	type_line(script, &n, "7 sqr");
	type_line(script, &n, "\"sqr\" forget");
	type_line(script, &n, "clear 7 sqr");

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
	type_line(first, &n, "fn sqr(x:i64 -- r:i64) { dup * }");
	type_line(first, &n, "\"sqr\" forget");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(first, n, fb);

	qdos_key_event second[64];
	n = 0;
	type_line(second, &n, "clear 7 sqr");
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
	CHECK(strstr(row, "USER") != NULL);
}

/** Tab widens from the installed programs to the catalog. */
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
	CHECK(strstr(row, "CATALOG") != NULL);

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

/** +/- while typing signs the entry, which a bare minus cannot do. */
static void test_neg_signs_the_entry(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "5");
	key(script, &n, QDOS_KEY_NEG);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "-5") != NULL);
}

/** Pressing it twice puts the sign back. */
static void test_neg_toggles(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "5");
	key(script, &n, QDOS_KEY_NEG);
	key(script, &n, QDOS_KEY_NEG);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "-") == NULL);
	CHECK(strstr(row, "5") != NULL);
}

/** With nothing being typed it negates what is on the stack, as an HP does. */
static void test_neg_negates_x(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_NEG);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "-7") != NULL);
}

/** A signed entry still works as the left operand. */
static void test_neg_then_arithmetic(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "3");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "5");
	key(script, &n, QDOS_KEY_NEG);
	key(script, &n, QDOS_KEY_ADD);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "-2") != NULL);
}

/** The catalog opens straight to every word, without going via the apps list. */
static void test_catalog_opens_directly(void) {
	store_reset();

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_CATALOG);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "CATALOG") != NULL);

	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(row[0] != '\0');
}

/** Picking from the catalog types the word, which is what it is for. */
static void test_catalog_pick_types_the_word(void) {
	store_reset();

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_CATALOG);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 2, row, sizeof(row));
	CHECK(row[0] == ':');
	CHECK(strlen(row) > 1); // something was typed
}

/** The maths words reach the stack in calculator mode, not just in a line. */
static void test_function_keys_apply(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "9");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_SQRT);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "3") != NULL);
}

/** A function key commits the number being typed first, as an operator does. */
static void test_function_key_commits_entry(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "16");
	key(script, &n, QDOS_KEY_SQRT);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "4") != NULL);
}

/** A word of yours shadows a built-in, and forgetting it brings the built-in back. */
static void test_user_word_shadows_builtin(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "fn sq(x:i64 -- r:i64) { dup * 1000 + }");
	type_line(script, &n, "7 sq");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1049") != NULL);

	n = 0;
	type_line(script, &n, "fn sq(x:i64 -- r:i64) { dup * 1000 + }");
	type_line(script, &n, "\"sq\" forget");
	type_line(script, &n, "clear 7 sq");
	run_script(script, n, fb);

	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "49") != NULL); // the built-in, not the shadow
	CHECK(strstr(row, "1049") == NULL);
}

/** Typing a letter jumps the catalog to it, or 100 words would be unusable. */
static void test_catalog_letter_jump(void) {
	store_reset();

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_CATALOG);
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, 's'};

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(row[1] == 's'); // the pane starts at the first s word
}

/** Editing means nothing in the catalog, so the key is not offered there. */
static void test_catalog_hides_edit(void) {
	store_reset();

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_CATALOG);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "EDIT") == NULL);
	CHECK(strstr(row, "PICK") != NULL);
}

/** The about view reports the firmware it is actually running. */
static void test_about_shows_the_version(void) {
	store_reset();

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_ABOUT);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "ABOUT") != NULL);

	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "QDOS") != NULL);
	CHECK(strstr(row, QDOS_VERSION) != NULL);

	read_row(fb, ROW_CONTENT_FIRST_T + 1, row, sizeof(row));
	CHECK(strstr(row, "BUILD") != NULL);
	CHECK(strlen(row) > strlen("BUILD ")); // a commit, not an empty label
}

/** Escape puts you back where you came from. */
static void test_about_returns(void) {
	store_reset();

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_ABOUT);
	key(script, &n, QDOS_KEY_CLEAR);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "INFO") != NULL); // the calculator's own soft row
}

/** Rotate reaches the third entry, which nothing else on the keypad can. */
static void test_rot_in_calculator_mode(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "1");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "2");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "3");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_ROT);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1") != NULL); // the one that was third
	read_row(fb, ROW_TOP_VALUE - 1, row, sizeof(row));
	CHECK(strstr(row, "3") != NULL);
	read_row(fb, ROW_TOP_VALUE - 2, row, sizeof(row));
	CHECK(strstr(row, "2") != NULL);
}

/** Three of them is a full turn, so the order comes back. */
static void test_rot_three_times_is_a_full_turn(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "1");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "2");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "3");
	key(script, &n, QDOS_KEY_ENTER);
	for (int i = 0; i < 3; i++)
		key(script, &n, QDOS_KEY_ROT);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "3") != NULL);
}

/** over copies the second entry, and also had no way to run outside a line. */
static void test_over_in_calculator_mode(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "9");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_OVER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL);
	read_row(fb, ROW_TOP_VALUE - 2, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL); // still where it was
}

/**
 * A type error in the runtime used to end the process. If this regresses the
 * test binary dies rather than failing, which is the point.
 */
static void test_fatal_runtime_errors_are_survivable(void) {
	static const char* const DEADLY[] = {
			"1.5 2.5 and", "1 2.5 mod", "1.5 shl", "\"s\" sqrt", "\"s\" sin",
			"0.0 0.0 fac", "1 0 /", "5 ln ln ln ln",
	};

	for (size_t i = 0; i < sizeof(DEADLY) / sizeof(*DEADLY); i++) {
		store_reset();

		qdos_key_event script[64];
		size_t n = 0;
		type_line(script, &n, DEADLY[i]);
		type_line(script, &n, "1 2 +"); // and it still works afterwards

		static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
		run_script(script, n, fb);

		char row[QDOS_COLS + 1];
		read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
		CHECK(strstr(row, "3") != NULL);
	}
}

/** The division key divides. Quadrate's own `/` still truncates, by design. */
static void test_division_key_is_not_integer_division(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "22");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_DIV);
	digits(script, &n, "100");
	key(script, &n, QDOS_KEY_MUL);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "314") != NULL); // truncating would have given 300
}

/** An exact division stays whole, so the bitwise words still take it. */
static void test_exact_division_stays_whole(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "10");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "5");
	key(script, &n, QDOS_KEY_DIV);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "2") != NULL);
	CHECK(strstr(row, ".") == NULL);
}

/** What a program prints reaches the panel, not just stdout. */
static void test_print_reaches_the_panel(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "\"HELLO\" print");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "HELLO") != NULL);
}

/**
 * Printing still works after a forget. `forget` restores a shipped program by
 * evaluating it, so an evaluation runs inside an evaluation -- and the output
 * capture has to come back out of that in one piece, or every later print is
 * swallowed for the rest of the session.
 */
static void test_print_survives_a_nested_evaluation(void) {
	store_reset();
	seed_system("hyp", "fn hyp( -- r:i64) { 5 }");

	qdos_key_event script[256];
	size_t n = 0;
	type_line(script, &n, "fn hyp( -- r:i64) { 9 }"); // override the shipped one
	type_line(script, &n, "\"hyp\" forget");			 // and put it back
	type_line(script, &n, "\"HELLO\" print");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "HELLO") != NULL);
}

/** The debug page keeps what was printed and what went wrong. */
static void test_debug_page_keeps_a_log(void) {
	store_reset();

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "\"FIRST\" print");
	type_line(script, &n, "1 0 /");
	key(script, &n, QDOS_KEY_DEBUG);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "DEBUG") != NULL);

	bool printed = false, failed = false;
	for (int r = ROW_CONTENT_FIRST_T; r <= QDOS_ROWS - 4; r++) {
		read_row(fb, r, row, sizeof(row));
		if (strstr(row, "FIRST") != NULL)
			printed = true;
		if (strstr(row, "ZERO") != NULL || strstr(row, "zero") != NULL)
			failed = true;
	}
	CHECK(printed);
	CHECK(failed);
}

/**
 * The keypad types into the line as well as acting on the stack. A keypad has
 * no letters on it, so these keys are how anything gets typed at all.
 */
static void test_keypad_types_into_the_line(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	digits(script, &n, "1234567890.");
	key(script, &n, QDOS_KEY_ADD);
	key(script, &n, QDOS_KEY_SUB);
	key(script, &n, QDOS_KEY_MUL);
	key(script, &n, QDOS_KEY_DIV);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "1234567890.+-*/") != NULL);
}

/** The stack keys spell their word out rather than acting on the spot. */
static void test_keypad_words_reach_the_line(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	key(script, &n, QDOS_KEY_DUP);
	key(script, &n, QDOS_KEY_SWAP);
	key(script, &n, QDOS_KEY_DROP);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "dupswapdrop") != NULL);
}

/** A word typed by its key has room around it, so it runs the line it lands in. */
static void test_a_typed_function_word_evaluates(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	digits(script, &n, "9");
	key(script, &n, QDOS_KEY_SQRT); // " sqrt ", not "9sqrt"
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "3") != NULL);
}

/** +/- is a word in the line, where it cannot sign an entry that is not there. */
static void test_neg_key_types_the_word(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	digits(script, &n, "5");
	key(script, &n, QDOS_KEY_NEG);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "-5") != NULL);
}

/** `cls` takes the message down, which is the only way to clear one. */
static void test_cls_clears_the_message(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "\"HELLO\" print");
	const size_t printed_at = n;
	type_more(script, &n, "cls");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t printed[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, printed_at, fb, printed);

	char row[QDOS_COLS + 1];
	read_row(printed, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "HELLO") != NULL);

	// With the message down, the line the message was covering is back
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "HELLO") == NULL);
	CHECK(row[0] == ':');
}

/**
 * A message has no row of its own: it takes the input line while it is up, and
 * the next key takes the line back. That is what buys the stack its eighth row.
 */
static void test_a_message_borrows_the_input_line(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "1 0 divide"); // refused
	const size_t erred_at = n;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, '7'};

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t errored[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, erred_at, fb, errored);

	// The error sits where the prompt was, and is inverted because it is one
	char row[QDOS_COLS + 1];
	read_row(errored, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "ZERO DIVISOR") != NULL);
	CHECK(row[0] != ':');
	CHECK(cell_is(errored, 0, ROW_INPUT_LINE, row[0], true));

	// One key later the line is the user's again, with that key on it
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "ZERO DIVISOR") == NULL);
	CHECK(row[0] == ':');
	CHECK(strchr(row, '7') != NULL);
}

/** The row the message used to keep for itself is the stack's now. */
static void test_the_stack_reaches_the_reclaimed_row(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "1 2 3 4 5 6 7 8"); // one more than the old layout held

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// All eight are on screen, the deepest at the very top row
	char row[QDOS_COLS + 1];
	read_row(fb, 0, row, sizeof(row));
	CHECK(strstr(row, "8:") != NULL);
	CHECK(strstr(row, "...") == NULL);

	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1:") != NULL);
}

/** Deeper than that still says so rather than quietly dropping the rest. */
static void test_a_deeper_stack_still_marks_itself(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "1 2 3 4 5 6 7 8 9");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, 0, row, sizeof(row));
	CHECK(strstr(row, "...") != NULL);
}

/**
 * A clean boot says nothing. The calculator being on screen is the whole
 * announcement, and a line saying so is one the user has to clear.
 */
static void test_a_clean_boot_has_nothing_to_say(void) {
	store_reset();

	// No keys at all: the shell paints once before it reads any, so this is
	// the screen as the machine comes up
	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(NULL, 0, fb);

	// Nothing announced, so the input line is the input line
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(row[0] == '>');
	CHECK(row[1] == '\0'); // the prompt and the cursor, and no words
}

/** A stack that came back is worth a word, or the numbers are unexplained. */
static void test_a_restored_session_is_announced(void) {
	store_reset();

	qdos_key_event first[32];
	size_t n = 0;
	digits(first, &n, "42");
	key(first, &n, QDOS_KEY_ENTER);
	key(first, &n, QDOS_KEY_POWER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(first, n, fb);

	// Up again, with the saved stack behind it
	run_script(NULL, 0, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "SESSION RESTORED") != NULL);

	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL);
}

/* ---------------------------------------------------------------------- */
/* Uploaded programs                                                      */
/*                                                                        */
/* A .qd file dropped on the card is a word at the next boot, shipped-like */
/* rather than user-like: it can be overridden but not written over.       */
/* ---------------------------------------------------------------------- */

/** What lands on the card is a word, without anything being pressed. */
static void test_an_uploaded_program_is_a_word(void) {
	store_reset();
	seed_inbox("triple", "fn triple(x:i64 -- r:i64) { 3 * }");

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_ENTER);
	type_line(script, &n, "triple");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "21") != NULL);
}

/**
 * The bug the inbox scope exists to fix. An upload used to be copied into the
 * user store at boot, so editing it on the calculator lasted until power-off.
 */
static void test_an_edit_outlives_the_upload_it_replaced(void) {
	store_reset();
	seed_inbox("triple", "fn triple(x:i64 -- r:i64) { 3 * }");

	qdos_key_event script[256];
	size_t n = 0;
	type_line(script, &n, "fn triple(x:i64 -- r:i64) { 30 * }"); // the user's own
	type_more(script, &n, "2 triple");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "60") != NULL);

	// The card's copy is untouched under it, and the edit survives a power
	// cycle rather than being overwritten on the way back up
	qdos_key_event again[64];
	size_t m = 0;
	digits(again, &m, "2");
	key(again, &m, QDOS_KEY_ENTER);
	type_line(again, &m, "triple");

	run_script(again, m, fb);
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "60") != NULL);
	CHECK(strstr(row, "6 ") == NULL);
}

/** The list says where each program came from, so it says where to remove it. */
static void test_the_list_marks_an_uploaded_program(void) {
	store_reset();
	seed_inbox("fromcard", "fn fromcard( -- r:i64) { 1 }");

	qdos_key_event script[32];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "fromcard") != NULL);
	CHECK(strstr(row, "CARD") != NULL);
}

/** An override is starred, the same as one covering a shipped program. */
static void test_the_list_stars_an_override_of_the_card(void) {
	store_reset();
	seed_inbox("both", "fn both( -- r:i64) { 1 }");

	qdos_key_event script[256];
	size_t n = 0;
	type_line(script, &n, "fn both( -- r:i64) { 2 }");
	key(script, &n, QDOS_KEY_LIST);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "both") != NULL);
	CHECK(strstr(row, "USER*") != NULL);
}

/** Forgetting an uploaded program says where it actually lives. */
static void test_forget_refuses_an_uploaded_program(void) {
	store_reset();
	seed_inbox("fromcard", "fn fromcard( -- r:i64) { 1 }");

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "\"fromcard\" forget");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "ON THE CARD") != NULL);
}

/** And dropping it from the list says the same thing. */
static void test_dropping_an_uploaded_program_is_refused(void) {
	store_reset();
	seed_inbox("fromcard", "fn fromcard( -- r:i64) { 1 }");

	qdos_key_event script[32];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);
	key(script, &n, QDOS_KEY_BACKSPACE);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "TAKE IT OFF THE CARD") != NULL);
}

/** Forgetting an override of an upload brings the card's copy back. */
static void test_forget_reverts_to_the_card(void) {
	store_reset();
	seed_inbox("triple", "fn triple(x:i64 -- r:i64) { 3 * }");

	qdos_key_event script[256];
	size_t n = 0;
	type_line(script, &n, "fn triple(x:i64 -- r:i64) { 30 * }");
	type_more(script, &n, "\"triple\" forget");
	type_more(script, &n, "2 triple");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "6") != NULL);
	CHECK(strstr(row, "60") == NULL);
}

/* ---------------------------------------------------------------------- */
/* Sharing the card over USB                                              */
/* ---------------------------------------------------------------------- */

/** Move the selection to the USB row, which sits below the other three. */
static void open_usb_setting(qdos_key_event* script, size_t* n) {
	key(script, n, QDOS_KEY_SETTINGS);
	key(script, n, QDOS_KEY_DOWN);
	key(script, n, QDOS_KEY_DOWN);
	key(script, n, QDOS_KEY_DOWN);
}

/** A machine with no gadget does not offer the row at all. */
static void test_usb_is_hidden_without_a_gadget(void) {
	store_reset(); // leaves g_usb_supported false

	qdos_key_event script[16];
	size_t n = 0;
	open_usb_setting(script, &n);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_SETTING_USB, row, sizeof(row));
	CHECK(row[0] == '\0');
	CHECK(g_usb_calls == 0);

	// The selection stopped on AUTO OFF, so Enter stepped that instead
	read_row(fb, ROW_SETTING_AUTO_OFF, row, sizeof(row));
	CHECK(strstr(row, "30 MIN") != NULL);
}

/** Where there is a gadget, the row is there and says which way round it is. */
static void test_usb_shares_the_card_and_takes_it_back(void) {
	store_reset();
	g_usb_supported = true;

	qdos_key_event script[16];
	size_t n = 0;
	open_usb_setting(script, &n);
	key(script, &n, QDOS_KEY_ENTER); // hand it over
	const size_t shared_at = n;
	key(script, &n, QDOS_KEY_ENTER); // and take it back

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t shared[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, shared_at, fb, shared);

	char row[QDOS_COLS + 1];
	read_row(shared, ROW_SETTING_USB, row, sizeof(row));
	CHECK(strstr(row, "USB") != NULL);
	CHECK(strstr(row, "SHARED") != NULL);

	read_row(fb, ROW_SETTING_USB, row, sizeof(row));
	CHECK(strstr(row, "OFF") != NULL);

	// Both ways round really reached the backend, and it ended up back with us
	CHECK(g_usb_calls == 2);
	CHECK(!g_usb_shared);
}

/** Taking the card back is when an upload becomes a word. */
static void test_taking_the_card_back_declares_what_landed(void) {
	store_reset();
	g_usb_supported = true;
	seed_inbox("fromcard", "fn fromcard( -- r:i64) { 1 }");

	qdos_key_event script[16];
	size_t n = 0;
	open_usb_setting(script, &n);
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "1 FROM THE CARD") != NULL);
}

/** Reloading the card must not cost the user the stack they were working on. */
static void test_taking_the_card_back_keeps_the_stack(void) {
	store_reset();
	g_usb_supported = true;
	seed_inbox("noisy", "1 2 3"); // a file that runs rather than declares

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, "40");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "2");
	key(script, &n, QDOS_KEY_ADD);
	open_usb_setting(script, &n);
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_CLEAR); // back to the calculator to read the stack

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL);
}

/** A gadget that will not switch says so rather than lying about the state. */
static void test_a_failed_usb_switch_is_reported(void) {
	store_reset();
	g_usb_supported = true;
	g_usb_fails = true;

	qdos_key_event script[16];
	size_t n = 0;
	open_usb_setting(script, &n);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "WOULD NOT SWITCH") != NULL);

	// And the row still reads OFF, because nothing was handed over
	read_row(fb, ROW_SETTING_USB, row, sizeof(row));
	CHECK(strstr(row, "OFF") != NULL);
}

/** Print more lines than the debug page can hold at once. */
static void log_nine_lines(qdos_key_event* script, size_t* n) {
	type_line(script, n, "\"L0\" print");
	for (int i = 1; i < 9; i++) {
		char text[32];
		snprintf(text, sizeof(text), "\"L%d\" print", i);
		type_more(script, n, text);
	}
}

/** The page opens on the newest lines, which are the ones worth seeing. */
static void test_debug_page_opens_at_the_end(void) {
	store_reset();

	qdos_key_event script[512];
	size_t n = 0;
	log_nine_lines(script, &n);
	key(script, &n, QDOS_KEY_DEBUG);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(page_has(fb, "L8"));
	CHECK(!page_has(fb, "L0"));
}

/**
 * Scrolling reaches both ends of the log and stops there. Pressed past the end
 * it must hold still, not walk off into whatever is next in memory.
 */
static void test_debug_page_scrolls_to_both_ends(void) {
	store_reset();

	qdos_key_event script[768];
	size_t n = 0;
	log_nine_lines(script, &n);
	key(script, &n, QDOS_KEY_DEBUG);

	// Far more than the log holds, in both directions
	const size_t at_top = n + 40;
	for (int i = 0; i < 40; i++)
		key(script, &n, QDOS_KEY_UP);
	for (int i = 0; i < 40; i++)
		key(script, &n, QDOS_KEY_DOWN);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t top[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, at_top, fb, top);

	// Up as far as it goes shows the oldest line and holds there
	CHECK(page_has(top, "L0"));
	CHECK(!page_has(top, "L8"));

	// And down as far as it goes comes back to the newest
	CHECK(page_has(fb, "L8"));
	CHECK(!page_has(fb, "L0"));
}

/** Backspace throws the log away, which is the only way to empty it. */
static void test_debug_page_clears_the_log(void) {
	store_reset();

	qdos_key_event script[512];
	size_t n = 0;
	log_nine_lines(script, &n);
	key(script, &n, QDOS_KEY_DEBUG);
	key(script, &n, QDOS_KEY_BACKSPACE);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(page_has(fb, "NOTHING LOGGED"));
	CHECK(!page_has(fb, "L8"));

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "DEBUG") != NULL);
	CHECK(strstr(row, "0") != NULL); // the count in the corner agrees
}

/** The page is a detour, so it goes back to whichever mode opened it. */
static void test_debug_page_returns(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST); // open a page with a header of its own
	key(script, &n, QDOS_KEY_DEBUG);
	key(script, &n, QDOS_KEY_CLEAR);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t debug[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, 2, fb, debug);

	char row[QDOS_COLS + 1];
	read_row(debug, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "DEBUG") != NULL);

	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "DEBUG") == NULL); // back to the list, not the calculator
	CHECK(strstr(row, "APPS") != NULL);
}

/** Settings change the machine, not just the screen. */
static void test_settings_change_angle_mode(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_ENTER); // ANGLE is the first row
	key(script, &n, QDOS_KEY_CLEAR);
	digits(script, &n, "30");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_SIN);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "0.5") != NULL); // radians would have given 0.988

	qdos_math_set_degrees(false); // the setting outlives this shell
}

/** Decimals settle how much of a float is shown. */
static void test_settings_change_decimals(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_DOWN);
	for (int i = 0; i < 3; i++)
		key(script, &n, QDOS_KEY_ENTER); // AUTO -> 0 -> 1 -> 2
	key(script, &n, QDOS_KEY_CLEAR);
	digits(script, &n, "2");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_SQRT);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1.41") != NULL);
	CHECK(strstr(row, "1.414") == NULL);
}

/**
 * The settings page says what it is about to change. Every other test presses
 * through it to check the machine changed, so this is the one that looks at it.
 */
static void test_settings_page_lists_the_settings(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_CLEAR); // leave again, so the page is passed through

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t page[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, 1, fb, page);

	char row[QDOS_COLS + 1];
	read_row(page, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "SETTINGS") != NULL);

	read_row(page, ROW_SETTING_ANGLE, row, sizeof(row));
	CHECK(strstr(row, "ANGLE") != NULL);
	CHECK(strstr(row, "RAD") != NULL); // radians until asked otherwise

	read_row(page, ROW_SETTING_DECIMALS, row, sizeof(row));
	CHECK(strstr(row, "DECIMALS") != NULL);
	CHECK(strstr(row, "AUTO") != NULL);

	read_row(page, ROW_SETTING_AUTO_OFF, row, sizeof(row));
	CHECK(strstr(row, "AUTO OFF") != NULL);
	CHECK(strstr(row, "10 MIN") != NULL); // longer than a calculator's usual five

	// Clear goes back where it came from, rather than leaving the page up
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "SETTINGS") == NULL);
}

/** The selected row is the inverted one, so a press has a visible target. */
static void test_settings_marks_the_selection(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_DOWN);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t opened[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, 1, fb, opened);

	// ANGLE is selected when the page opens; the names start one cell in
	CHECK(cell_is(opened, 1, ROW_CONTENT_FIRST_T, 'A', true));
	CHECK(cell_is(opened, 1, ROW_CONTENT_FIRST_T + 1, 'D', false));

	// And down moves the mark to DECIMALS
	CHECK(cell_is(fb, 1, ROW_CONTENT_FIRST_T + 1, 'D', true));
	CHECK(cell_is(fb, 1, ROW_CONTENT_FIRST_T, 'A', false));
}

/** Angle mode is one setting with two values, so either direction toggles it. */
static void test_settings_angle_toggles_both_ways(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_RIGHT);
	key(script, &n, QDOS_KEY_LEFT);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t degrees[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, 2, fb, degrees);

	char row[QDOS_COLS + 1];
	read_row(degrees, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "DEG") != NULL);

	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "RAD") != NULL);

	CHECK(!qdos_math_degrees()); // and the machine is as it was found
}

/** Decimals run out at both ends and come round, rather than sticking. */
static void test_settings_decimals_wrap_downwards(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_LEFT); // AUTO has nothing below it

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T + 1, row, sizeof(row));
	CHECK(strstr(row, "9") != NULL); // so it wraps to the most there is
	CHECK(strstr(row, "AUTO") == NULL);
}

/** The selection stops at the ends instead of running off them. */
static void test_settings_selection_stops_at_the_ends(void) {
	store_reset();

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_UP);	// already at the top
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN); // already at the bottom, with no gadget
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];

	// Neither press moved off the list, so Enter changed the last row and the
	// two above it were left exactly as they were
	read_row(fb, ROW_SETTING_ANGLE, row, sizeof(row));
	CHECK(strstr(row, "RAD") != NULL);

	read_row(fb, ROW_SETTING_DECIMALS, row, sizeof(row));
	CHECK(strstr(row, "AUTO") != NULL);

	read_row(fb, ROW_SETTING_AUTO_OFF, row, sizeof(row));
	CHECK(strstr(row, "30 MIN") != NULL);
}

/** A fixed setting is a column to read down, so whole numbers get decimals too. */
static void test_fixed_decimals_apply_to_integers(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_DOWN);
	for (int i = 0; i < 3; i++)
		key(script, &n, QDOS_KEY_ENTER); // AUTO -> 0 -> 1 -> 2
	key(script, &n, QDOS_KEY_CLEAR);
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "7.00") != NULL);
}

/** Text is not a number and gets none of this. */
static void test_fixed_decimals_leave_strings_alone(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_DOWN);
	for (int i = 0; i < 3; i++)
		key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_CLEAR);
	type_line(script, &n, "\"text\"");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "text") != NULL);
	CHECK(strstr(row, ".00") == NULL);
}

/** @brief Run three keys and say whether the shell stopped before reading them all */
static bool stops_the_shell(qdos_key first) {
	store_reset();

	stub_state st;
	memset(&st, 0, sizeof(st));
	qdos_key_event script[3] = {{first, 0}, {QDOS_KEY_1, 0}, {QDOS_KEY_2, 0}};
	st.script = script;
	st.count = 3;

	qdos_hal hal;
	stub_hal(&hal, &st);
	qdos_shell* sh = qdos_shell_create(&hal);
	CHECK(sh != NULL);
	qdos_shell_run(sh);
	qdos_shell_destroy(sh);

	return st.next < st.count;
}

/**
 * Turning off belongs to PWR alone. The soft row used to carry an OFF that
 * arrived as SOFT5, one press away from CAT and INFO, which is close company
 * for the one key on the row that cannot be undone by pressing it again.
 */
static void test_only_the_power_key_stops_the_shell(void) {
	CHECK(stops_the_shell(QDOS_KEY_POWER));
	CHECK(!stops_the_shell(QDOS_KEY_SOFT5));
}

/** Undo puts back what the last operation consumed. */
static void test_undo_restores_the_stack(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "9");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_ADD);
	key(script, &n, QDOS_KEY_UNDO);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "9") != NULL);
	read_row(fb, ROW_TOP_VALUE - 1, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL);
}

/** Dropping a program asks once, then takes it off the list. */
static void test_delete_a_program(void) {
	store_reset();
	seed_system("shipped", "fn shipped( -- ){}");

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "fn mine( -- ) { 1 }");
	key(script, &n, QDOS_KEY_LIST);
	key(script, &n, QDOS_KEY_BACKSPACE);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "AGAIN") != NULL); // it asked rather than acting

	key(script, &n, QDOS_KEY_BACKSPACE);
	run_script(script, n, fb);

	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "1/1") != NULL); // only the shipped one is left
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "shipped") != NULL);
}

/** A shipped program cannot be dropped, there being no way to put it back. */
static void test_delete_refuses_system(void) {
	store_reset();
	seed_system("shipped", "fn shipped( -- ){}");

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);
	key(script, &n, QDOS_KEY_BACKSPACE);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "SHIPPED") != NULL);
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "1/1") != NULL);
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

/* Where the caret sits after two digits: past the prompt, past both of them. */
#define CURSOR_COL 4

/** The cursor goes dark between blinks. */
static void test_cursor_blinks_off(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	digits(script, &n, "12");

	// A clock that never moves, which is every other script in this file
	static uint8_t lit[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, lit);
	CHECK(cell_is(lit, CURSOR_COL, ROW_INPUT_LINE, ' ', true));

	// One idle pass, longer than half a period, and the block has gone
	static uint8_t dark[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_idling(script, n, 600, 1, dark, NULL);
	CHECK(cell_is(dark, CURSOR_COL, ROW_INPUT_LINE, ' ', false));
}

/** Typing puts the cursor back on, so a keypress is never a dark cell. */
static void test_a_key_lights_the_cursor(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	digits(script, &n, "123"); // an odd number of keys, so the blink parity flips

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_idling(script, n, 600, 0, fb, NULL);

	// The caret has moved one right, and is lit however the toggling fell out
	CHECK(cell_is(fb, CURSOR_COL + 1, ROW_INPUT_LINE, ' ', true));
}

/** Left alone, the cursor settles solid and the repainting stops. */
static void test_cursor_settles_when_left_alone(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	digits(script, &n, "12");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	int presents = 0;
	run_script_idling(script, n, 600, 60, fb, &presents);
	CHECK(cell_is(fb, CURSOR_COL, ROW_INPUT_LINE, ' ', true));

	// Twice as long ignored, and not one more repaint. That is the property the
	// backend needs: nothing to wake up for, so it can wait on the keypad alone.
	int presents_later = 0;
	run_script_idling(script, n, 600, 120, fb, &presents_later);
	CHECK(presents_later == presents);
}

/** A page with no caret on it never blinks, whatever the clock does. */
static void test_pages_do_not_blink(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_ABOUT);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	int presents = 0;
	run_script_idling(script, n, 600, 4, fb, &presents);

	int presents_later = 0;
	run_script_idling(script, n, 600, 8, fb, &presents_later);
	CHECK(presents_later == presents);
	CHECK(page_has(fb, "ABOUT") || page_has(fb, "QDOS"));
}

/**
 * Settings outlive the machine being off, which for auto-off is the whole
 * point: the setting that turned it off has to still be there when it returns.
 *
 * Angle is deliberately not checked here. It lives in a process-wide global, so
 * a second shell would read it back whether or not anything was ever stored.
 */
static void test_settings_survive_a_power_cycle(void) {
	store_reset();

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];

	qdos_key_event first[16];
	size_t n = 0;
	key(first, &n, QDOS_KEY_SETTINGS);
	key(first, &n, QDOS_KEY_DOWN);
	key(first, &n, QDOS_KEY_ENTER); // DECIMALS: AUTO -> 0
	key(first, &n, QDOS_KEY_DOWN);
	key(first, &n, QDOS_KEY_LEFT); // AUTO OFF: 10 MIN -> 5 MIN
	run_script(first, n, fb);

	// A second shell over the same store, which is what a power cycle is
	qdos_key_event second[8];
	n = 0;
	key(second, &n, QDOS_KEY_SETTINGS);
	run_script(second, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_SETTING_AUTO_OFF, row, sizeof(row));
	CHECK(strstr(row, "5 MIN") != NULL);

	read_row(fb, ROW_SETTING_DECIMALS, row, sizeof(row));
	CHECK(strstr(row, "AUTO") == NULL);
	CHECK(strstr(row, "0") != NULL);
}

/** A store written by firmware that knew more settings must not be obeyed. */
static void test_a_nonsense_setting_is_ignored(void) {
	store_reset();

	// A timeout index this build has no minute count for
	seed_setting("settings.autooff", 99);

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_SETTING_AUTO_OFF, row, sizeof(row));
	CHECK(strstr(row, "10 MIN") != NULL); // the default, not a reading off the end
}

/** Left alone past the timeout, the machine warns and then turns itself off. */
static void test_auto_off_warns_then_stops(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	digits(script, &n, "12");

	// Ten-second steps, so a pass lands inside the warning window rather than
	// stepping straight over it
	const size_t budget = 80;
	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	const size_t waits = run_script_idling(script, n, 10000, budget, fb, NULL);

	// It stopped on its own rather than running out of budget
	CHECK(waits < n + budget);

	// And the last thing on screen was the warning, not a silent death
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "TURNING OFF") != NULL);
}

/** Set to NEVER, it sits there however long it is ignored. */
static void test_auto_off_never_stays_on(void) {
	store_reset();

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN); // AUTO OFF
	key(script, &n, QDOS_KEY_LEFT); // 10 MIN -> 5 MIN
	key(script, &n, QDOS_KEY_LEFT); // 5 MIN -> NEVER

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	int presents = 0;

	// A minute a pass, so this is an hour of being ignored
	const size_t budget = 60;
	const size_t waits = run_script_idling(script, n, 60000, budget, fb, &presents);

	CHECK(waits == n + budget); // ran the budget out, so it never turned off

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_SETTING_AUTO_OFF, row, sizeof(row));
	CHECK(strstr(row, "NEVER") != NULL);
}

/** A card handed to a PC is not a machine to switch off underneath it. */
static void test_auto_off_waits_for_the_card(void) {
	store_reset();
	g_usb_supported = true;

	qdos_key_event script[16];
	size_t n = 0;
	open_usb_setting(script, &n);
	key(script, &n, QDOS_KEY_ENTER); // hand the inbox over

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	const size_t budget = 60;
	const size_t waits = run_script_idling(script, n, 60000, budget, fb, NULL);

	CHECK(waits == n + budget);
	CHECK(g_usb_shared); // still handed over, rather than pulled out from under
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
	CHECK(strstr(row, "INFO") != NULL);
	CHECK(strstr(row, "OFF") == NULL); // turning off is the PWR key's, not a soft key's

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
	CHECK(strstr(row, QDOS_GLYPH_DOWN) != NULL);
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
	CHECK(strstr(row, "INFO") != NULL);

	// Out of the editor, without saving
	n = 0;
	key(script, &n, QDOS_KEY_SOFT2);
	key(script, &n, QDOS_KEY_SOFT5); // edit
	key(script, &n, QDOS_KEY_SOFT1);
	run_script(script, n, fb);
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "INFO") != NULL);

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
	test_print_reaches_the_panel();
	test_print_survives_a_nested_evaluation();
	test_a_message_borrows_the_input_line();
	test_the_stack_reaches_the_reclaimed_row();
	test_a_deeper_stack_still_marks_itself();
	test_a_clean_boot_has_nothing_to_say();
	test_a_restored_session_is_announced();
	test_an_uploaded_program_is_a_word();
	test_an_edit_outlives_the_upload_it_replaced();
	test_the_list_marks_an_uploaded_program();
	test_the_list_stars_an_override_of_the_card();
	test_forget_refuses_an_uploaded_program();
	test_dropping_an_uploaded_program_is_refused();
	test_forget_reverts_to_the_card();
	test_usb_is_hidden_without_a_gadget();
	test_usb_shares_the_card_and_takes_it_back();
	test_taking_the_card_back_declares_what_landed();
	test_taking_the_card_back_keeps_the_stack();
	test_a_failed_usb_switch_is_reported();
	test_keypad_types_into_the_line();
	test_keypad_words_reach_the_line();
	test_a_typed_function_word_evaluates();
	test_neg_key_types_the_word();
	test_cls_clears_the_message();
	test_debug_page_keeps_a_log();
	test_debug_page_opens_at_the_end();
	test_debug_page_scrolls_to_both_ends();
	test_debug_page_clears_the_log();
	test_debug_page_returns();
	test_settings_page_lists_the_settings();
	test_settings_marks_the_selection();
	test_settings_angle_toggles_both_ways();
	test_settings_decimals_wrap_downwards();
	test_settings_selection_stops_at_the_ends();
	test_settings_change_angle_mode();
	test_settings_change_decimals();
	test_fixed_decimals_apply_to_integers();
	test_fixed_decimals_leave_strings_alone();
	test_undo_restores_the_stack();
	test_only_the_power_key_stops_the_shell();
	test_delete_a_program();
	test_delete_refuses_system();
	test_division_key_is_not_integer_division();
	test_exact_division_stays_whole();
	test_fatal_runtime_errors_are_survivable();
	test_rot_in_calculator_mode();
	test_rot_three_times_is_a_full_turn();
	test_over_in_calculator_mode();
	test_about_shows_the_version();
	test_about_returns();
	test_user_word_shadows_builtin();
	test_catalog_opens_directly();
	test_catalog_letter_jump();
	test_catalog_hides_edit();
	test_catalog_pick_types_the_word();
	test_function_keys_apply();
	test_function_key_commits_entry();
	test_neg_signs_the_entry();
	test_neg_toggles();
	test_neg_negates_x();
	test_neg_then_arithmetic();
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
	test_cursor_blinks_off();
	test_a_key_lights_the_cursor();
	test_cursor_settles_when_left_alone();
	test_pages_do_not_blink();
	test_settings_survive_a_power_cycle();
	test_a_nonsense_setting_is_ignored();
	test_auto_off_warns_then_stops();
	test_auto_off_never_stays_on();
	test_auto_off_waits_for_the_card();
	return check_report("shell");
}
