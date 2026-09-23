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

	int last_timeout;	///< What the shell last asked wait() for
	size_t waits;		///< wait() calls so far
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
	if (st->next >= st->count) {
		return false;
	}

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
	stub_state* st = hal->impl;
	st->last_timeout = timeout_ms;
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

/* A card that gains a file while the machine is running */
static bool g_card_arrival;
static const char* g_card_name;
static const char* g_card_source;
static const char* g_card_module;

/* A wall clock, which runs with the fake monotonic one, and a battery. Off by default. */
static bool g_has_clock;
static int g_clock_start; ///< Seconds since midnight when the shell starts
static int g_battery;	  ///< -1 for a backend that asks and finds none
static bool g_has_battery;

/* A keypad with more than one face, which only some backends have */
static qdos_keypad_mod g_modifier;
static bool g_has_modifier;

static void store_reset(void) {
	memset(g_store, 0, sizeof(g_store));
	g_usb_supported = false;
	g_usb_shared = false;
	g_usb_fails = false; // or one failing-gadget test poisons every later one
	g_usb_calls = 0;
	g_modifier = QDOS_MOD_NONE;
	g_has_clock = false;
	g_has_battery = false;
	g_has_modifier = false;
	g_card_arrival = false;
	g_card_name = NULL;
	g_card_source = NULL;
	g_card_module = NULL;
}

static qdos_store_result stub_read(qdos_hal* h, qdos_store_scope scope, const char* n, void* b, size_t c, size_t* l) {
	(void)h;
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used && g_store[i].scope == scope && strcmp(g_store[i].name, n) == 0) {
			if (g_store[i].len > c) {
				return QDOS_STORE_TOO_BIG;
			}
			memcpy(b, g_store[i].data, g_store[i].len);
			if (l) {
				*l = g_store[i].len;
			}
			return QDOS_STORE_OK;
		}
	}
	return QDOS_STORE_NOT_FOUND;
}

static qdos_store_result stub_write(qdos_hal* h, const char* n, const void* b, size_t l) {
	(void)h;
	if (l > STORE_BYTES) {
		return QDOS_STORE_TOO_BIG;
	}

	int slot = -1;
	for (int i = 0; i < STORE_SLOTS; i++) {
		// Writes only ever land in the user store
		if (g_store[i].used && g_store[i].scope == QDOS_SCOPE_USER && strcmp(g_store[i].name, n) == 0) {
			slot = i;
			break;
		}
		if (!g_store[i].used && slot < 0) {
			slot = i;
		}
	}
	if (slot < 0) {
		return QDOS_STORE_IO_ERROR;
	}

	snprintf(g_store[slot].name, sizeof(g_store[slot].name), "%s", n);
	memcpy(g_store[slot].data, b, l);
	g_store[slot].len = l;
	g_store[slot].scope = QDOS_SCOPE_USER;
	g_store[slot].used = true;
	return QDOS_STORE_OK;
}

/**
 * The store is flat, so a folder is whatever stands before a '/' in a name.
 * Listing the card reports each one once with the mark on it; listing a folder
 * reports what is under it, bare.
 */
static qdos_store_result stub_list(
		qdos_hal* hal, qdos_store_scope scope, const char* folder, qdos_store_visit visit, void* user) {
	(void)hal;

	char seen[STORE_SLOTS][QDOS_PROGRAM_NAME_MAX];
	size_t seen_count = 0;

	for (int i = 0; i < STORE_SLOTS; i++) {
		if (!g_store[i].used || g_store[i].scope != scope) {
			continue;
		}

		const char* name = g_store[i].name;
		const char* slash = strchr(name, '/');

		if (folder != NULL) {
			if (slash == NULL || strncmp(name, folder, (size_t)(slash - name)) != 0 || folder[slash - name] != '\0') {
				continue;
			}
			if (!visit(slash + 1, user)) {
				return QDOS_STORE_OK;
			}
			continue;
		}

		if (slash == NULL) {
			if (!visit(name, user)) {
				return QDOS_STORE_OK;
			}
			continue;
		}

		char dir[QDOS_PROGRAM_NAME_MAX];
		snprintf(dir, sizeof(dir), "%.*s/", (int)(slash - name), name);

		bool already = false;
		for (size_t s = 0; s < seen_count; s++) {
			already = already || strcmp(seen[s], dir) == 0;
		}
		if (already) {
			continue;
		}

		snprintf(seen[seen_count++], QDOS_PROGRAM_NAME_MAX, "%s", dir);
		if (!visit(dir, user)) {
			return QDOS_STORE_OK;
		}
	}
	return QDOS_STORE_OK;
}

/** Put an entry in a store under its own name, whatever shape that name is */
static void seed_raw(qdos_store_scope scope, const char* file, const char* bytes) {
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used) {
			continue;
		}
		snprintf(g_store[i].name, sizeof(g_store[i].name), "%s", file);
		memcpy(g_store[i].data, bytes, strlen(bytes));
		g_store[i].len = strlen(bytes);
		g_store[i].scope = scope;
		g_store[i].used = true;
		return;
	}
}

/** Whether anything is on the card under this name, in any scope */
static bool stored(const char* file) {
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used && strcmp(g_store[i].name, file) == 0 && g_store[i].len > 0) {
			return true;
		}
	}
	return false;
}

/** An app: a folder with an entry point in it, as an upload would arrive */
static void seed_app(qdos_store_scope scope, const char* name, const char* source) {
	char key[QDOS_PROGRAM_NAME_MAX * 2];
	snprintf(key, sizeof(key), "%s/" QDOS_APP_MAIN, name);
	seed_raw(scope, key, source);
}

/** Put a program in one of the read-only stores */
static void seed_scope(qdos_store_scope scope, const char* name, const char* source) {
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (g_store[i].used) {
			continue;
		}
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
		if (g_store[i].used) {
			continue;
		}
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
	if (g_usb_fails) {
		return -1;
	}

	g_usb_shared = on;
	return 0;
}

static qdos_keypad_mod stub_modifier(qdos_hal* hal) {
	(void)hal;
	return g_modifier;
}

/* The modules the suite builds sit beside this binary */
#ifndef MODULE_DIR
#define MODULE_DIR "."
#endif

static bool stub_store_changed(qdos_hal* hal) {
	(void)hal;
	if (!g_card_arrival) {
		return false;
	}

	g_card_arrival = false;
	if (g_card_name != NULL) {
		seed_inbox(g_card_name, g_card_source);
	}
	if (g_card_module != NULL) {
		seed_raw(QDOS_SCOPE_INBOX, g_card_module, "");
	}
	return true;
}

static bool stub_store_path(qdos_hal* hal, qdos_store_scope scope, const char* name, char* buf, size_t cap) {
	(void)hal;
	(void)scope;
	const int written = snprintf(buf, cap, "%s/%s", MODULE_DIR, name);
	return written > 0 && (size_t)written < cap;
}

static bool stub_time_of_day(qdos_hal* hal, int* seconds) {
	*seconds = (g_clock_start + (int)(((stub_state*)hal->impl)->ms / 1000)) % 86400;
	return true;
}

static int stub_battery(qdos_hal* hal) {
	(void)hal;
	return g_battery;
}

static void stub_hal(qdos_hal* hal, stub_state* st) {
	memset(hal, 0, sizeof(*hal));
	hal->init = stub_init;
	hal->shutdown = stub_shutdown;
	hal->present = stub_present;
	hal->poll_key = stub_poll_key;
	hal->modifier = g_has_modifier ? stub_modifier : NULL;
	hal->running = stub_running;
	hal->ticks_ms = stub_ticks_ms;
	hal->wait = stub_wait;
	hal->store_read = stub_read;
	hal->store_write = stub_write;
	hal->store_list = stub_list;
	hal->store_path = stub_store_path;
	hal->store_changed = stub_store_changed;
	hal->usb_export = g_usb_supported ? stub_usb_export : NULL;
	hal->time_of_day = g_has_clock ? stub_time_of_day : NULL;
	hal->battery = g_has_battery ? stub_battery : NULL;
	hal->impl = st;
}

/* Where the shell draws things, mirroring shell.c's layout. */
#define SOFT_WIDTH_T (QDOS_COLS / 5)
#define ROW_STATUS_T 0
#define ROW_HEADER_T 1
#define ROW_CONTENT_FIRST_T 2
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

/* The editor's pane, in the small font between the header's rule and its own */
#define EDIT_PANE_TOP_T (ROW_CONTENT_FIRST_T * QDOS_CELL_H)
#define EDIT_PANE_H_T ((ROW_CONTENT_LAST_T + 1) * QDOS_CELL_H - 1 - EDIT_PANE_TOP_T)
#define EDIT_ROWS_T (EDIT_PANE_H_T / QDOS_SMALL_FONT_H)
#define EDIT_COLS_T (QDOS_SCREEN_W / QDOS_SMALL_FONT_W)
#define EDIT_Y0_T (EDIT_PANE_TOP_T + (EDIT_PANE_H_T - EDIT_ROWS_T * QDOS_SMALL_FONT_H) / 2)

/** Press one key. */
static void key(qdos_key_event* script, size_t* n, qdos_key k) {
	script[(*n)++] = (qdos_key_event){k, 0};
}

/** Press the keys for a number, as a keypad would. */
static void digits(qdos_key_event* script, size_t* n, const char* text) {
	for (const char* p = text; *p; p++) {
		if (*p >= '0' && *p <= '9') {
			script[(*n)++] = (qdos_key_event){(qdos_key)(QDOS_KEY_0 + (*p - '0')), 0};
		} else if (*p == '.') {
			script[(*n)++] = (qdos_key_event){QDOS_KEY_DOT, 0};
		}
	}
}

/** Type a whole line in line mode: ':', the text, then Enter. */
static void type_line(qdos_key_event* script, size_t* n, const char* text) {
	script[(*n)++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	for (const char* p = text; *p; p++) {
		script[(*n)++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	}
	script[(*n)++] = (qdos_key_event){QDOS_KEY_ENTER, 0};
}

/**
 * Type while already in line mode. ':' is only the way in -- pressing it again
 * just inserts a colon -- so these skip it.
 */
static void type_more(qdos_key_event* script, size_t* n, const char* text) {
	for (const char* p = text; *p; p++) {
		script[(*n)++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	}
	script[(*n)++] = (qdos_key_event){QDOS_KEY_ENTER, 0};
}

/** As type_more(), but leaves the line unsubmitted so Tab can be pressed. */
static void type_partial(qdos_key_event* script, size_t* n, const char* text) {
	for (const char* p = text; *p; p++) {
		script[(*n)++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	}
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
			if (lit != ((bits & (1u << (x / QDOS_FONT_SCALE))) != 0)) {
				return false;
			}
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
			if (cell_is(fb, col, row, ch, false) || cell_is(fb, col, row, ch, true)) {
				found = ch;
			}
		}
		out[len++] = found;
	}

	// Trim trailing blanks
	while (len > 0 && out[len - 1] == ' ') {
		len--;
	}
	out[len] = '\0';
}

/**
 * @brief Run a key script and keep two screens
 * @param stop_after Which key to also keep the screen after, or 0 for none
 * @param fb_out     The final screen
 * @param mid_out    The screen after @p stop_after keys, or NULL
 */
static void run_script_capturing(
		const qdos_key_event* script, size_t count, size_t stop_after, uint8_t* fb_out, uint8_t* mid_out) {
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
static size_t run_script_idling(
		const qdos_key_event* script, size_t count, uint32_t step, size_t passes, uint8_t* fb_out, int* presents_out) {
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
	if (presents_out != NULL) {
		*presents_out = st.presents;
	}

	// Short of the budget means the shell stopped of its own accord
	return st.waits;
}

/** Does any content row of this screen hold this text? */
/** A small-font cell whose glyph starts at pixel row @p y0 */
static bool small_cell_is(const uint8_t* fb, int col, int y0, char ch, bool inverted) {
	for (int y = 0; y < QDOS_SMALL_FONT_H; y++) {
		const uint8_t bits = qdos_small_font_row(ch, y);
		for (int x = 0; x < QDOS_SMALL_FONT_W; x++) {
			const size_t i = (size_t)(y0 + y) * QDOS_SCREEN_W + col * QDOS_SMALL_FONT_W + x;
			const bool dark = fb[i] < 0x80;
			if ((inverted ? !dark : dark) != ((bits & (1u << x)) != 0)) {
				return false;
			}
		}
	}
	return true;
}

/** Read small-font text starting at pixel row @p y0 back, as read_row() does a row */
static void read_small(const uint8_t* fb, int y0, char* out, size_t cap) {
	size_t len = 0;
	for (int col = 0; col < EDIT_COLS_T && len + 1 < cap; col++) {
		char found = ' ';
		for (char ch = '!'; ch <= '~'; ch++) {
			if (small_cell_is(fb, col, y0, ch, false) || small_cell_is(fb, col, y0, ch, true)) {
				found = ch;
				break;
			}
		}
		out[len++] = found;
	}
	while (len > 0 && out[len - 1] == ' ') {
		len--;
	}
	out[len] = '\0';
}

static void read_edit_line(const uint8_t* fb, int line, char* out, size_t cap) {
	read_small(fb, EDIT_Y0_T + line * QDOS_SMALL_FONT_H, out, cap);
}

/* An error is small, centred in the message line */
#define ERROR_Y0_T (ROW_MESSAGE_LINE * QDOS_CELL_H + (QDOS_CELL_H - QDOS_SMALL_FONT_H) / 2)

static void read_error(const uint8_t* fb, char* out, size_t cap) {
	read_small(fb, ERROR_Y0_T, out, cap);
}

static bool error_cell_is(const uint8_t* fb, int col, char ch, bool inverted) {
	return small_cell_is(fb, col, ERROR_Y0_T, ch, inverted);
}

/* The band's text, small and centred in the top row */
#define STATUS_Y0_T (ROW_STATUS_T * QDOS_CELL_H + (QDOS_CELL_H - QDOS_SMALL_FONT_H) / 2)

static bool status_band_is_black(const uint8_t* fb) {
	for (int y = ROW_STATUS_T * QDOS_CELL_H; y < (ROW_STATUS_T + 1) * QDOS_CELL_H; y++) {
		if (fb[(size_t)y * QDOS_SCREEN_W] >= 0x80 || fb[(size_t)y * QDOS_SCREEN_W + QDOS_SCREEN_W - 1] >= 0x80) {
			return false;
		}
	}
	return true;
}

static bool edit_pane_has(const uint8_t* fb, const char* text) {
	char line[EDIT_COLS_T + 1];
	for (int i = 0; i < EDIT_ROWS_T; i++) {
		read_edit_line(fb, i, line, sizeof(line));
		if (strstr(line, text) != NULL) {
			return true;
		}
	}
	return false;
}

static bool page_has(const uint8_t* fb, const char* text) {
	char row[QDOS_COLS + 1];
	for (int r = ROW_CONTENT_FIRST_T; r <= ROW_CONTENT_LAST_T; r++) {
		read_row(fb, r, row, sizeof(row));
		if (strstr(row, text) != NULL) {
			return true;
		}
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
	CHECK(strstr(row, "123") != NULL);	   // one number, not three
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

/** A point with no digit on one side is still a number on a keypad. */
static void test_bare_point_entry(void) {
	store_reset();

	qdos_key_event script[32];
	size_t n = 0;
	digits(script, &n, ".5");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "3.");
	key(script, &n, QDOS_KEY_MUL);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1.5") != NULL);
	CHECK(row[0] == '1' && row[1] == ':');
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
	read_error(fb, row, sizeof(row));
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
	for (const char* p = "2 3 +"; *p; p++) {
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	}
	key(script, &n, QDOS_KEY_ENTER);
	for (const char* p = "10 *"; *p; p++) {
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	}
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
	read_error(fb, row, sizeof(row));
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
	read_error(fb, row, sizeof(row));
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

/*
 * An array is a pointer, and a pointer renders as the address it holds -- a
 * different number every run, and nothing to anyone reading it. Written out
 * instead, for as long as the row holds it.
 */
static void test_an_array_shows_its_elements(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "[1 2 3]");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "[1 2 3]") != NULL);
	CHECK(strstr(row, "0x") == NULL); // not the address it used to be
}

/** Too wide for the row, so its shape is what is left worth saying */
static void test_a_wide_array_shows_its_shape(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "[100000 200000 300000 400000]");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "[4 i64]") != NULL);
}

/*
 * There is no encoding for an array, so the save stops below it. A stack that
 * comes back shorter than it was left has to say so: the alternative is a value
 * quietly becoming a number nobody entered.
 */
static void test_a_lost_stack_says_so(void) {
	store_reset();

	qdos_key_event first[128];
	size_t n = 0;
	type_line(first, &n, "7");
	type_line(first, &n, "[1 2 3]");
	key(first, &n, QDOS_KEY_POWER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(first, n, fb);

	// A fresh shell with no keys at all, so the boot message is still up
	run_script(NULL, 0, fb);

	char row[QDOS_COLS + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "LOST FROM STACK") != NULL);

	// What was below it is still there
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL);
}

/**
 */
static void test_open_line_continues(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	for (const char* p = "fn sq(x:i64 -- r:i64) { x"; *p; p++) {
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	}
	key(script, &n, QDOS_KEY_ENTER); // open: continues
	for (const char* p = "dup *"; *p; p++) {
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	}
	key(script, &n, QDOS_KEY_ENTER); // still open
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, '}'};
	key(script, &n, QDOS_KEY_ENTER); // closed: evaluates
	for (const char* p = "7 sq"; *p; p++) {
		script[n++] = (qdos_key_event){QDOS_KEY_CHAR, *p};
	}
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
	read_error(fb, row, sizeof(row));
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
	type_line(script, &n, "fn sqr(x:i64 -- r:i64) { x x * }");
	type_line(script, &n, "7 sqr");
	type_line(script, &n, "\"sqr\" forget");
	type_line(script, &n, "clear 7 sqr");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "not defined") != NULL); // gone after forgetting
}

/** An error is small, so one longer than the 24 columns of the large font still ends on screen. */
static void test_a_long_error_is_not_cut_at_24(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "\"somewhatlongname\" forget");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[EDIT_COLS_T + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strlen(row) > QDOS_COLS);
	CHECK(strstr(row, "somewhatlongname") != NULL);
	CHECK(strstr(row, "IS NOT DECLARED") != NULL);
	CHECK(error_cell_is(fb, 0, row[0], true));

	// The band runs the width of the panel, past the end of the text
	const int y = ROW_MESSAGE_LINE * QDOS_CELL_H;
	CHECK(fb[(size_t)y * QDOS_SCREEN_W + QDOS_SCREEN_W - 1] < 0x80);
	CHECK(fb[(size_t)(y + QDOS_CELL_H - 1) * QDOS_SCREEN_W + QDOS_SCREEN_W - 1] < 0x80);
}

/** The clock on the left of the band and the charge on the right, white on black. */
static void test_the_status_band_shows_clock_and_battery(void) {
	store_reset();
	g_has_clock = true;
	g_clock_start = 14 * 3600 + 32 * 60 + 10;
	g_has_battery = true;
	g_battery = 87;

	qdos_key_event script[8];
	size_t n = 0;
	digits(script, &n, "5");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(status_band_is_black(fb));

	char band[EDIT_COLS_T + 1];
	read_small(fb, STATUS_Y0_T, band, sizeof(band));
	CHECK(strncmp(band, " 14:32", 6) == 0);
	CHECK(strlen(band) == EDIT_COLS_T - 1); // ends a cell in from the edge
	CHECK(strcmp(band + strlen(band) - 3, "87%") == 0);
	CHECK(small_cell_is(fb, 1, STATUS_Y0_T, '1', true));
}

/** No clock set and no battery, and the band is still there, saying nothing. */
static void test_the_status_band_stays_when_it_has_nothing(void) {
	store_reset();
	g_has_battery = true;
	g_battery = -1;

	qdos_key_event script[8];
	size_t n = 0;
	digits(script, &n, "5");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(status_band_is_black(fb));
	char band[EDIT_COLS_T + 1];
	read_small(fb, STATUS_Y0_T, band, sizeof(band));
	CHECK(band[0] == '\0');
}

/** Left alone, the shell wakes at the turn of the minute, and only then. */
static void test_the_clock_turns_over_while_idle(void) {
	store_reset();
	g_has_clock = true;
	g_clock_start = 9 * 3600 + 59 * 60 + 45; // 15 seconds to the hour

	qdos_key_event script[8];
	size_t n = 0;
	digits(script, &n, "5");

	stub_state st;
	memset(&st, 0, sizeof(st));
	st.script = script;
	st.count = n;
	st.wait_budget = n + 1;
	st.ms_step = 11000; // past the cursor settling, so the blink is not what is due

	qdos_hal hal;
	stub_hal(&hal, &st);
	qdos_shell* sh = qdos_shell_create(&hal);
	qdos_shell_run(sh);
	qdos_shell_destroy(sh);

	// 11 seconds on is 09:59:56, so asleep for the 4 left in the minute
	CHECK(st.last_timeout == 4000);

	// And when it does, the band shows the new one
	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	int presents = 0;
	run_script_idling(script, n, 20000, 2, fb, &presents);
	char band[EDIT_COLS_T + 1];
	read_small(fb, STATUS_Y0_T, band, sizeof(band));
	CHECK(strstr(band, "10:00") != NULL);
}

/* The graph's readout sits where a message does, small */
#define READOUT_Y0_T ERROR_Y0_T

static bool plot_has_ink(const uint8_t* fb) {
	int lit = 0;
	for (int y = (ROW_STATUS_T + 1) * QDOS_CELL_H; y < (ROW_CONTENT_LAST_T + 1) * QDOS_CELL_H - 1; y++) {
		for (int x = 0; x < QDOS_SCREEN_W; x++) {
			lit += fb[(size_t)y * QDOS_SCREEN_W + x] < 0x80 ? 1 : 0;
		}
	}
	return lit > QDOS_SCREEN_W; // more than an axis
}

/** `"f" graph` plots the word, says what and where, and offers trace. */
static void test_graph_plots_a_word(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "fn sq(x:f64 -- y:f64) { x x * }");
	type_more(script, &n, "\"sq\" graph");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(plot_has_ink(fb));
	CHECK(status_band_is_black(fb));

	char line[EDIT_COLS_T + 1];
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "sq") == line);
	CHECK(strstr(line, "X -10:10") != NULL);
	// Fitted to x^2 at the column centres, 0.025 to 9.975 squared, with 5% either side
	CHECK(strstr(line, "Y -4.974:104.5") != NULL);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "TRACE") != NULL);
	CHECK(strstr(row, "FIT") != NULL);
}

/** Plotting runs the word hundreds of times and leaves the stack as it found it. */
static void test_graph_leaves_the_stack_alone(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	digits(script, &n, "42");
	key(script, &n, QDOS_KEY_ENTER);
	type_line(script, &n, "fn sq(x:f64 -- y:f64) { x x * }");
	type_more(script, &n, "\"sq\" graph");
	key(script, &n, QDOS_KEY_CLEAR);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1:") != NULL);
	CHECK(strstr(row, "42") != NULL);
	read_row(fb, ROW_TOP_VALUE - 1, row, sizeof(row));
	CHECK(strstr(row, "2:") == NULL); // and nothing under it
}

/** Trace puts x and y in the readout, and the arrows walk it along the curve. */
static void test_graph_trace_reads_the_curve(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "fn twice(x:f64 -- y:f64) { x 2 * }");
	type_more(script, &n, "\"twice\" graph");
	key(script, &n, QDOS_KEY_TRACE);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// The middle column of -10..10 over 400 is x = 0.025
	char line[EDIT_COLS_T + 1];
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strcmp(line, "X=0.025  Y=0.05") == 0);

	key(script, &n, QDOS_KEY_RIGHT);
	run_script(script, n, fb);
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strcmp(line, "X=0.075  Y=0.15") == 0);
}

/** A word that is not there says so and leaves the calculator as it was. */
static void test_graph_of_an_unknown_word(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "\"nosuch\" graph");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[EDIT_COLS_T + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "nosuch") != NULL);
	CHECK(error_cell_is(fb, 0, row[0], true));

	char soft[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 1, soft, sizeof(soft));
	CHECK(strstr(soft, "TRACE") == NULL);
}

/** One that does not take x and leave y is told what it has to do. */
static void test_graph_of_the_wrong_shape(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "fn two(x:f64 -- a:f64 b:f64) { x x }");
	type_more(script, &n, "\"two\" graph");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[EDIT_COLS_T + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "MUST TAKE X, LEAVE Y") != NULL);

	// And none of what it left behind is on the stack
	char value[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, value, sizeof(value));
	CHECK(strstr(value, "1:") == NULL);
}

/** `"f" graph3` draws the surface and says where it is seen from. */
static void test_graph3_plots_a_surface(void) {
	store_reset();

	qdos_key_event script[160];
	size_t n = 0;
	digits(script, &n, "42");
	key(script, &n, QDOS_KEY_ENTER);
	type_line(script, &n, "fn rip(x:f64 y:f64 -- z:f64) { x x * y y * + sqrt sin }");
	type_more(script, &n, "\"rip\" graph3");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(plot_has_ink(fb));
	char line[EDIT_COLS_T + 1];
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "rip  AZ 30 EL 25  Z -0.996:1") == line);

	char soft[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 1, soft, sizeof(soft));
	CHECK(strstr(soft, "STD") != NULL);

	// Turned and tilted by the arrows, which only redraw it
	key(script, &n, QDOS_KEY_LEFT);
	key(script, &n, QDOS_KEY_UP);
	key(script, &n, QDOS_KEY_UP);
	store_reset(); // or the session saved by the run before comes back
	run_script(script, n, fb);
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "AZ 15 EL 45") != NULL);

	// Round past zero it reads as a whole turn, and STD brings it back
	key(script, &n, QDOS_KEY_LEFT);
	key(script, &n, QDOS_KEY_LEFT);
	store_reset(); // or the session saved by the run before comes back
	run_script(script, n, fb);
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "AZ 345") != NULL);
	key(script, &n, QDOS_KEY_STD);
	store_reset(); // or the session saved by the run before comes back
	run_script(script, n, fb);
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "AZ 30 EL 25") != NULL);

	// And the stack is as it was
	key(script, &n, QDOS_KEY_CLEAR);
	store_reset(); // or the session saved by the run before comes back
	run_script(script, n, fb);
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL);
	read_row(fb, ROW_TOP_VALUE - 1, row, sizeof(row));
	CHECK(strstr(row, "2:") == NULL);
}

/** A word taking only x is a curve, not a surface, and is told so. */
static void test_graph3_of_the_wrong_shape(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "fn sq(x:f64 -- y:f64) { x x * }");
	type_more(script, &n, "\"sq\" graph3");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[EDIT_COLS_T + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "MUST TAKE X Y, LEAVE Z") != NULL);
}

/* A Y= slot's body, read back small from where the page puts it */
static void read_slot_body(const uint8_t* fb, int slot, char* out, size_t cap) {
	char line[EDIT_COLS_T + 1];
	read_small(
			fb, (ROW_CONTENT_FIRST_T + slot) * QDOS_CELL_H + (QDOS_CELL_H - QDOS_SMALL_FONT_H) / 2, line, sizeof(line));
	// The name is at reading size in the first four cells, eight small ones
	snprintf(out, cap, "%s", strlen(line) > 8 ? line + 8 : "");
}

/* Into Y= from the calculator, onto slot @p slot, and type @p body into it */
static void type_slot(qdos_key_event* script, size_t* n, int slot, const char* body) {
	for (int i = 0; i < slot; i++) {
		key(script, n, QDOS_KEY_DOWN);
	}
	key(script, n, QDOS_KEY_SOFT2); // EDIT
	type_more(script, n, body);
	for (int i = 0; i < slot; i++) {
		key(script, n, QDOS_KEY_UP);
	}
}

/** PLOT, beside APPS, is the Y= page: six slots of the user's own. */
static void test_plot_is_the_y_editor(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "PLOT") != NULL && strstr(row, "PLOT") < strstr(row, "APPS"));
	CHECK(strstr(row, "PLOTAPPS") == NULL);

	key(script, &n, QDOS_KEY_SOFT1);
	run_script(script, n, fb);

	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strcmp(row, "Y=") == 0);
	for (int i = 0; i < 6; i++) {
		char want[4];
		snprintf(want, sizeof(want), "Y%d", i + 1);
		read_row(fb, ROW_CONTENT_FIRST_T + i, row, sizeof(row));
		CHECK(strncmp(row, want, 2) == 0);
		CHECK(strstr(row, "2D") == NULL); // empty, so nothing to plot as
	}
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "EDIT") && strstr(row, "ON") && strstr(row, "GRAPH"));
}

/** A slot is typed like a line, switched on, kept across a restart, and a word of its own. */
static void test_a_slot_is_typed_and_kept(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT1);
	type_slot(script, &n, 0, "x sin x *");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1], body[EDIT_COLS_T + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strncmp(row, "Y1=", 3) == 0); // on, as a new one is
	CHECK(strstr(row, "2D") != NULL);
	read_slot_body(fb, 0, body, sizeof(body));
	CHECK(strcmp(body, "x sin x *") == 0);

	// Back after a restart, with nothing typed this time
	n = 0;
	key(script, &n, QDOS_KEY_SOFT1);
	run_script(script, n, fb);
	read_slot_body(fb, 0, body, sizeof(body));
	CHECK(strcmp(body, "x sin x *") == 0);
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strncmp(row, "Y1=", 3) == 0);

	// And Y1 is a word like any other
	n = 0;
	type_line(script, &n, "2 Y1");
	run_script(script, n, fb);
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1.8185") != NULL); // 2 sin 2 *
}

/** GRAPH draws every slot switched on together; trace goes from one to the next. */
static void test_graph_draws_every_slot_that_is_on(void) {
	store_reset();

	qdos_key_event script[160];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT1);
	type_slot(script, &n, 0, "x sin");
	type_slot(script, &n, 1, "x cos");
	key(script, &n, QDOS_KEY_SOFT4); // GRAPH

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(plot_has_ink(fb));
	char line[EDIT_COLS_T + 1];
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "Y1 Y2  X -10:10") == line);

	key(script, &n, QDOS_KEY_TRACE);
	store_reset();
	run_script(script, n, fb);
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "Y1 X=0.025  Y=0.024997") == line);

	key(script, &n, QDOS_KEY_DOWN);
	store_reset();
	run_script(script, n, fb);
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "Y2 X=0.025  Y=0.99968") == line);

	// ESC goes back to Y=, where Y2 is switched off and GRAPH draws Y1 alone
	key(script, &n, QDOS_KEY_CLEAR);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_SOFT3); // ON, off
	store_reset();
	run_script(script, n, fb);
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strcmp(row, "Y=") == 0);
	read_row(fb, ROW_CONTENT_FIRST_T + 1, row, sizeof(row));
	CHECK(strncmp(row, "Y2 ", 3) == 0);

	key(script, &n, QDOS_KEY_UP);
	key(script, &n, QDOS_KEY_SOFT4);
	store_reset();
	run_script(script, n, fb);
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "Y1  X -10:10") == line);
}

/** A body using y is a surface, and GRAPH on it draws it in 3D. */
static void test_a_slot_using_y_is_a_surface(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT1);
	type_slot(script, &n, 0, "x x * y y * +");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "3D") != NULL);

	key(script, &n, QDOS_KEY_SOFT4);
	store_reset();
	run_script(script, n, fb);
	char line[EDIT_COLS_T + 1];
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strstr(line, "Y1  AZ 30 EL 25") == line);
}

/** A body that will not declare says why, is marked, and is not plotted. */
static void test_a_broken_slot(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT1);
	type_slot(script, &n, 0, "x sin }");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[EDIT_COLS_T + 1];
	read_error(fb, row, sizeof(row));
	CHECK(row[0] != '\0' && error_cell_is(fb, 0, row[0], true));

	key(script, &n, QDOS_KEY_SOFT4);
	store_reset();
	run_script(script, n, fb);
	char big[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, big, sizeof(big));
	CHECK(strstr(big, "ERR") != NULL);
	read_row(fb, ROW_MESSAGE_LINE, big, sizeof(big));
	CHECK(strstr(big, "NO CURVE IS ON") != NULL);
}

/** DEL empties a slot, and it is empty after a restart too. */
static void test_del_empties_a_slot(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT1);
	type_slot(script, &n, 0, "x");
	key(script, &n, QDOS_KEY_BACKSPACE);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	n = 0;
	key(script, &n, QDOS_KEY_SOFT1);
	run_script(script, n, fb);
	char body[EDIT_COLS_T + 1], row[QDOS_COLS + 1];
	read_slot_body(fb, 0, body, sizeof(body));
	CHECK(body[0] == '\0');
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strcmp(row, "Y1") == 0); // off, and nothing after it
}

/** x and y are soft keys while a slot is typed, each typed as a word of its own. */
static void test_x_and_y_are_keys_in_a_slot(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT1);
	key(script, &n, QDOS_KEY_SOFT2); // EDIT
	key(script, &n, QDOS_KEY_SOFT2); // x
	key(script, &n, QDOS_KEY_SOFT2); // x
	key(script, &n, QDOS_KEY_MUL);
	key(script, &n, QDOS_KEY_SOFT4); // y
	key(script, &n, QDOS_KEY_ADD);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// Labelled while typing, where the soft row had nothing to offer
	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, " x ") != NULL && strstr(row, " y ") != NULL);

	key(script, &n, QDOS_KEY_ENTER);
	store_reset();
	run_script(script, n, fb);
	char body[EDIT_COLS_T + 1];
	read_slot_body(fb, 0, body, sizeof(body));
	CHECK(strcmp(body, "x x * y +") == 0); // spaced once, and trimmed
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "3D") != NULL);
}

/** Traced, a square root is a square root: y in full, not in whole steps. */
static void test_a_traced_slot_is_not_rounded(void) {
	store_reset();

	qdos_key_event script[160];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT1);
	type_slot(script, &n, 0, "x sqrt");
	key(script, &n, QDOS_KEY_SOFT4);
	key(script, &n, QDOS_KEY_TRACE);
	for (int i = 0; i < 37; i++) {
		key(script, &n, QDOS_KEY_RIGHT);
	}

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char line[EDIT_COLS_T + 1];
	read_small(fb, READOUT_Y0_T, line, sizeof(line));
	CHECK(strcmp(line, "X=1.875  Y=1.3693064") == 0);
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
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "IS NOT DECLARED") != NULL);
}

/**
 * A word written on the line is memory only: it is usable at once and gone at
 * the next boot. The card holds programs, which are put there by `edit` or by
 * uploading them, and scratch work at the prompt is not that.
 */
static void test_a_declared_word_is_not_written_to_the_card(void) {
	store_reset();

	qdos_key_event first[160];
	size_t n = 0;
	type_line(first, &n, "fn answer( -- r:i64) { 42 }");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(first, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "SAVED") == NULL);
	CHECK(strstr(row, "DECLARED") != NULL);

	// Nothing on the card, and so nothing in APPS
	CHECK(!stored("answer.qd"));

	// A fresh shell, as after a reboot: the word went with the power
	qdos_key_event second[64];
	n = 0;
	type_line(second, &n, "answer");
	run_script(second, n, fb);

	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "not defined") != NULL);
}

/** Forgetting a word also drops it from the store. */
static void test_forget_outlives_the_reboot(void) {
	store_reset();

	qdos_key_event first[160];
	size_t n = 0;
	type_line(first, &n, "fn sqr(x:i64 -- r:i64) { x x * }");
	type_line(first, &n, "\"sqr\" forget");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(first, n, fb);

	qdos_key_event second[64];
	n = 0;
	type_line(second, &n, "clear 7 sqr");
	run_script(second, n, fb);

	char row[QDOS_COLS + 1];
	read_error(fb, row, sizeof(row));
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
	type_line(script, &n, "fn sq(x:i64 -- r:i64) { x dup * }");
	type_more(script, &n, "7 sq");
	type_more(script, &n, "1 0 /"); // an error, so the inverted path is drawn too

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	int ambiguous = 0;
	for (size_t i = 0; i < sizeof(fb); i++) {
		if (fb[i] >= 64 && fb[i] < 192) {
			ambiguous++;
		}
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
	seed_scope(QDOS_SCOPE_USER, "mine", "fn mine( -- r:i64) { 1 }");

	qdos_key_event script[160];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "APPS") != NULL);
	CHECK(strstr(row, "/2") != NULL);

	// Yours first, whatever the alphabet says
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "mine") != NULL);
	CHECK(strstr(row, "USER") != NULL);

	read_row(fb, ROW_CONTENT_FIRST_T + 1, row, sizeof(row));
	CHECK(strstr(row, "hyp") != NULL);
	CHECK(strstr(row, "SYS") != NULL);
}

/**
 * A folder on the card is an app, listed under the folder's name.
 *
 * What is inside it belongs to it: main.qd is the entry point and is not a
 * program of its own, so the row says `doom` and there is one of it.
 */
static void test_an_app_folder_is_listed_under_its_own_name(void) {
	store_reset();
	seed_app(QDOS_SCOPE_INBOX, "doom", "fn main( -- ) { }");

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "/1") != NULL);

	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "doom") != NULL);
	CHECK(strstr(row, "main") == NULL);
	CHECK(strstr(row, "CARD") != NULL);
}

/** Typing an app's name runs its main. */
static void test_an_app_runs_its_entry_point(void) {
	store_reset();
	seed_app(QDOS_SCOPE_INBOX, "greet", "fn main( -- ) { \"HI FROM MAIN\" print }");

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "greet");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "HI FROM MAIN") != NULL);
}

/**
 * Every app calls its entry point `main`, so two of them have to be able to
 * without meeting. Each runs in an interpreter of its own.
 */
static void test_two_apps_may_both_have_a_main(void) {
	store_reset();
	seed_app(QDOS_SCOPE_INBOX, "one", "fn main( -- ) { \"I AM ONE\" print }");
	seed_app(QDOS_SCOPE_INBOX, "two", "fn main( -- ) { \"I AM TWO\" print }");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	char row[QDOS_COLS + 1];

	qdos_key_event first[64];
	size_t n = 0;
	type_line(first, &n, "one");
	run_script(first, n, fb);
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "I AM ONE") != NULL);

	qdos_key_event second[64];
	n = 0;
	type_line(second, &n, "two");
	run_script(second, n, fb);
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "I AM TWO") != NULL);
}

/** An app's helpers are its own: `main` is not a word at the prompt. */
static void test_an_app_leaves_nothing_behind(void) {
	store_reset();
	seed_app(QDOS_SCOPE_INBOX, "quiet", "fn helper( -- ) { }\nfn main( -- ) { helper }");

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "quiet");
	type_line(script, &n, "helper");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "not defined") != NULL);
}

/** Editing an app opens its main.qd, and saving writes back into the folder. */
static void test_editing_an_app_writes_into_its_folder(void) {
	store_reset();
	seed_app(QDOS_SCOPE_INBOX, "tweak", "fn main( -- ) { }");

	qdos_key_event script[256];
	size_t n = 0;
	type_line(script, &n, "\"tweak\" edit");
	key(script, &n, QDOS_KEY_SAVE);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// Into the folder, not beside it
	CHECK(stored("tweak/" QDOS_APP_MAIN));
	CHECK(!stored("tweak.qd"));
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
	read_error(fb, row, sizeof(row));
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

	// The body is laid out over the stack area, a line per row, small
	char line[EDIT_COLS_T + 1];
	read_edit_line(fb, 0, line, sizeof(line));
	CHECK(strstr(line, "fn hyp") != NULL);
	read_edit_line(fb, 1, line, sizeof(line));
	CHECK(strstr(line, "5") != NULL);
}

/** The small font is the point: eight lines of fifty columns, not six of 24. */
static void test_edit_shows_eight_lines_of_fifty(void) {
	store_reset();
	seed_system("wide", "fn wide( -- ) {\n"
						"\t1\n\t2\n\t3\n\t4\n\t5\n"
						"\"0123456789012345678901234567890123456789\" print\n"
						"}");

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "\"wide\" edit");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(EDIT_ROWS_T == 8 && EDIT_COLS_T == 50);

	char line[EDIT_COLS_T + 1];
	read_edit_line(fb, 6, line, sizeof(line));
	CHECK(strcmp(line, "\"0123456789012345678901234567890123456789\" print") == 0);
	read_edit_line(fb, 7, line, sizeof(line));
	CHECK(strcmp(line, "}") == 0);
}

/** An unknown name starts a new program rather than failing. */
static void test_edit_starts_new(void) {
	store_reset();

	qdos_key_event script[160];
	size_t n = 0;
	type_line(script, &n, "\"fresh\" edit");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char line[EDIT_COLS_T + 1];
	read_edit_line(fb, 0, line, sizeof(line));
	CHECK(strstr(line, "fn fresh") != NULL);
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
	read_error(fb, row, sizeof(row));
	CHECK(row[0] != '\0');
	CHECK(strstr(row, "COMPILES") == NULL);
	CHECK(error_cell_is(fb, 0, row[0], true)); // inverted, so flagged

	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "bad") != NULL);
}

/**
 * A body that parses can still name something that is not a word. The
 * interpreter would only notice when the word is called, so the check says it
 * here instead.
 */
static void test_edit_check_finds_an_undefined_word(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"boom\" edit");
	type_partial(script, &n, "ok");
	key(script, &n, QDOS_KEY_CHECK);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "'ok' NOT DEFINED") != NULL);
	CHECK(strstr(row, "COMPILES") == NULL);
	CHECK(error_cell_is(fb, 0, row[0], true)); // inverted, so flagged

	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "boom") != NULL);
}

/** A body that only calls words that exist still passes. */
static void test_edit_check_accepts_a_known_word(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"fine\" edit");
	type_partial(script, &n, "1 dup +");
	key(script, &n, QDOS_KEY_CHECK);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "COMPILES") != NULL);
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
	type_line(script, &n, "fn sq(x:i64 -- r:i64) { x dup * 1000 + }");
	type_line(script, &n, "7 sq");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1049") != NULL);

	n = 0;
	type_line(script, &n, "fn sq(x:i64 -- r:i64) { x dup * 1000 + }");
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

/** Rotate reaches the third entry. Off the keypad now, but still a key. */
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
	for (int i = 0; i < 3; i++) {
		key(script, &n, QDOS_KEY_ROT);
	}

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

/** e^x and the inverse trigonometry have keys, and apply to x like the rest */
static void test_exp_and_asin_in_calculator_mode(void) {
	store_reset();
	qdos_math_set_degrees(false);

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "1");
	key(script, &n, QDOS_KEY_EXP);
	digits(script, &n, "1");
	key(script, &n, QDOS_KEY_ASIN);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1.5707") != NULL);
	read_row(fb, ROW_TOP_VALUE - 1, row, sizeof(row));
	CHECK(strstr(row, "2.7182") != NULL);
}

/**
 * A type error in the runtime used to end the process. If this regresses the
 * test binary dies rather than failing, which is the point.
 */
static void test_fatal_runtime_errors_are_survivable(void) {
	static const char* const DEADLY[] = {
			"1.5 2.5 and",
			"1 2.5 mod",
			"1.5 shl",
			"\"s\" sqrt",
			"\"s\" sin",
			"0.0 0.0 fac",
			"1 0 /",
			"5 ln ln ln ln",
	};

	for (size_t i = 0; i < sizeof(DEADLY) / sizeof(*DEADLY); i++) {
		store_reset();

		qdos_key_event script[64];
		size_t n = 0;
		type_line(script, &n, DEADLY[i]);

		// The line that failed is still there to be corrected, so it is thrown
		// away and the mode left before the next one is typed
		key(script, &n, QDOS_KEY_CLEAR);
		key(script, &n, QDOS_KEY_CLEAR);
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
	type_line(script, &n, "\"hyp\" forget");		  // and put it back
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
		if (strstr(row, "FIRST") != NULL) {
			printed = true;
		}
		if (strstr(row, "ZERO") != NULL || strstr(row, "zero") != NULL) {
			failed = true;
		}
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
	read_error(errored, row, sizeof(row));
	CHECK(strstr(row, "ZERO DIVISOR") != NULL);
	CHECK(row[0] != ':');
	CHECK(error_cell_is(errored, 0, row[0], true));

	// One key later the line is the user's again, with that key on it
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "ZERO DIVISOR") == NULL);
	CHECK(row[0] == ':');
	CHECK(strchr(row, '7') != NULL);
}

/** The stack fills every row between the status band and the input line. */
static void test_the_stack_reaches_the_reclaimed_row(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "1 2 3 4 5 6 7");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// All seven are on screen, the deepest right under the band
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_STATUS_T + 1, row, sizeof(row));
	CHECK(strstr(row, "7:") != NULL);
	CHECK(strstr(row, "...") == NULL);

	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1:") != NULL);
}

/** Deeper than that says how many are hidden, on a row of its own. */
static void test_a_deeper_stack_still_marks_itself(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "1 2 3 4 5 6 7 8 9 10");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// Ten deep, six rows of values under the marker, so four are not on screen
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_STATUS_T + 1, row, sizeof(row));
	CHECK(strstr(row, "4 MORE") != NULL);

	// The row under it is a numbered value, not something the marker ate
	read_row(fb, ROW_STATUS_T + 2, row, sizeof(row));
	CHECK(strstr(row, "6:") != NULL);
	CHECK(strstr(row, "5") != NULL);

	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1:") != NULL);
	CHECK(strstr(row, "10") != NULL);
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
	seed_inbox("triple", "fn triple(x:i64 -- r:i64) { x 3 * }");

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
	seed_inbox("triple", "fn triple(x:i64 -- r:i64) { x 3 * }");

	// What saving an edit leaves behind: a copy of your own over the upload
	seed_scope(QDOS_SCOPE_USER, "triple", "fn triple(x:i64 -- r:i64) { x 30 * }");

	qdos_key_event script[256];
	size_t n = 0;
	type_line(script, &n, "2 triple");

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
	seed_scope(QDOS_SCOPE_USER, "both", "fn both( -- r:i64) { 2 }");

	qdos_key_event script[256];
	size_t n = 0;
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
	read_error(fb, row, sizeof(row));
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
	seed_inbox("triple", "fn triple(x:i64 -- r:i64) { x 3 * }");

	qdos_key_event script[256];
	size_t n = 0;
	type_line(script, &n, "fn triple(x:i64 -- r:i64) { x 30 * }");
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
	read_error(fb, row, sizeof(row));
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
	for (int i = 0; i < 40; i++) {
		key(script, &n, QDOS_KEY_UP);
	}
	for (int i = 0; i < 40; i++) {
		key(script, &n, QDOS_KEY_DOWN);
	}

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
	for (int i = 0; i < 3; i++) {
		key(script, &n, QDOS_KEY_ENTER); // AUTO -> 0 -> 1 -> 2
	}
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
	key(script, &n, QDOS_KEY_UP); // already at the top
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
	for (int i = 0; i < 3; i++) {
		key(script, &n, QDOS_KEY_ENTER); // AUTO -> 0 -> 1 -> 2
	}
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
	for (int i = 0; i < 3; i++) {
		key(script, &n, QDOS_KEY_ENTER);
	}
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
	seed_scope(QDOS_SCOPE_USER, "mine", "fn mine( -- ) { 1 }");

	qdos_key_event script[160];
	size_t n = 0;
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

/** A line that would not evaluate stays on the input for correction. */
static void test_a_failed_line_is_kept(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "1 2 zzz");
	key(script, &n, QDOS_KEY_RIGHT); // any key takes the line back off the message

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t mid[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, n - 1, fb, mid);

	char row[QDOS_COLS + 1];
	read_error(mid, row, sizeof(row));
	CHECK(strstr(row, "not defined") != NULL);

	// The next key shows the line again, still holding what was typed
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(row[0] == ':');
	CHECK(strstr(row, "1 2 zzz") != NULL);
}

/** And the way to be rid of it is the key that says CLR while it is there. */
static void test_clear_empties_the_line_before_leaving(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	type_partial(script, &n, "1 2 zzz");
	key(script, &n, QDOS_KEY_CLEAR);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	static uint8_t mid[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_mid(script, n, n, fb, mid);

	// Empty, and still in line mode
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(row[0] == ':');
	CHECK(strstr(row, "zzz") == NULL);

	// The label says which of its two jobs is next
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "ESC") != NULL);
}

/** Infix is valid Quadrate that means something else, so it is refused. */
static void test_infix_is_refused_with_the_postfix(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "5 - 3");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "5 3 -") != NULL);

	// Nothing was evaluated, so nothing reached the stack
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1:") == NULL);
}

/** Unspaced too, which is two numbers rather than one wrong answer. */
static void test_unspaced_infix_is_refused(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "5-3");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "5 3 -") != NULL);
}

/** Postfix, and a negative number, go through untouched. */
static void test_the_hint_leaves_real_lines_alone(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "5 3 -");
	type_line(script, &n, "-4");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "-4") != NULL);

	read_row(fb, ROW_TOP_VALUE - 1, row, sizeof(row));
	CHECK(strstr(row, "2") != NULL);
}

/** Emptying the stack is the largest thing CLR does, so it is undoable. */
static void test_clearing_the_stack_can_be_undone(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "1");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "2");
	key(script, &n, QDOS_KEY_ENTER);
	digits(script, &n, "3");
	key(script, &n, QDOS_KEY_ENTER);
	key(script, &n, QDOS_KEY_CLEAR); // nothing part-typed, so this is the stack
	key(script, &n, QDOS_KEY_UNDO);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1:") != NULL);
	CHECK(strstr(row, "3") != NULL);

	read_row(fb, ROW_TOP_VALUE - 2, row, sizeof(row));
	CHECK(strstr(row, "3:") != NULL);
	CHECK(strstr(row, "1") != NULL);
}

/** A number too wide for its row goes to exponent form rather than being cut. */
static void test_a_wide_number_keeps_its_magnitude(void) {
	store_reset();
	seed_setting("settings.decimals", 9); // nine places of 1e24 fits nowhere

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "1000000.0 dup * dup *");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "e+") != NULL);
	CHECK(strstr(row, "1:") != NULL); // and it did not draw over the label
}

/** A string that will not fit keeps its head, with the cut marked. */
static void test_a_wide_string_is_marked_where_it_was_cut(void) {
	store_reset();

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "\"abcdefghijklmnopqrstuvwxyz\"");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "abcde") != NULL);
	CHECK(strchr(row, QDOS_ELIDED) != NULL);
	CHECK(row[0] == '1' && row[1] == ':');
}

/** STO and RCL take the digit after them, as the key marked STO always has. */
static void test_sto_and_rcl_from_the_calculator(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "42");
	key(script, &n, QDOS_KEY_STO);
	key(script, &n, QDOS_KEY_7);
	key(script, &n, QDOS_KEY_CLEAR); // the stack, since the entry is spent
	key(script, &n, QDOS_KEY_RCL);
	key(script, &n, QDOS_KEY_7);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL);
}

/** The press asks which register, so the digit is not taken for a number. */
static void test_sto_asks_which_register(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "42");
	key(script, &n, QDOS_KEY_STO);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "STO") != NULL);
	CHECK(strstr(row, "0-9") != NULL);
}

/** Anything but a digit takes it back, rather than storing somewhere random. */
static void test_a_register_press_can_be_cancelled(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "42");
	key(script, &n, QDOS_KEY_STO);
	key(script, &n, QDOS_KEY_DUP); // not a digit

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "CANCELLED") != NULL);

	// 42 went on the stack when STO was pressed, and dup did not run
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL);
	read_row(fb, ROW_TOP_VALUE - 1, row, sizeof(row));
	CHECK(row[0] == '\0');
}

/** Leaving the calculator gives up waiting, rather than eating a later digit. */
static void test_a_register_press_does_not_outlive_the_calculator(void) {
	store_reset();

	qdos_key_event script[64];
	size_t n = 0;
	digits(script, &n, "42");
	key(script, &n, QDOS_KEY_STO);
	key(script, &n, QDOS_KEY_LIST); // off to a page instead of answering
	key(script, &n, QDOS_KEY_CLEAR);
	digits(script, &n, "7"); // a number, not a register

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL);
	CHECK(strstr(row, "STORED") == NULL);
}

/** The angle soft key is the setting, and pressing it turns it over. */
static void test_the_angle_soft_key_shows_and_toggles(void) {
	store_reset();
	// The angle lives in mathwords, which outlives one shell, so say where to
	// start rather than inheriting it from whichever test ran last
	seed_setting("settings.angle", 0);

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT5);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "DEG") != NULL); // radians to start with, so now degrees

	// And it is the setting itself, not a mode of its own
	store_reset();
	seed_setting("settings.angle", 0);
	n = 0;
	key(script, &n, QDOS_KEY_SOFT5);
	key(script, &n, QDOS_KEY_SETTINGS);
	run_script(script, n, fb);

	read_row(fb, ROW_SETTING_ANGLE, row, sizeof(row));
	CHECK(strstr(row, "DEG") != NULL);
}

/** Which face the keypad is on shows beside the cursor, since a cap cannot. */
static void test_the_prompt_shows_the_keypad_layer(void) {
	store_reset();
	g_has_modifier = true;
	g_modifier = QDOS_MOD_ALPHA;

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(NULL, 0, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(row[0] == '>' && row[1] == 'A');

	// With a space of its own, or it reads as the first letter of the line
	qdos_key_event script[8];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	type_partial(script, &n, "sq");
	run_script(script, n, fb);

	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strncmp(row, ":A sq", 5) == 0);

	// A keypad with one face says nothing and takes no room
	store_reset();
	run_script(NULL, 0, fb);
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(row[0] == '>' && row[1] != 'A');
}

/* ---- uploaded native code ------------------------------------------------ */

/**
 * The whole path: a shared object on the card, and a stored program calling a
 * word out of it.
 */
static void test_a_program_calls_an_uploaded_module(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libdemo.so", "");
	seed_system("twice", "fn twice(n:i64 -- r:i64) { n demo::double }");

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "21 twice");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "42") != NULL);
}

/** And from the line, scoped by the file it came from. */
static void test_a_module_word_is_callable_directly(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libdemo.so", "");

	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "3.0 4.0 demo::hypot");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "5") != NULL);
}

/** The lint compiles against a scratch interpreter, which needs them too. */
static void test_check_knows_about_module_words(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libdemo.so", "");
	seed_system("uses", "fn uses(n:i64 -- r:i64) { demo::double }");

	qdos_key_event script[128];
	size_t n = 0;
	type_line(script, &n, "\"uses\" edit");
	key(script, &n, QDOS_KEY_CHECK);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "COMPILES") != NULL);
	CHECK(strstr(row, "not defined") == NULL);
}

/** A module that was being opened when the machine stopped is left alone. */
static void test_a_module_that_faulted_is_not_opened_again(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libdemo.so", "");

	// What the last boot left behind, having died part-way through opening it
	seed_raw(QDOS_SCOPE_USER, "native.loading", "demo");

	// The screen the machine comes up with, before any key is pressed
	static uint8_t boot[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(NULL, 0, boot);

	char row[QDOS_COLS + 1];
	read_error(boot, row, sizeof(row));
	CHECK(strstr(row, "FAULTED") != NULL);
	CHECK(strstr(row, "demo") != NULL);

	// It stays blocked at the next start too, so the word is not there -- which
	// is the machine being usable rather than the machine being absent
	qdos_key_event script[64];
	size_t n = 0;
	type_line(script, &n, "21 demo::double");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "demo::double") != NULL);
	CHECK(strstr(row, "not def") != NULL);
}

/** Once blocked it stays blocked, which is what the settings row is for. */
static void test_a_blocked_module_can_be_let_back_in(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libdemo.so", "");
	seed_raw(QDOS_SCOPE_USER, "native.loading", "demo");

	qdos_key_event script[64];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN); // past angle, decimals, auto off

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// The row is only there because something is blocked
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T + 3, row, sizeof(row));
	CHECK(strstr(row, "MODULES") != NULL);
	CHECK(strstr(row, "1 BLOCKED") != NULL);

	// Changing it clears the list, and the row goes with it
	n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_ENTER);

	run_script(script, n, fb);
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "NEXT START") != NULL);
	CHECK(!page_has(fb, "MODULES"));
}

/** A machine with no modules at all is unchanged by any of this. */
static void test_no_modules_means_no_settings_row(void) {
	store_reset();

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	CHECK(!page_has(fb, "MODULES"));
	CHECK(page_has(fb, "ANGLE"));
}

/** Installed code is listed with the programs, marked as the scope it is. */
static void test_the_list_shows_modules(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libdemo.so", "");
	seed_system("twice", "fn twice(n:i64 -- r:i64) { n demo::double }");

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// Modules first, being what the programs under them are liable to call
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "demo::") != NULL);
	CHECK(strstr(row, "CARD") != NULL);

	read_row(fb, ROW_CONTENT_FIRST_T + 1, row, sizeof(row));
	CHECK(strstr(row, "twice") != NULL);

	// Both of them counted
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "1/2") != NULL);
}

/** A module that is not loaded says why, that being what there is to know. */
static void test_the_list_says_why_a_module_is_missing(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libbadarch.so", "");

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "badarch::") != NULL);
	CHECK(strstr(row, "BUILT FOR") != NULL);
}

/** Picking a module types the way into it rather than a word. */
static void test_picking_a_module_types_its_scope(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libdemo.so", "");

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "demo::") != NULL);

	// And the soft row does not offer to edit something that is not text
	read_row(fb, QDOS_ROWS - 1, row, sizeof(row));
	CHECK(strstr(row, "EDIT") == NULL);
	CHECK(strstr(row, "PICK") == NULL); // line mode by now, so the row changed
}

/** From there Tab finishes the word, the colons being part of the name. */
static void test_tab_completes_a_scoped_word(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libdemo.so", "");

	qdos_key_event script[32];
	size_t n = 0;
	script[n++] = (qdos_key_event){QDOS_KEY_CHAR, ':'};
	type_partial(script, &n, "demo::hy");
	key(script, &n, QDOS_KEY_TAB);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "demo::hypot") != NULL);
}

/** A program is listed as what you type to run it, with no colons. */
static void test_the_list_shows_a_program_without_colons(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libapp.so", "");
	seed_raw(QDOS_SCOPE_INBOX, "libdemo.so", "");

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_CONTENT_FIRST_T, row, sizeof(row));
	CHECK(strstr(row, "app") != NULL);
	CHECK(strstr(row, "app::") == NULL);

	read_row(fb, ROW_CONTENT_FIRST_T + 1, row, sizeof(row));
	CHECK(strstr(row, "demo::") != NULL);
}

/** And picking it types the name, the way picking a program does. */
static void test_picking_a_program_types_its_name(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libapp.so", "");

	qdos_key_event script[8];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);
	key(script, &n, QDOS_KEY_ENTER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(strstr(row, "app") != NULL);
	CHECK(strstr(row, "::") == NULL);
}

/** Running one takes the screen, and gives it back on the way out. */
static void test_a_program_owns_the_screen_while_it_runs(void) {
	store_reset();
	seed_raw(QDOS_SCOPE_INBOX, "libapp.so", "");

	qdos_key_event script[32];
	size_t n = 0;
	type_line(script, &n, "app");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// The shell has painted over it by now, which is the screen coming back:
	// the prompt is there and the program's corner is not
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_INPUT_LINE, row, sizeof(row));
	CHECK(row[0] == ':');
	CHECK(fb[0] != 0x00 || fb[7 * QDOS_SCREEN_W + 7] != 0x00);
}

/* ---- the card changing underneath ---------------------------------------- */

/** A program dropped on the card arrives without a restart. */
static void test_a_program_arriving_on_the_card_is_picked_up(void) {
	store_reset();
	g_card_arrival = true;
	g_card_name = "landed";
	g_card_source = "fn landed( -- r:i64) { 7 }";

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_idling(NULL, 0, 0, 3, fb, NULL);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "1 FROM THE CARD") != NULL);
}

/** And it is a word by then, not just a name on a list. */
static void test_a_program_arriving_is_callable(void) {
	store_reset();
	g_card_arrival = true;
	g_card_name = "landed";
	g_card_source = "fn landed( -- r:i64) { 7 }";

	qdos_key_event script[32];
	size_t n = 0;
	type_line(script, &n, "landed");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_idling(script, n, 0, 3, fb, NULL);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL);
}

/** A module cannot arrive the same way, and says so rather than being ignored. */
static void test_a_module_arriving_asks_for_a_restart(void) {
	store_reset();
	g_card_arrival = true;
	g_card_module = "libarrived.so"; // lands after the machine is already up

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_idling(NULL, 0, 0, 3, fb, NULL);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "RESTART") != NULL);
	CHECK(strstr(row, "arrived") != NULL);
}

/** A list already on screen takes the new program without being reopened. */
static void test_an_open_list_takes_what_arrives(void) {
	store_reset();
	seed_system("already", "fn already( -- r:i64) { 1 }");

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_LIST);

	g_card_arrival = true;
	g_card_name = "landed";
	g_card_source = "fn landed( -- r:i64) { 7 }";

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_idling(script, n, 0, 3, fb, NULL);

	// Still the list, and the arrival is on it
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "APPS") != NULL);
	CHECK(page_has(fb, "landed"));
	CHECK(page_has(fb, "already"));
}

/** While a host holds the card, those blocks are not ours to read. */
static void test_the_card_is_not_read_while_it_is_shared(void) {
	store_reset();
	g_usb_supported = true;

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SETTINGS);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_DOWN);	 // onto USB
	key(script, &n, QDOS_KEY_ENTER); // share it

	// And only then does something land
	g_card_arrival = true;
	g_card_name = "late";
	g_card_source = "fn late( -- r:i64) { 1 }";

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script_idling(script, n, 0, 3, fb, NULL);

	// Still the sharing message: the arrival was not acted on
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "FROM THE CARD") == NULL);
}

/** A program can ask the machine about itself, which is how one takes over. */
static void test_a_program_can_ask_the_machine(void) {
	store_reset();
	seed_system("alive", "fn alive( -- r:i64) { qdos::running }");

	qdos_key_event script[32];
	size_t n = 0;
	type_line(script, &n, "alive");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1") != NULL);
}

/** And take a keypress, there being none waiting in a scripted run. */
static void test_a_program_can_take_a_key(void) {
	store_reset();
	seed_system("gotkey", "fn gotkey( -- r:i64) { qdos::key drop drop }");

	qdos_key_event script[32];
	size_t n = 0;
	type_line(script, &n, "gotkey");

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "1:") != NULL);
}

/** PWR stops a program that only ever asks for keys, and not the machine. */
static void test_power_breaks_a_program(void) {
	store_reset();
	seed_system("spin", "fn spin( -- ) { loop { qdos::key drop drop drop } }");

	qdos_key_event script[32];
	size_t n = 0;
	type_line(script, &n, "spin");
	key(script, &n, QDOS_KEY_POWER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "BREAK") != NULL);

	// Still on: the next line is evaluated, and the break does not linger
	key(script, &n, QDOS_KEY_CLEAR);
	digits(script, &n, "7");
	key(script, &n, QDOS_KEY_ENTER);
	run_script(script, n, fb);

	read_row(fb, ROW_TOP_VALUE, row, sizeof(row));
	CHECK(strstr(row, "7") != NULL);
}

/** An app is stopped the same way, and what it printed does not hide that. */
static void test_power_breaks_an_app(void) {
	store_reset();
	seed_app(QDOS_SCOPE_INBOX, "game", "fn main( -- ) { \"BANNER\" print loop { qdos::key drop drop drop } }");

	qdos_key_event script[32];
	size_t n = 0;
	type_line(script, &n, "game");
	key(script, &n, QDOS_KEY_POWER);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_error(fb, row, sizeof(row));
	CHECK(strstr(row, "BREAK") != NULL);
}

/** Leaving without writing, once it has asked. */
static void test_edit_discards(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"gone\" edit");
	type_partial(script, &n, "x"); // something to lose
	key(script, &n, QDOS_KEY_CLEAR);
	key(script, &n, QDOS_KEY_CLEAR);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "NOT SAVED") != NULL);
}

/** The first press asks, because the editor holds the only copy. */
static void test_edit_asks_before_losing_work(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"typed\" edit");
	type_partial(script, &n, "1 2 +");
	key(script, &n, QDOS_KEY_CLEAR);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	// Still in the editor, with the text and the question both on screen
	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "ESC AGAIN") != NULL);

	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "typed") != NULL);
	CHECK(edit_pane_has(fb, "1 2 +"));
}

/** Nothing typed is nothing to lose, so it just leaves. */
static void test_an_untouched_editor_leaves_at_once(void) {
	store_reset();

	qdos_key_event script[200];
	size_t n = 0;
	type_line(script, &n, "\"fresh\" edit");
	key(script, &n, QDOS_KEY_CLEAR);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_MESSAGE_LINE, row, sizeof(row));
	CHECK(strstr(row, "ESC AGAIN") == NULL);

	// Back at the calculator: the prompt is on the input line
	CHECK(row[0] == '>');
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
	CHECK(strstr(row, "PICK") != NULL);
	CHECK(strstr(row, "EDIT") != NULL);

	// and no arrows among them, the keypad having its own
	CHECK(strstr(row, QDOS_GLYPH_DOWN) == NULL);
	CHECK(strstr(row, QDOS_GLYPH_UP) == NULL);
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

/** The list moves on the arrow keys, and f2 and f3 no longer stand in for them. */
static void test_list_moves_on_the_arrow_keys(void) {
	store_reset();
	seed_system("aaa", "fn aaa( -- r:i64) { 1 }");
	seed_system("bbb", "fn bbb( -- r:i64) { 2 }");

	qdos_key_event script[16];
	size_t n = 0;
	key(script, &n, QDOS_KEY_SOFT2); // apps
	key(script, &n, QDOS_KEY_DOWN);

	static uint8_t fb[QDOS_SCREEN_W * QDOS_SCREEN_H];
	run_script(script, n, fb);

	char row[QDOS_COLS + 1];
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "2/2") != NULL);

	n = 0;
	key(script, &n, QDOS_KEY_SOFT2); // apps
	key(script, &n, QDOS_KEY_DOWN);
	key(script, &n, QDOS_KEY_UP);
	run_script(script, n, fb);
	read_row(fb, ROW_HEADER_T, row, sizeof(row));
	CHECK(strstr(row, "1/2") != NULL);

	n = 0;
	key(script, &n, QDOS_KEY_SOFT2); // apps
	key(script, &n, QDOS_KEY_SOFT2);
	key(script, &n, QDOS_KEY_SOFT3);
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
	test_bare_point_entry();
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
	test_graph_plots_a_word();
	test_graph_leaves_the_stack_alone();
	test_graph_trace_reads_the_curve();
	test_graph_of_an_unknown_word();
	test_graph_of_the_wrong_shape();
	test_graph3_plots_a_surface();
	test_graph3_of_the_wrong_shape();
	test_plot_is_the_y_editor();
	test_a_slot_is_typed_and_kept();
	test_graph_draws_every_slot_that_is_on();
	test_a_slot_using_y_is_a_surface();
	test_a_broken_slot();
	test_del_empties_a_slot();
	test_x_and_y_are_keys_in_a_slot();
	test_a_traced_slot_is_not_rounded();
	test_the_status_band_shows_clock_and_battery();
	test_the_status_band_stays_when_it_has_nothing();
	test_the_clock_turns_over_while_idle();
	test_a_long_error_is_not_cut_at_24();
	test_entry_survives_mode_switch();
	test_native_words_reach_the_hal();
	test_recall_of_empty_register();
	test_clear_a_register();
	test_store_a_string();
	test_control_flow_in_line_mode();
	test_session_survives_power_cycle();
	test_an_array_shows_its_elements();
	test_a_wide_array_shows_its_shape();
	test_a_lost_stack_says_so();
	test_a_declared_word_is_not_written_to_the_card();
	test_forget_outlives_the_reboot();
	test_tab_completes_a_word();
	test_tab_lists_ambiguous();
	test_tab_on_no_match();
	test_pixels_are_unambiguous();
	test_list_browses_words();
	test_list_shows_apps_and_origin();
	test_an_app_folder_is_listed_under_its_own_name();
	test_an_app_runs_its_entry_point();
	test_two_apps_may_both_have_a_main();
	test_an_app_leaves_nothing_behind();
	test_editing_an_app_writes_into_its_folder();
	test_list_toggles_to_all_words();
	test_list_moves_and_picks();
	test_list_exits();
	test_forget_refuses_system();
	test_forget_reverts_to_system();
	test_edit_opens_editor();
	test_edit_shows_eight_lines_of_fifty();
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
	test_exp_and_asin_in_calculator_mode();
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
	test_edit_check_finds_an_undefined_word();
	test_edit_check_accepts_a_known_word();
	test_check_leaves_session_alone();
	test_soft_labels_follow_mode();
	test_soft_key_opens_apps();
	test_soft_open_edits_selection();
	test_list_moves_on_the_arrow_keys();
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
	test_a_failed_line_is_kept();
	test_clear_empties_the_line_before_leaving();
	test_infix_is_refused_with_the_postfix();
	test_unspaced_infix_is_refused();
	test_the_hint_leaves_real_lines_alone();
	test_clearing_the_stack_can_be_undone();
	test_a_wide_number_keeps_its_magnitude();
	test_a_wide_string_is_marked_where_it_was_cut();
	test_sto_and_rcl_from_the_calculator();
	test_sto_asks_which_register();
	test_a_register_press_can_be_cancelled();
	test_a_register_press_does_not_outlive_the_calculator();
	test_the_angle_soft_key_shows_and_toggles();
	test_the_prompt_shows_the_keypad_layer();
	test_edit_asks_before_losing_work();
	test_an_untouched_editor_leaves_at_once();
	test_a_program_calls_an_uploaded_module();
	test_a_module_word_is_callable_directly();
	test_check_knows_about_module_words();
	test_a_module_that_faulted_is_not_opened_again();
	test_a_blocked_module_can_be_let_back_in();
	test_no_modules_means_no_settings_row();
	test_the_list_shows_modules();
	test_the_list_says_why_a_module_is_missing();
	test_picking_a_module_types_its_scope();
	test_tab_completes_a_scoped_word();
	test_the_list_shows_a_program_without_colons();
	test_picking_a_program_types_its_name();
	test_a_program_owns_the_screen_while_it_runs();
	test_a_program_can_ask_the_machine();
	test_a_program_can_take_a_key();
	test_power_breaks_a_program();
	test_power_breaks_an_app();
	test_a_program_arriving_on_the_card_is_picked_up();
	test_a_program_arriving_is_callable();
	test_a_module_arriving_asks_for_a_restart();
	test_the_card_is_not_read_while_it_is_shared();
	test_an_open_list_takes_what_arrives();
	return check_report("shell");
}
