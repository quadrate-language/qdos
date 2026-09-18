/**
 * @file shell.c
 * @brief The calculator's user-facing loop
 */

#include <qdos/shell.h>

#include <quadrate/interp/interp.h>

#include "../ui/console.h"
#include "complete.h"
#include "editor.h"
#include "guarded.h"
#include "lint.h"
#include "mathwords.h"
#include "native.h"

#include "qdos_version.h"
#include "wordlist.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Maximum characters in the input line. */
#define INPUT_MAX 256

/** Maximum characters in a pending numeric entry. */
#define ENTRY_MAX 64

/** Evaluator stack capacity, in elements. */
#define STACK_SIZE 4096

/*
 * Screen layout, in rows. A rule is one pixel on the bottom edge of a row, and
 * no glyph reaches that far, so it underlines a row of content rather than
 * taking a row of its own -- the panel has ten and cannot spare two for lines.
 */
/* The list and the editor caption themselves; the calculator does not need to */
#define ROW_HEADER 0
#define ROW_CONTENT_FIRST 1

/* Numbered rows already say how deep the stack is, so it starts at the top */
#define ROW_STACK_FIRST 0
#define ROW_CONTENT_LAST (QDOS_ROWS - 3)
#define ROW_INPUT (QDOS_ROWS - 2)

/*
 * A message borrows the input line rather than keeping a row of its own. The
 * panel has ten rows and a message is on screen for one keypress in twenty, so
 * a row reserved for it is a row wasted nineteen times over. The next key takes
 * the line back -- see handle_key().
 */
#define ROW_MESSAGE ROW_INPUT

/* Last row, so the labels sit against the edge the function keys are under */
#define ROW_SOFT (QDOS_ROWS - 1)

#define SOFT_KEYS 5
#define SOFT_WIDTH (QDOS_COLS / SOFT_KEYS)

#define STACK_ROWS (ROW_CONTENT_LAST - ROW_STACK_FIRST + 1)

/*
 * Half a blink period, so the cursor comes and goes once a second. The panel
 * is a Sharp Memory LCD: it holds its image unpowered and costs only what is
 * clocked into it, and the driver is already sending it a VCOM message every
 * second, so two frames a second sit alongside traffic the machine has anyway.
 */
#define CURSOR_BLINK_MS 500

/*
 * How long the cursor keeps blinking after the last key. Past this the machine
 * has been left alone rather than thought about, so the cursor goes solid --
 * still saying where you are -- and the loop stops waking to toggle it. That is
 * what lets qdos_shell_run() wait on the keypad with no timer at all.
 */
#define CURSOR_SETTLE_MS 10000

/*
 * Minutes of inactivity before the machine turns itself off, nought being
 * never. Calculators normally use five; this starts longer because no boot has
 * been timed on the hardware yet, and a machine that switches off more eagerly
 * than it comes back is worse than one that simply stays on.
 */
static const int AUTO_OFF_MINUTES[] = {0, 5, 10, 30, 60};
#define AUTO_OFF_COUNT (sizeof(AUTO_OFF_MINUTES) / sizeof(AUTO_OFF_MINUTES[0]))
#define AUTO_OFF_DEFAULT 2

/** Long enough to read the warning and reach for a key. */
#define AUTO_OFF_WARN_MS 10000

/** Prompts. The character says which mode the keypad is in. */
#define PROMPT "> "
#define LINE_PROMPT ": "
#define CONT_PROMPT ".."
#define PROMPT_LEN 2

/** @brief What the keypad is doing */
typedef enum {
	QDOS_MODE_CALC, ///< Digits build a number; an operator applies immediately
	QDOS_MODE_LINE,	///< Whole lines of Quadrate, evaluated on Enter
	QDOS_MODE_LIST,	///< Browsing the vocabulary
	QDOS_MODE_EDIT,	   ///< Editing a program in the stack area
	QDOS_MODE_ABOUT,   ///< What this firmware is
	QDOS_MODE_SETTINGS,
	QDOS_MODE_DEBUG	   ///< What the machine has been saying
} qdos_mode;

#define LOG_LINES 48

/** @brief What a setting can be changed to */
typedef enum {
	SETTING_ANGLE = 0,
	SETTING_DECIMALS,
	SETTING_AUTO_OFF,
	SETTING_USB,	 ///< Only where the backend has a gadget to offer
	SETTING_MODULES, ///< Only when something is blocked, there being nothing else to say
	SETTING__COUNT
} qdos_setting;

#define DECIMALS_AUTO -1
#define DECIMALS_MAX 9

/** @brief As many apps as the card may offer at once */
#define QDOS_APPS_MAX 32

/** @brief As many words as one session may declare at the prompt */
#define QDOS_LINE_WORDS 64

/** @brief One registered app word, and what running it needs to know */
typedef struct {
	char name[QDOS_PROGRAM_NAME_MAX];
	struct qdos_shell* sh;
} app_word;

struct qdos_shell {
	qdos_hal* hal;	   ///< Borrowed, not owned
	qd_interp* interp; ///< Owned
	qdos_console con;

	qdos_mode mode;

	char entry[ENTRY_MAX]; ///< Number being typed, not yet on the stack
	size_t entry_len;

	char input[INPUT_MAX]; ///< Line being typed in QDOS_MODE_LINE

	qdos_wordlist list;
	qdos_program_entry apps[QDOS_WORDLIST_MAX];
	size_t app_count;

	/** @brief What each app word was registered with; the interpreter keeps
	 * the pointer, so it has to outlive the registration */
	app_word app_word[QDOS_APPS_MAX];
	size_t app_word_count;

	/** @brief Words declared at the prompt rather than read off the card.
	 * They are in memory only, so `forget` has nothing to erase for them. */
	char line_word[QDOS_LINE_WORDS][QDOS_PROGRAM_NAME_MAX];
	size_t line_word_count;
	bool list_all; ///< Every word, rather than just the installed programs
	size_t list_sel;
	size_t list_top;
	qdos_mode list_from;
	qdos_mode about_from; ///< Mode to return to
	qdos_mode page_from;  ///< Where settings and debug were opened from

	char log[LOG_LINES][QDOS_COLS + 1];
	size_t log_count; ///< Lines ever written, so the ring can be read in order
	size_t log_top;   ///< First line shown on the debug page

	size_t setting_sel;
	int decimals; ///< DECIMALS_AUTO, or how many to show after the point

	qdos_value undo[QDOS_REGISTER_MAX];
	size_t undo_depth;
	bool undo_ready;
	bool delete_armed;  ///< One press of backspace has already asked
	bool drop_armed;	///< One press of ESC has already asked, in the editor
	bool powering_off;  ///< Set by the power key, acted on by the run loop
	bool usb_exported;  ///< The inbox is currently a PC's to write to

	enum { REGISTER_IDLE = 0, REGISTER_STORING, REGISTER_RECALLING } register_wait;

	qdos_natives natives; ///< Uploaded code, and its words in the vocabulary

	qdos_editor ed;
	size_t ed_top; ///< First visible line
	size_t input_len;
	size_t input_cursor;

	char message[QDOS_COLS + 1]; ///< Error or status under the stack
	bool message_is_error;

	bool cursor_on; ///< Which half of the blink the cursor is in

	size_t auto_off;  ///< Index into AUTO_OFF_MINUTES
	bool off_warned;  ///< The warning is already on screen
};

/** @brief How long the machine may sit idle, in ms; nought means never */
static uint32_t auto_off_ms(const qdos_shell* sh) {
	// Nothing to come back to a half-finished transfer for, so a shared card
	// keeps the machine awake however long it is left.
	if (sh->usb_exported)
		return 0;

	return (uint32_t)AUTO_OFF_MINUTES[sh->auto_off] * 60u * 1000u;
}

/** @brief A keypad with one face reports none, and costs no space */
static char modifier_char(const qdos_shell* sh) {
	if (sh->hal->modifier == NULL)
		return '\0';

	switch (sh->hal->modifier(sh->hal)) {
		case QDOS_MOD_ALPHA: return 'A';
		case QDOS_MOD_SYMBOL: return '#';
		default: return '\0';
	}
}

/** @brief Is there a cursor on screen, and therefore something to blink? */
static bool has_cursor(const qdos_shell* sh) {
	if (sh->mode == QDOS_MODE_EDIT)
		return true;

	// Everywhere else the caret is on the input row, which a message takes
	// over. The pages -- list, settings, debug, about -- have no caret at all.
	if (sh->mode == QDOS_MODE_CALC || sh->mode == QDOS_MODE_LINE)
		return sh->message[0] == '\0';

	return false;
}

/** @brief Append text to the input line, silently ignoring overflow */
static void input_append(qdos_shell* sh, const char* text) {
	const size_t len = strlen(text);
	if (sh->input_len + len >= INPUT_MAX)
		return;

	memmove(sh->input + sh->input_cursor + len, sh->input + sh->input_cursor,
			sh->input_len - sh->input_cursor);
	memcpy(sh->input + sh->input_cursor, text, len);
	sh->input_len += len;
	sh->input_cursor += len;
	sh->input[sh->input_len] = '\0';
}

static void input_backspace(qdos_shell* sh) {
	if (sh->input_cursor == 0)
		return;

	memmove(sh->input + sh->input_cursor - 1, sh->input + sh->input_cursor,
			sh->input_len - sh->input_cursor);
	sh->input_cursor--;
	sh->input_len--;
	sh->input[sh->input_len] = '\0';
}

static void input_clear(qdos_shell* sh) {
	sh->input_len = 0;
	sh->input_cursor = 0;
	sh->input[0] = '\0';
}

/*
 * Every evaluation goes through here. A type error in lib/rt -- `1.5 2.5 and`,
 * a string where a number was wanted -- is fatal and would end the process, so
 * recovery is armed and the runtime longjmps back instead. The stack may have
 * been left part-way through the failed word, which is the documented price.
 */


/** @brief Append one line, wrapped to the panel, to the ring */
static void log_line(qdos_shell* sh, const char* text, size_t len) {
	while (len > 0) {
		const size_t take = (len > QDOS_COLS) ? (size_t)QDOS_COLS : len;
		char* slot = sh->log[sh->log_count % LOG_LINES];
		memcpy(slot, text, take);
		slot[take] = '\0';
		sh->log_count++;
		text += take;
		len -= take;
	}
}

/** @brief Record whatever a program printed, or anything worth reading later */
static void log_add(qdos_shell* sh, const char* text) {
	if (text == NULL || text[0] == '\0')
		return;

	const char* start = text;
	for (const char* p = text;; p++) {
		if (*p != '\n' && *p != '\0')
			continue;
		if (p > start)
			log_line(sh, start, (size_t)(p - start));
		if (*p == '\0')
			break;
		start = p + 1;
	}
}

/** @brief The i-th line still in the ring, oldest first, or NULL */
static const char* log_at(const qdos_shell* sh, size_t index) {
	const size_t held = (sh->log_count < LOG_LINES) ? sh->log_count : LOG_LINES;
	if (index >= held)
		return NULL;
	const size_t first = sh->log_count - held;
	return sh->log[(first + index) % LOG_LINES];
}

static size_t log_held(const qdos_shell* sh) {
	return (sh->log_count < LOG_LINES) ? sh->log_count : LOG_LINES;
}

/**
 * @brief A value as the settings say to show it, in @p room columns
 *
 * Too wide for the row, a number goes to exponent form: there is no end of one
 * that is safe to drop.
 */
static void format_value(
		const qdos_shell* sh, const qd_interp_value* value, char* out, size_t cap, size_t room) {
	const bool number = value->type == QD_INTERP_VALUE_INT || value->type == QD_INTERP_VALUE_FLOAT;
	const double shown = (value->type == QD_INTERP_VALUE_INT) ? (double)value->i : value->f;

	if (sh->decimals == DECIMALS_AUTO || !number) {
		snprintf(out, cap, "%s", value->text);
	} else {
		// A fixed number of decimals is a column to read down, so whole numbers
		// get them too rather than jumping about
		snprintf(out, cap, "%.*f", sh->decimals, shown);
	}

	if (!number || strlen(out) <= room)
		return;

	for (int digits = 9; digits >= 0; digits--) {
		snprintf(out, cap, "%.*e", digits, shown);
		if (strlen(out) <= room)
			return;
	}
}

static void set_message(qdos_shell* sh, const char* text, bool is_error) {
	snprintf(sh->message, sizeof(sh->message), "%s", text ? text : "");
	sh->message_is_error = is_error;
	if (is_error)
		log_add(sh, sh->message);
}

/** @brief Is the line ready to evaluate, or still open? */
static bool input_is_complete(const qdos_shell* sh) {
	int depth = 0;
	bool in_string = false;

	for (size_t i = 0; i < sh->input_len; i++) {
		const char ch = sh->input[i];

		if (in_string) {
			if (ch == '\\' && i + 1 < sh->input_len) {
				i++; // an escaped character is never a delimiter
			} else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		switch (ch) {
			case '"': in_string = true; break;
			case '{':
			case '(':
			case '[': depth++; break;
			case '}':
			case ')':
			case ']': depth--; break;
			default: break;
		}
	}

	return depth <= 0;
}

/**
 * @brief Say what the last eval declared
 *
 * A word written on the line lives in memory only. The card holds programs,
 * which are put there by `edit` or by uploading them, and scratch work at the
 * prompt is not that -- it would otherwise fill the store with every `sq` ever
 * tried, each one a row in APPS.
 */
static void report_declaration(qdos_shell* sh) {
	const char* name = qd_interp_last_declared(sh->interp);
	if (!name) {
		set_message(sh, "", false);
		return;
	}

	bool known = false;
	for (size_t i = 0; i < sh->line_word_count; i++)
		known = known || strcmp(sh->line_word[i], name) == 0;

	if (!known && sh->line_word_count < QDOS_LINE_WORDS)
		snprintf(sh->line_word[sh->line_word_count++], QDOS_PROGRAM_NAME_MAX, "%s", name);

	char message[80];
	snprintf(message, sizeof(message), "DECLARED '%.20s'", name);
	set_message(sh, message, false);
}

/** @brief Whether this word was written at the prompt, and so is not on the card */
static bool declared_here(qdos_shell* sh, const char* name) {
	for (size_t i = 0; i < sh->line_word_count; i++)
		if (strcmp(sh->line_word[i], name) == 0)
			return true;
	return false;
}

static void forget_here(qdos_shell* sh, const char* name) {
	for (size_t i = 0; i < sh->line_word_count; i++) {
		if (strcmp(sh->line_word[i], name) != 0)
			continue;
		memcpy(sh->line_word[i], sh->line_word[--sh->line_word_count], QDOS_PROGRAM_NAME_MAX);
		return;
	}
}

/** A soft key's label, and the key it stands for in this mode */
typedef struct {
	const char* label;
	qdos_key key;
} soft_key;

static const soft_key SOFT[7][SOFT_KEYS] = {
	// Turning off is the PWR key's job: the one action on the row that cannot be
	// undone by pressing it again. The last slot's label is the setting itself.
	[QDOS_MODE_CALC] ={{"CLR", QDOS_KEY_CLEAR}, {"APPS", QDOS_KEY_LIST}, {"CAT", QDOS_KEY_CATALOG},
			{"INFO", QDOS_KEY_ABOUT}, {"", QDOS_KEY_ANGLE}},
	[QDOS_MODE_LINE] = {{"ESC", QDOS_KEY_CLEAR}, {"APPS", QDOS_KEY_LIST}, {"COMP", QDOS_KEY_TAB},
			{"CAT", QDOS_KEY_CATALOG}, {"", QDOS_KEY_ANGLE}},
	// down then up, so the pair sits like vim's j and k
	[QDOS_MODE_LIST] = {{"ESC", QDOS_KEY_CLEAR}, {QDOS_GLYPH_DOWN, QDOS_KEY_DOWN}, {QDOS_GLYPH_UP, QDOS_KEY_UP},
			{"PICK", QDOS_KEY_ENTER}, {"EDIT", QDOS_KEY_OPEN}},
	// ESC, as everywhere else: DROP is a keypad word that empties the stack
	[QDOS_MODE_EDIT] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"CHECK", QDOS_KEY_CHECK},
			{"", QDOS_KEY_NONE}, {"SAVE", QDOS_KEY_SAVE}},
	[QDOS_MODE_ABOUT] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"SET", QDOS_KEY_SETTINGS},
			{"LOG", QDOS_KEY_DEBUG}, {"", QDOS_KEY_NONE}},
	[QDOS_MODE_SETTINGS] = {{"ESC", QDOS_KEY_CLEAR}, {QDOS_GLYPH_DOWN, QDOS_KEY_DOWN}, {QDOS_GLYPH_UP, QDOS_KEY_UP},
			{"CHG", QDOS_KEY_ENTER}, {"LOG", QDOS_KEY_DEBUG}},
	[QDOS_MODE_DEBUG] = {{"ESC", QDOS_KEY_CLEAR}, {QDOS_GLYPH_DOWN, QDOS_KEY_DOWN}, {QDOS_GLYPH_UP, QDOS_KEY_UP},
			{"CLR", QDOS_KEY_BACKSPACE}, {"SET", QDOS_KEY_SETTINGS}},
};

static const char* angle_text(void);
static const qdos_native_entry* list_module(const qdos_shell* sh, size_t i);
static void register_apps(qdos_shell* sh, qd_interp* interp);

static void render_soft(qdos_shell* sh, qdos_console* con) {
	for (int i = 0; i < SOFT_KEYS; i++) {
		const char* label = SOFT[sh->mode][i].label;

		if (SOFT[sh->mode][i].key == QDOS_KEY_ANGLE)
			label = angle_text();

		// Two jobs, so it names whichever is next
		if (sh->mode == QDOS_MODE_LINE && SOFT[sh->mode][i].key == QDOS_KEY_CLEAR)
			label = (sh->input_len > 0) ? "CLR" : "ESC";

		if (label[0] == '\0')
			continue;

		// Only a program can be edited
		if (sh->mode == QDOS_MODE_LIST && SOFT[sh->mode][i].key == QDOS_KEY_OPEN
				&& (sh->list_all || list_module(sh, sh->list_sel) != NULL))
			continue;

		const int width = (int)strlen(label);
		qdos_console_puts(con, i * SOFT_WIDTH + (SOFT_WIDTH - width) / 2, ROW_SOFT, label);
	}
}

static int push_value(qd_context* ctx, const qdos_value* value);

/** @brief Take what an evaluation printed and put it where it can be seen */
static void absorb_output(qdos_shell* sh) {
	const char* printed = qdos_guarded_output();
	if (printed[0] == '\0')
		return;

	log_add(sh, printed);
	const size_t held = log_held(sh);
	if (held > 0)
		set_message(sh, log_at(sh, held - 1), false);
}

/* One step back, which is all a calculator ever offers */
static void undo_snapshot(qdos_shell* sh) {
	const size_t depth = qd_interp_depth(sh->interp);
	sh->undo_depth = (depth > QDOS_REGISTER_MAX) ? (size_t)QDOS_REGISTER_MAX : depth;

	for (size_t i = 0; i < sh->undo_depth; i++) {
		qd_interp_value value;
		if (!qd_interp_peek(sh->interp, i, &value)) {
			sh->undo_depth = i;
			break;
		}

		qdos_value* slot = &sh->undo[i];
		if (value.type == QD_INTERP_VALUE_INT) {
			slot->type = QDOS_VALUE_INT;
			slot->i = value.i;
		} else if (value.type == QD_INTERP_VALUE_FLOAT) {
			slot->type = QDOS_VALUE_FLOAT;
			slot->f = value.f;
		} else {
			slot->type = QDOS_VALUE_STRING;
			snprintf(slot->s, sizeof(slot->s), "%s", value.text);
		}
	}
	sh->undo_ready = true;
}

static void undo_restore(qdos_shell* sh) {
	if (!sh->undo_ready) {
		set_message(sh, "NOTHING TO UNDO", false);
		return;
	}

	qd_context* ctx = qd_interp_context(sh->interp);
	qdos_guarded_eval(sh->interp, "clear");

	// Snapshots run top-first, so put them back the other way round
	for (size_t i = sh->undo_depth; i > 0; i--)
		push_value(ctx, &sh->undo[i - 1]);

	sh->undo_ready = false;
	set_message(sh, "UNDONE", false);
}

static bool infix_operator(char ch) {
	return ch == '+' || ch == '-' || ch == '*' || ch == '/';
}

/** @brief Is the whole of this the way a number is written? */
static bool infix_number(const char* text, size_t len) {
	bool digit = false;
	bool point = false;

	for (size_t i = 0; i < len; i++) {
		if (text[i] >= '0' && text[i] <= '9') {
			digit = true;
		} else if (text[i] == '.' && !point) {
			point = true;
		} else if (text[i] != '-' || i != 0) {
			return false;
		}
	}
	return digit;
}

/**
 * @brief Catch a line written the way it is said aloud
 *
 * `5 - 3` and `5-3` are both valid Quadrate and neither is two. See
 * docs/design.md.
 *
 * @return true when @p out holds what to say instead
 */
static bool infix_hint(const char* line, size_t len, char* out, size_t cap) {
	const char* token[4];
	size_t token_len[4];
	size_t count = 0;

	for (size_t i = 0; i < len;) {
		while (i < len && line[i] == ' ')
			i++;
		if (i >= len)
			break;
		if (count == 4)
			return false; // more on the line than this mistake is made of
		token[count] = line + i;
		while (i < len && line[i] != ' ')
			i++;
		token_len[count] = (size_t)(line + i - token[count]);
		count++;
	}

	const char* left = NULL;
	const char* right = NULL;
	size_t left_len = 0;
	size_t right_len = 0;
	char op = 0;

	if (count == 3 && token_len[1] == 1 && infix_operator(token[1][0])) {
		left = token[0];
		left_len = token_len[0];
		right = token[2];
		right_len = token_len[2];
		op = token[1][0];
	} else if (count == 1) {
		// Never the first character, which is a sign rather than an operator
		for (size_t i = 1; i + 1 < token_len[0]; i++) {
			if (!infix_operator(token[0][i]))
				continue;
			left = token[0];
			left_len = i;
			right = token[0] + i + 1;
			right_len = token_len[0] - i - 1;
			op = token[0][i];
			break;
		}
	}

	if (op == 0 || !infix_number(left, left_len) || !infix_number(right, right_len))
		return false;

	const int shown = 6;
	snprintf(out, cap, "RPN: TRY %.*s %.*s %c", (int)(left_len < (size_t)shown ? left_len : (size_t)shown),
			left, (int)(right_len < (size_t)shown ? right_len : (size_t)shown), right, op);
	return true;
}

/** @brief Evaluate the input line and report the outcome */
static void submit(qdos_shell* sh) {
	if (sh->input_len == 0)
		return;

	// Caught before anything on the stack moves
	char hint[QDOS_COLS + 1];
	if (infix_hint(sh->input, sh->input_len, hint, sizeof(hint))) {
		set_message(sh, hint, true);
		return;
	}

	// A word may take over the screen, and then the line and the message it
	// left are its business, not ours
	const qdos_mode before = sh->mode;

	undo_snapshot(sh);
	if (qdos_guarded_eval(sh->interp, sh->input)) {
		if (sh->mode == before)
			report_declaration(sh);
	} else {
		set_message(sh, qdos_guarded_error(sh->interp), true);

		// The line stays, to be corrected rather than typed again
		if (sh->mode == before)
			return;
	}

	// What a program printed outranks whatever the shell had to say
	if (sh->mode == before)
		absorb_output(sh);

	input_clear(sh);
}

/** @brief Complete the word being typed, or report why nothing happened */
static void complete_word(qdos_shell* sh) {
	const size_t n = qdos_complete_prefix_len(sh->input, sh->input_len);
	if (n == 0) {
		return;
	}

	char prefix[QDOS_WORD_MAX];
	memcpy(prefix, sh->input + sh->input_len - n, n);
	prefix[n] = '\0';

	qdos_completion done;
	qdos_complete(sh->interp, prefix, &done);

	if (done.matches == 0) {
		set_message(sh, "NO MATCH", false);
		return;
	}

	// The shared prefix is always safe to type for the user, however many
	// words matched. Only when it adds nothing is the list worth showing.
	if (strlen(done.common) > n) {
		input_append(sh, done.common + n);
	}

	if (done.matches == 1) {
		input_append(sh, " ");
		set_message(sh, "", false);
	} else {
		set_message(sh, done.listing, false);
	}
}

/** @brief Translate a key press into an edit or an action */
/** Append to the pending number, ignoring overflow. */
static void entry_append(qdos_shell* sh, char ch) {
	if (sh->entry_len + 1 >= ENTRY_MAX) {
		return;
	}
	sh->entry[sh->entry_len++] = ch;
	sh->entry[sh->entry_len] = '\0';
}

static void entry_clear(qdos_shell* sh) {
	sh->entry_len = 0;
	sh->entry[0] = '\0';
}

/**
 * @brief Put the pending number on the stack
 * @return false if the entry would not parse
 */
static bool entry_commit(qdos_shell* sh) {
	if (sh->entry_len == 0) {
		return true;
	}

	undo_snapshot(sh);
	if (!qdos_guarded_eval(sh->interp, sh->entry)) {
		set_message(sh, qdos_guarded_error(sh->interp), true);
		return false;
	}
	entry_clear(sh);
	return true;
}

/** @brief Apply a word to the stack, committing any pending number first */
static void apply_word(qdos_shell* sh, const char* word) {
	if (!entry_commit(sh)) {
		return;
	}
	undo_snapshot(sh);
	if (qdos_guarded_eval(sh->interp, word)) {
		set_message(sh, "", false);
		absorb_output(sh);
	} else {
		set_message(sh, qdos_guarded_error(sh->interp), true);
	}
}

static void enter_line_mode(qdos_shell* sh) {
	// Commit first, or the digits already typed are lost.
	entry_commit(sh);
	sh->mode = QDOS_MODE_LINE;
	input_clear(sh);

	// No announcement: it would sit on the input line and hide the ':' prompt,
	// which is what says the mode changed. The soft row already labels ESC.
	set_message(sh, "", false);
}

static void leave_line_mode(qdos_shell* sh) {
	sh->mode = QDOS_MODE_CALC;
	input_clear(sh);
	set_message(sh, "", false);
}

/** Keys while the keypad is a calculator. */
/* In QDOS_KEY_FN_FIRST..QDOS_KEY_FN_LAST order */
static const char* const FUNCTION_WORD[] = {
	"sin", "cos", "tan", "ln", "log", "sqrt", "sq",
	"pow", "inv", "abs", "floor", "ceil", "round", "mod",
	"rot", "over",
};

static const char* function_word(qdos_key key) {
	if (key < QDOS_KEY_FN_FIRST || key > QDOS_KEY_FN_LAST)
		return NULL;
	return FUNCTION_WORD[key - QDOS_KEY_FN_FIRST];
}

/* Flips the sign of the number being typed, or negates x when none is */
static void entry_negate(qdos_shell* sh) {
	if (sh->entry_len == 0) {
		apply_word(sh, "neg");
		return;
	}

	if (sh->entry[0] == '-') {
		memmove(sh->entry, sh->entry + 1, sh->entry_len);
		sh->entry_len--;
	} else if (sh->entry_len + 1 < ENTRY_MAX) {
		memmove(sh->entry + 1, sh->entry, sh->entry_len + 1);
		sh->entry[0] = '-';
		sh->entry_len++;
	}
}

/** @brief Nought to nine; the other ninety need `sto` and `rcl` written out */
static bool register_digit(qdos_shell* sh, const qdos_key_event* ev) {
	if (sh->register_wait == REGISTER_IDLE)
		return false;

	const bool storing = (sh->register_wait == REGISTER_STORING);
	sh->register_wait = REGISTER_IDLE;

	if (ev->key < QDOS_KEY_0 || ev->key > QDOS_KEY_9) {
		set_message(sh, "CANCELLED", false);
		return true;
	}

	char line[16];
	snprintf(line, sizeof(line), "%d %s", (int)(ev->key - QDOS_KEY_0), storing ? "sto" : "rcl");

	undo_snapshot(sh);
	if (!qdos_guarded_eval(sh->interp, line)) {
		set_message(sh, qdos_guarded_error(sh->interp), true);
		return true;
	}

	char message[QDOS_COLS + 1];
	snprintf(message, sizeof(message), "%s %d", storing ? "STORED" : "RECALLED",
			(int)(ev->key - QDOS_KEY_0));
	set_message(sh, message, false);
	return true;
}

static void handle_calc_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (register_digit(sh, ev))
		return;

	const char* word = function_word(ev->key);
	if (word != NULL) {
		apply_word(sh, word);
		return;
	}

	switch (ev->key) {
		case QDOS_KEY_0: entry_append(sh, '0'); break;
		case QDOS_KEY_1: entry_append(sh, '1'); break;
		case QDOS_KEY_2: entry_append(sh, '2'); break;
		case QDOS_KEY_3: entry_append(sh, '3'); break;
		case QDOS_KEY_4: entry_append(sh, '4'); break;
		case QDOS_KEY_5: entry_append(sh, '5'); break;
		case QDOS_KEY_6: entry_append(sh, '6'); break;
		case QDOS_KEY_7: entry_append(sh, '7'); break;
		case QDOS_KEY_8: entry_append(sh, '8'); break;
		case QDOS_KEY_9: entry_append(sh, '9'); break;
		case QDOS_KEY_DOT: entry_append(sh, '.'); break;

		case QDOS_KEY_ADD: apply_word(sh, "+"); break;
		case QDOS_KEY_SUB: apply_word(sh, "-"); break;
		case QDOS_KEY_MUL: apply_word(sh, "*"); break;
		// A calculator divides rather than truncating; the language keeps "/"
		case QDOS_KEY_DIV: apply_word(sh, "divide"); break;
		case QDOS_KEY_DUP: apply_word(sh, "dup"); break;
		case QDOS_KEY_DROP: apply_word(sh, "drop"); break;
		case QDOS_KEY_SWAP: apply_word(sh, "swap"); break;
		case QDOS_KEY_NEG: entry_negate(sh); break;
		case QDOS_KEY_UNDO: undo_restore(sh); break;

		// Commit first, or the register gets what was under the entry
		case QDOS_KEY_STO:
		case QDOS_KEY_RCL:
			if (!entry_commit(sh))
				break;
			sh->register_wait = (ev->key == QDOS_KEY_STO) ? REGISTER_STORING : REGISTER_RECALLING;
			set_message(sh, ev->key == QDOS_KEY_STO ? "STO: WHICH? 0-9" : "RCL: WHICH? 0-9", false);
			break;

		// Bare Enter duplicates: how an RPN calculator squares a number.
		case QDOS_KEY_ENTER:
			if (sh->entry_len > 0) {
				if (entry_commit(sh)) {
					set_message(sh, "", false);
				}
			} else {
				apply_word(sh, "dup");
			}
			break;

		case QDOS_KEY_BACKSPACE:
			if (sh->entry_len > 0) {
				sh->entry[--sh->entry_len] = '\0';
			}
			break;

		case QDOS_KEY_CLEAR:
			if (sh->entry_len > 0) {
				entry_clear(sh);
			} else {
				undo_snapshot(sh);
				qdos_guarded_eval(sh->interp, "clear");
				set_message(sh, "STACK CLEARED", false);
			}
			break;

		case QDOS_KEY_CHAR:
			if (ev->ch == ':') {
				enter_line_mode(sh);
			} else if (ev->ch != ' ') {
				set_message(sh, "PRESS : TO TYPE A LINE", false);
			}
			break;

		default:
			break;
	}
}

/** Keys while whole lines of Quadrate are being typed. */
static void handle_line_key(qdos_shell* sh, const qdos_key_event* ev) {
	const char* word = function_word(ev->key);
	if (word != NULL) {
		input_append(sh, " ");
		input_append(sh, word);
		input_append(sh, " ");
		return;
	}

	switch (ev->key) {
		case QDOS_KEY_0: input_append(sh, "0"); break;
		case QDOS_KEY_1: input_append(sh, "1"); break;
		case QDOS_KEY_2: input_append(sh, "2"); break;
		case QDOS_KEY_3: input_append(sh, "3"); break;
		case QDOS_KEY_4: input_append(sh, "4"); break;
		case QDOS_KEY_5: input_append(sh, "5"); break;
		case QDOS_KEY_6: input_append(sh, "6"); break;
		case QDOS_KEY_7: input_append(sh, "7"); break;
		case QDOS_KEY_8: input_append(sh, "8"); break;
		case QDOS_KEY_9: input_append(sh, "9"); break;
		case QDOS_KEY_DOT: input_append(sh, "."); break;

		case QDOS_KEY_ADD: input_append(sh, "+"); break;
		case QDOS_KEY_SUB: input_append(sh, "-"); break;
		case QDOS_KEY_MUL: input_append(sh, "*"); break;
		case QDOS_KEY_DIV: input_append(sh, "/"); break;
		case QDOS_KEY_DUP: input_append(sh, "dup"); break;
		case QDOS_KEY_DROP: input_append(sh, "drop"); break;
		case QDOS_KEY_SWAP: input_append(sh, "swap"); break;
		case QDOS_KEY_NEG: input_append(sh, " neg "); break;

		case QDOS_KEY_STO: input_append(sh, " sto "); break;
		case QDOS_KEY_RCL: input_append(sh, " rcl "); break;

		case QDOS_KEY_CHAR:
			if (ev->ch) {
				const char text[2] = {ev->ch, '\0'};
				input_append(sh, text);
			}
			break;

		case QDOS_KEY_ENTER:
			if (input_is_complete(sh)) {
				submit(sh);
			} else {
				input_append(sh, "\n");
			}
			break;

		case QDOS_KEY_BACKSPACE:
			input_backspace(sh);
			break;

		case QDOS_KEY_TAB:
			complete_word(sh);
			break;

		case QDOS_KEY_LEFT:
			if (sh->input_cursor > 0)
				sh->input_cursor--;
			break;

		case QDOS_KEY_RIGHT:
			if (sh->input_cursor < sh->input_len)
				sh->input_cursor++;
			break;

		// The line first, the mode second, as CLR does in the calculator. A line
		// that survives a failed eval needs somewhere to be thrown away.
		case QDOS_KEY_CLEAR:
			if (sh->input_len > 0)
				input_clear(sh);
			else
				leave_line_mode(sh);
			break;

		default:
			break;
	}
}

static void edit_open(qdos_shell* sh, const char* name, const char* source);

#define LIST_ROWS (ROW_CONTENT_LAST - ROW_CONTENT_FIRST + 1)

/**
 * @brief Shared libraries, being what the programs under them are liable to call
 *
 * A module inside an app is that app's own half rather than something the card
 * offers, so it is loaded and callable but never listed: the row that matters
 * is the app.
 */
static size_t module_count(const qdos_shell* sh) {
	size_t n = 0;
	for (size_t i = 0; i < sh->natives.count; i++)
		if (sh->natives.entry[i].app[0] == '\0')
			n++;
	return n;
}

static size_t list_count(const qdos_shell* sh) {
	if (sh->list_all)
		return sh->list.count;
	return module_count(sh) + sh->app_count;
}

/** @brief The module a row shows, or NULL where the row is a program */
static const qdos_native_entry* list_module(const qdos_shell* sh, size_t i) {
	if (sh->list_all)
		return NULL;

	size_t at = 0;
	for (size_t n = 0; n < sh->natives.count; n++) {
		if (sh->natives.entry[n].app[0] != '\0')
			continue;
		if (at++ == i)
			return &sh->natives.entry[n];
	}
	return NULL;
}

/** @brief The program a row shows, or NULL where the row is a module */
static const qdos_program_entry* list_program(const qdos_shell* sh, size_t i) {
	if (sh->list_all || i < module_count(sh))
		return NULL;

	const size_t at = i - module_count(sh);
	return (at < sh->app_count) ? &sh->apps[at] : NULL;
}

static const char* list_name(const qdos_shell* sh, size_t i) {
	if (sh->list_all)
		return sh->list.name[i];

	const qdos_native_entry* module = list_module(sh, i);
	if (module != NULL)
		return module->name;

	const qdos_program_entry* program = list_program(sh, i);
	return (program != NULL) ? program->name : "";
}

/** @brief A star is an override with a read-only copy underneath */
static const char* origin_text(bool system, bool inbox, bool user) {
	if (user)
		return (system || inbox) ? "USER*" : "USER";
	return inbox ? "CARD" : "SYS";
}

/** @brief Read a program from wherever it lives, nearest scope first */
static bool load_program_anywhere(qdos_shell* sh, const char* name, char* out, size_t cap) {
	static const qdos_store_scope ORDER[] = {QDOS_SCOPE_USER, QDOS_SCOPE_INBOX, QDOS_SCOPE_SYSTEM};

	for (size_t i = 0; i < sizeof(ORDER) / sizeof(*ORDER); i++) {
		if (qdos_program_load(sh->hal, ORDER[i], name, out, cap) == QDOS_STORE_OK)
			return true;
	}
	return false;
}

static void list_load(qdos_shell* sh) {
	if (sh->list_all)
		qdos_wordlist_gather(sh->interp, &sh->list);
	else
		sh->app_count = qdos_programs_gather(sh->hal, sh->apps, QDOS_WORDLIST_MAX);

	sh->list_sel = 0;
	sh->list_top = 0;
}

static void list_open_scoped(qdos_shell* sh, bool all) {
	sh->list_all = all;
	list_load(sh);
	sh->list_from = sh->mode;
	sh->mode = QDOS_MODE_LIST;
	set_message(sh, "", false);
}

static void list_open(qdos_shell* sh) {
	list_open_scoped(sh, false);
}

/* Every word, the way a TI-83 reaches the ones with no key of their own */
static void catalog_open(qdos_shell* sh) {
	list_open_scoped(sh, true);
}

static void list_scroll_into_view(qdos_shell* sh) {
	if (sh->list_sel < sh->list_top)
		sh->list_top = sh->list_sel;
	else if (sh->list_sel >= sh->list_top + LIST_ROWS)
		sh->list_top = sh->list_sel - (LIST_ROWS - 1);
}

static void handle_list_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (ev->key != QDOS_KEY_BACKSPACE)
		sh->delete_armed = false;

	switch (ev->key) {
		case QDOS_KEY_UP:
			if (sh->list_sel > 0)
				sh->list_sel--;
			list_scroll_into_view(sh);
			break;

		case QDOS_KEY_DOWN:
			if (sh->list_sel + 1 < list_count(sh))
				sh->list_sel++;
			list_scroll_into_view(sh);
			break;

		case QDOS_KEY_LEFT:
			sh->list_sel = (sh->list_sel > LIST_ROWS) ? sh->list_sel - LIST_ROWS : 0;
			list_scroll_into_view(sh);
			break;

		case QDOS_KEY_RIGHT:
			sh->list_sel += LIST_ROWS;
			if (sh->list_sel >= list_count(sh))
				sh->list_sel = list_count(sh) ? list_count(sh) - 1 : 0;
			list_scroll_into_view(sh);
			break;

		case QDOS_KEY_TAB:
			sh->list_all = !sh->list_all;
			list_load(sh);
			break;

		// Typing a letter jumps to it, which is how a catalog of 100 is usable
		case QDOS_KEY_CHAR: {
			const char want = (ev->ch >= 'A' && ev->ch <= 'Z') ? (char)(ev->ch + 32) : ev->ch;
			for (size_t i = 0; i < list_count(sh); i++) {
				if (list_name(sh, i)[0] == want) {
					sh->list_sel = i;
					sh->list_top = i; // the match at the top, with its neighbours under it
					list_scroll_into_view(sh);
					break;
				}
			}
			break;
		}

		case QDOS_KEY_ENTER:
			// Picking types the name, leaving the user to run or edit it
			sh->mode = QDOS_MODE_LINE;
			if (list_count(sh) > 0) {
				input_append(sh, list_name(sh, sh->list_sel));

				// A library is a scope; Tab does the rest. A program is a word.
				const qdos_native_entry* picked = list_module(sh, sh->list_sel);
				input_append(sh, (picked != NULL && !qdos_native_is_app(picked)) ? "::" : " ");
			}
			break;

		case QDOS_KEY_OPEN: {
			// A module is not text and there is nothing to open
			if (list_count(sh) == 0 || sh->list_all || list_module(sh, sh->list_sel) != NULL)
				break;

			const char* name = list_name(sh, sh->list_sel);
			char source[QDOS_PROGRAM_MAX];
			if (load_program_anywhere(sh, name, source, sizeof(source)))
				edit_open(sh, name, source);
			break;
		}

		case QDOS_KEY_BACKSPACE: {
			if (sh->list_all || list_count(sh) == 0)
				break;

			// Nothing here writes a module, so there is no copy to drop
			if (list_module(sh, sh->list_sel) != NULL) {
				set_message(sh, "TAKE IT OFF THE CARD", false);
				break;
			}

			const char* name = list_name(sh, sh->list_sel);
			if (!qdos_program_is_user(sh->hal, name)) {
				set_message(sh, qdos_program_is_inbox(sh->hal, name) ? "TAKE IT OFF THE CARD"
																	 : "THAT ONE IS SHIPPED",
						false);
				break;
			}

			// Asking once, because there is no way back from this
			char confirm[QDOS_COLS + 8];
			snprintf(confirm, sizeof(confirm), "BKS AGAIN: DROP '%.8s'", name);
			if (!sh->delete_armed) {
				sh->delete_armed = true;
				set_message(sh, confirm, false);
				break;
			}

			char message[QDOS_COLS + 8];
			qdos_program_erase(sh->hal, name);
			snprintf(message, sizeof(message), "DROPPED '%.10s'", name);
			sh->delete_armed = false;
			list_load(sh);
			if (sh->list_sel >= list_count(sh) && sh->list_sel > 0)
				sh->list_sel--;
			set_message(sh, message, false);
			break;
		}

		case QDOS_KEY_LIST:
		case QDOS_KEY_CLEAR:
			sh->mode = sh->list_from;
			break;

		default:
			break;
	}
}

#define EDIT_ROWS (ROW_CONTENT_LAST - ROW_CONTENT_FIRST + 1)

static void edit_scroll_into_view(qdos_shell* sh) {
	size_t line, col;
	qdos_editor_where(&sh->ed, &line, &col);

	if (line < sh->ed_top)
		sh->ed_top = line;
	else if (line >= sh->ed_top + EDIT_ROWS)
		sh->ed_top = line - (EDIT_ROWS - 1);
}

static void edit_open(qdos_shell* sh, const char* name, const char* source) {
	qdos_editor_open(&sh->ed, name, source);
	sh->ed_top = 0;
	sh->mode = QDOS_MODE_EDIT;
	edit_scroll_into_view(sh);
	set_message(sh, "", false);
}

static void register_natives(qdos_shell* sh, qd_interp* interp);

/**
 * @brief Compile the editor text without letting it reach the session
 *
 * A scratch interpreter, because eval also runs whatever is at top level and
 * declares what parses -- neither belongs in the session until a save.
 *
 * Parsing alone is not much of an answer: declaring a word does not resolve
 * the names in its body, so the lint reads the body afterwards and says what
 * calling it would have said. See src/shell/lint.h.
 */
static void check_program(qdos_shell* sh) {
	qd_interp* scratch = qd_interp_create(STACK_SIZE);
	if (!scratch) {
		set_message(sh, "CANNOT CHECK", true);
		return;
	}

	// The same vocabulary the real interpreter has, or a program that calls
	// another would fail to compile here and nowhere else
	register_natives(sh, scratch);
	qdos_natives_register(&sh->natives, scratch);
	for (int scope = 0; scope < QDOS_SCOPE__COUNT; scope++)
		qdos_programs_restore(sh->hal, (qdos_store_scope)scope, scratch);
	register_apps(sh, scratch);

	char message[80];
	bool bad = !qdos_guarded_eval(scratch, sh->ed.text);
	if (bad)
		snprintf(message, sizeof(message), "%s", qdos_guarded_error(scratch));
	else if (qdos_lint_program(scratch, sh->ed.text, message, sizeof(message)))
		bad = true; // the lint wrote the message
	else
		snprintf(message, sizeof(message), "'%.12s' COMPILES", sh->ed.name);

	qd_interp_destroy(scratch);
	set_message(sh, message, bad);
}

/**
 * @brief Run an app, which is a folder on the card holding main.qd
 *
 * In an interpreter of its own, so that every app may call its entry point
 * `main` and name its helpers whatever suits it without two of them ever
 * meeting. An app holds the screen until `main` returns, and leaves nothing
 * behind in the vocabulary when it does.
 */
static int run_app(qd_context* ctx, void* userdata) {
	(void)ctx;
	qdos_shell* sh = ((app_word*)userdata)->sh;
	const char* name = ((app_word*)userdata)->name;

	char source[QDOS_PROGRAM_MAX];
	if (!load_program_anywhere(sh, name, source, sizeof(source))) {
		set_message(sh, "GONE FROM THE CARD", true);
		return 0;
	}

	qd_interp* app = qd_interp_create(STACK_SIZE);
	if (!app) {
		set_message(sh, "CANNOT RUN IT", true);
		return 0;
	}

	register_natives(sh, app);
	qdos_natives_register(&sh->natives, app);

	if (!qdos_guarded_eval(app, source))
		set_message(sh, qdos_guarded_error(app), true);
	else if (!qdos_guarded_eval(app, QDOS_APP_ENTRY))
		set_message(sh, qdos_guarded_error(app), true);

	qd_interp_destroy(app);
	return 0;
}

/**
 * @brief Make every app on the card a word, so typing its name runs it
 *
 * A slot is found by name and never moved, because the interpreter keeps the
 * pointer: reusing one for a different app would make a name that is still
 * registered run something else. An app taken off the card keeps its slot and
 * says so when it cannot be loaded.
 */
static void register_apps(qdos_shell* sh, qd_interp* interp) {
	qdos_program_entry found[QDOS_WORDLIST_MAX];
	const size_t count = qdos_programs_gather(sh->hal, found, QDOS_WORDLIST_MAX);

	for (size_t i = 0; i < count; i++) {
		if (!found[i].app)
			continue;

		app_word* slot = NULL;
		for (size_t s = 0; s < sh->app_word_count && slot == NULL; s++)
			if (strcmp(sh->app_word[s].name, found[i].name) == 0)
				slot = &sh->app_word[s];

		if (slot == NULL) {
			if (sh->app_word_count >= QDOS_APPS_MAX)
				break;
			slot = &sh->app_word[sh->app_word_count];
			snprintf(slot->name, sizeof(slot->name), "%s", found[i].name);
			slot->sh = sh;
			sh->app_word_count++;
		}

		qd_interp_register(interp, slot->name, "( -- )", run_app, slot);
	}
}

static void edit_insert_word(qdos_shell* sh, const char* word) {
	qdos_editor_insert(&sh->ed, ' ');
	for (const char* c = word; *c; c++)
		qdos_editor_insert(&sh->ed, *c);
	qdos_editor_insert(&sh->ed, ' ');
	edit_scroll_into_view(sh);
}

static void handle_edit_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (ev->key != QDOS_KEY_CLEAR)
		sh->drop_armed = false;

	const char* word = function_word(ev->key);
	if (word != NULL) {
		edit_insert_word(sh, word);
		return;
	}

	switch (ev->key) {
		case QDOS_KEY_STO: edit_insert_word(sh, "sto"); return;
		case QDOS_KEY_RCL: edit_insert_word(sh, "rcl"); return;
		default: break;
	}

	switch (ev->key) {
		case QDOS_KEY_UP: qdos_editor_move(&sh->ed, 0, -1); break;
		case QDOS_KEY_DOWN: qdos_editor_move(&sh->ed, 0, 1); break;
		case QDOS_KEY_LEFT: qdos_editor_move(&sh->ed, -1, 0); break;
		case QDOS_KEY_RIGHT: qdos_editor_move(&sh->ed, 1, 0); break;

		case QDOS_KEY_ENTER: qdos_editor_insert(&sh->ed, '\n'); break;
		case QDOS_KEY_TAB: qdos_editor_insert(&sh->ed, '\t'); break;

		case QDOS_KEY_NEG:
			for (const char* c = " neg "; *c; c++)
				qdos_editor_insert(&sh->ed, *c);
			break;

		case QDOS_KEY_BACKSPACE: qdos_editor_backspace(&sh->ed); break;

		case QDOS_KEY_CHAR:
			if (ev->ch)
				qdos_editor_insert(&sh->ed, ev->ch);
			break;

		case QDOS_KEY_CHECK: check_program(sh); break;

		case QDOS_KEY_SAVE: {
			char message[80];
			if (!qdos_guarded_eval(sh->interp, sh->ed.text)) {
				// Stay in the editor: the text is still the only copy
				set_message(sh, qdos_guarded_error(sh->interp), true);
				break;
			}
			qdos_program_save(sh->hal, sh->ed.name, sh->ed.text);
			snprintf(message, sizeof(message), "SAVED '%.12s'", sh->ed.name);
			sh->mode = QDOS_MODE_CALC;
			set_message(sh, message, false);
			break;
		}

		case QDOS_KEY_CLEAR:
			// The editor holds the only copy; untouched text has nothing to lose
			if (sh->ed.dirty && !sh->drop_armed) {
				sh->drop_armed = true;
				set_message(sh, "ESC AGAIN: LOSE EDITS", false);
				break;
			}
			sh->drop_armed = false;
			sh->mode = QDOS_MODE_CALC;
			set_message(sh, sh->ed.dirty ? "NOT SAVED" : "", false);
			break;

		default:
			break;
	}

	edit_scroll_into_view(sh);
}

static void handle_mode_key(qdos_shell* sh, const qdos_key_event* ev);

/** @brief Translate a soft key press into the key it stands for */
static bool expand_soft(qdos_shell* sh, const qdos_key_event* in, qdos_key_event* out) {
	if (in->key < QDOS_KEY_SOFT1 || in->key > QDOS_KEY_SOFT5)
		return false;

	const soft_key* sk = &SOFT[sh->mode][in->key - QDOS_KEY_SOFT1];
	if (sk->key == QDOS_KEY_NONE)
		return false;

	out->key = sk->key;
	out->ch = 0;
	return true;
}

static void handle_key(qdos_shell* sh, const qdos_key_event* ev) {
	// A message is holding the input line, so this press takes it back. Set
	// first, so whatever this key has to say replaces it rather than being
	// wiped by it.
	sh->message[0] = '\0';
	sh->message_is_error = false;

	// One keypress, one session write at most
	qdos_natives_rearm();

	qdos_key_event expanded;
	if (expand_soft(sh, ev, &expanded)) {
		handle_mode_key(sh, &expanded);
		return;
	}

	handle_mode_key(sh, ev);
}

/**
 * @brief The settings this machine actually has, in the order they are listed
 *
 * Two are conditional, so the page is built rather than numbered and the
 * selection indexes what is on screen.
 */
static size_t settings_visible(const qdos_shell* sh, qdos_setting* out) {
	size_t n = 0;
	out[n++] = SETTING_ANGLE;
	out[n++] = SETTING_DECIMALS;
	out[n++] = SETTING_AUTO_OFF;

	if (sh->hal->usb_export)
		out[n++] = SETTING_USB;
	if (qdos_natives_blocked_count(&sh->natives) > 0)
		out[n++] = SETTING_MODULES;

	return n;
}

static size_t setting_count(const qdos_shell* sh) {
	qdos_setting shown[SETTING__COUNT];
	return settings_visible(sh, shown);
}

/** @brief Which setting the selection is sitting on */
static qdos_setting setting_at(const qdos_shell* sh, size_t index) {
	qdos_setting shown[SETTING__COUNT];
	const size_t count = settings_visible(sh, shown);
	return (index < count) ? shown[index] : shown[0];
}

/**
 * @brief Hand the inbox to a PC, or take it back and read what landed
 *
 * Taking it back is the moment an upload becomes a word, so the programs are
 * declared again there rather than on the next boot.
 */
static void toggle_usb(qdos_shell* sh) {
	const bool want = !sh->usb_exported;

	if (sh->hal->usb_export(sh->hal, want) != 0) {
		set_message(sh, "USB WOULD NOT SWITCH", true);
		return;
	}
	sh->usb_exported = want;

	if (want) {
		set_message(sh, "PLUG INTO A PC", false);
		return;
	}

	const int found = qdos_programs_restore(sh->hal, QDOS_SCOPE_INBOX, sh->interp);
	char message[QDOS_COLS + 1];
	snprintf(message, sizeof(message), "%d FROM THE CARD", found > 0 ? found : 0);
	set_message(sh, message, false);
}

/*
 * One entry per setting rather than one record holding all of them, so adding a
 * setting cannot make the others unreadable: a key nobody writes reads as absent
 * and keeps its default. They carry no ".qd", so the vocabulary never lists them.
 */
#define SETTINGS_KEY_ANGLE "settings.angle"
#define SETTINGS_KEY_DECIMALS "settings.decimals"
#define SETTINGS_KEY_AUTO_OFF "settings.autooff"

static void save_setting(qdos_shell* sh, const char* key, int64_t number) {
	qdos_value value;
	memset(&value, 0, sizeof(value));
	value.type = QDOS_VALUE_INT;
	value.i = number;

	// Nowhere to report a failed write to, and refusing to change the setting on
	// screen because of it would be worse than forgetting it on the next boot.
	qdos_storage_save(sh->hal, key, &value);
}

/** @brief Read one saved setting, leaving @p out alone when there is none */
static void load_setting(qdos_shell* sh, const char* key, int64_t* out) {
	qdos_value value;
	if (qdos_storage_load(sh->hal, key, &value) == QDOS_STORE_OK && value.type == QDOS_VALUE_INT)
		*out = value.i;
}

/** @brief Write the settings out, which is done the moment one changes */
static void save_settings(qdos_shell* sh) {
	save_setting(sh, SETTINGS_KEY_ANGLE, qdos_math_degrees() ? 1 : 0);
	save_setting(sh, SETTINGS_KEY_DECIMALS, sh->decimals);
	save_setting(sh, SETTINGS_KEY_AUTO_OFF, (int64_t)sh->auto_off);
}

/**
 * @brief Put back what was saved, keeping the default where nothing was
 *
 * Every value is checked against what this build accepts. A store written by
 * other firmware must not be able to leave the machine unreadable, or turning
 * off at a timeout this build has no name for.
 */
static void restore_settings(qdos_shell* sh) {
	int64_t degrees = qdos_math_degrees() ? 1 : 0;
	load_setting(sh, SETTINGS_KEY_ANGLE, &degrees);
	qdos_math_set_degrees(degrees != 0);

	int64_t decimals = sh->decimals;
	load_setting(sh, SETTINGS_KEY_DECIMALS, &decimals);
	if (decimals >= DECIMALS_AUTO && decimals <= DECIMALS_MAX)
		sh->decimals = (int)decimals;

	int64_t auto_off = (int64_t)sh->auto_off;
	load_setting(sh, SETTINGS_KEY_AUTO_OFF, &auto_off);
	if (auto_off >= 0 && auto_off < (int64_t)AUTO_OFF_COUNT)
		sh->auto_off = (size_t)auto_off;
}

/**
 * @brief Change the selected setting
 * @param dir +1 to step forwards, -1 back. The two toggles read the same either
 *            way, which is why both arrows reach them.
 */
static void setting_step(qdos_shell* sh, int dir) {
	const qdos_setting setting = setting_at(sh, sh->setting_sel);

	switch (setting) {
		case SETTING_ANGLE:
			qdos_math_set_degrees(!qdos_math_degrees());
			break;

		case SETTING_USB:
			toggle_usb(sh);
			break;

		case SETTING_MODULES:
			qdos_natives_unblock(&sh->natives, sh->hal);
			set_message(sh, "ON AT NEXT START", false);
			break;

		case SETTING_AUTO_OFF:
			sh->auto_off = (sh->auto_off + (dir > 0 ? 1 : AUTO_OFF_COUNT - 1)) % AUTO_OFF_COUNT;
			break;

		default:
			if (dir > 0)
				sh->decimals = (sh->decimals >= DECIMALS_MAX) ? DECIMALS_AUTO : sh->decimals + 1;
			else
				sh->decimals = (sh->decimals <= DECIMALS_AUTO) ? DECIMALS_MAX : sh->decimals - 1;
			break;
	}

	// Written now: pulling the battery is a normal way to turn a calculator off.
	// The other two are live state rather than a preference.
	if (setting != SETTING_USB && setting != SETTING_MODULES)
		save_settings(sh);

	// Unblocking takes the row away
	if (sh->setting_sel >= setting_count(sh) && sh->setting_sel > 0)
		sh->setting_sel = setting_count(sh) - 1;
}

static void handle_settings_key(qdos_shell* sh, const qdos_key_event* ev) {
	switch (ev->key) {
		case QDOS_KEY_UP:
			if (sh->setting_sel > 0)
				sh->setting_sel--;
			break;

		case QDOS_KEY_DOWN:
			if (sh->setting_sel + 1 < setting_count(sh))
				sh->setting_sel++;
			break;

		case QDOS_KEY_ENTER:
		case QDOS_KEY_RIGHT:
			setting_step(sh, +1);
			break;

		case QDOS_KEY_LEFT:
			setting_step(sh, -1);
			break;

		case QDOS_KEY_CLEAR:
			sh->mode = sh->page_from;
			break;

		default:
			break;
	}
}

static void handle_debug_key(qdos_shell* sh, const qdos_key_event* ev) {
	const size_t held = log_held(sh);
	const size_t last = (held > (size_t)LIST_ROWS) ? held - (size_t)LIST_ROWS : 0;

	switch (ev->key) {
		case QDOS_KEY_UP:
			if (sh->log_top > 0)
				sh->log_top--;
			break;

		case QDOS_KEY_DOWN:
			if (sh->log_top < last)
				sh->log_top++;
			break;

		case QDOS_KEY_BACKSPACE:
			sh->log_count = 0;
			sh->log_top = 0;
			break;

		case QDOS_KEY_CLEAR:
			sh->mode = sh->page_from;
			break;

		default:
			break;
	}
}

static void handle_mode_key(qdos_shell* sh, const qdos_key_event* ev) {
	// Only the calculator asks which register
	if (sh->mode != QDOS_MODE_CALC)
		sh->register_wait = REGISTER_IDLE;

	if (ev->key == QDOS_KEY_LIST && sh->mode != QDOS_MODE_LIST) {
		list_open(sh);
		return;
	}

	if (ev->key == QDOS_KEY_CATALOG && sh->mode != QDOS_MODE_LIST) {
		catalog_open(sh);
		return;
	}

	if (ev->key == QDOS_KEY_POWER) {
		sh->powering_off = true;
		return;
	}

	// Wherever it is pressed from: the soft label reads back as the new setting,
	// so nothing else has to be said
	if (ev->key == QDOS_KEY_ANGLE) {
		qdos_math_set_degrees(!qdos_math_degrees());
		save_settings(sh);
		return;
	}

	if (ev->key == QDOS_KEY_SETTINGS && sh->mode != QDOS_MODE_SETTINGS) {
		sh->page_from = (sh->mode == QDOS_MODE_DEBUG) ? sh->page_from : sh->mode;
		sh->mode = QDOS_MODE_SETTINGS;
		set_message(sh, "", false);
		return;
	}

	if (ev->key == QDOS_KEY_DEBUG && sh->mode != QDOS_MODE_DEBUG) {
		sh->page_from = (sh->mode == QDOS_MODE_SETTINGS) ? sh->page_from : sh->mode;
		// Open at the end, where the newest lines are
		const size_t held = log_held(sh);
		sh->log_top = (held > (size_t)LIST_ROWS) ? held - (size_t)LIST_ROWS : 0;
		sh->mode = QDOS_MODE_DEBUG;
		set_message(sh, "", false);
		return;
	}

	if (ev->key == QDOS_KEY_ABOUT && sh->mode != QDOS_MODE_ABOUT) {
		sh->about_from = sh->mode;
		sh->mode = QDOS_MODE_ABOUT;
		set_message(sh, "", false);
		return;
	}

	if (sh->mode == QDOS_MODE_SETTINGS) {
		handle_settings_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_DEBUG) {
		handle_debug_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_ABOUT) {
		// Any way out will do
		if (ev->key == QDOS_KEY_CLEAR || ev->key == QDOS_KEY_ENTER || ev->key == QDOS_KEY_ABOUT)
			sh->mode = sh->about_from;
	} else if (sh->mode == QDOS_MODE_EDIT) {
		handle_edit_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_LIST) {
		handle_list_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_LINE) {
		handle_line_key(sh, ev);
	} else {
		handle_calc_key(sh, ev);
	}
}

static void render_edit(qdos_shell* sh, qdos_console* con) {
	size_t line, col;
	qdos_editor_where(&sh->ed, &line, &col);

	const char mod = modifier_char(sh);
	char header[QDOS_COLS + 1];
	if (mod != '\0')
		snprintf(header, sizeof(header), "%c %zu:%zu", mod, line + 1, col + 1);
	else
		snprintf(header, sizeof(header), "%zu:%zu", line + 1, col + 1);

	qdos_console_puts(con, 0, ROW_HEADER, sh->ed.name);
	qdos_console_puts_right(con, ROW_HEADER, header);
	qdos_console_rule(con, ROW_HEADER);

	// One horizontal offset for the whole pane, so columns stay aligned
	const size_t width = QDOS_COLS - 1;
	const size_t left = (col >= width) ? col - width + 1 : 0;

	for (size_t i = 0; i < EDIT_ROWS; i++) {
		size_t len = 0;
		const char* text = qdos_editor_line(&sh->ed, sh->ed_top + i, &len);
		if (text == NULL)
			break;

		const int row = ROW_CONTENT_FIRST + (int)i;
		for (size_t c = 0; left + c < len && c < width; c++) {
			const char ch = text[left + c];
			qdos_console_putc(con, (int)c, row, ch == '\t' ? ' ' : ch);
		}

		if (sh->ed_top + i == line && sh->cursor_on)
			qdos_console_invert(con, (int)(col - left), row, 1);
	}

	qdos_console_rule(con, ROW_CONTENT_LAST);
}

static void render_about(qdos_console* con) {
	qdos_console_puts(con, 0, ROW_HEADER, "ABOUT");
	qdos_console_rule(con, ROW_HEADER);

	char line[QDOS_COLS + 1];
	int row = ROW_CONTENT_FIRST;

	snprintf(line, sizeof(line), "QDOS %s", QDOS_VERSION);
	qdos_console_puts(con, 0, row++, line);

	snprintf(line, sizeof(line), "BUILD %s", QDOS_COMMIT);
	qdos_console_puts(con, 0, row++, line);

	snprintf(line, sizeof(line), "PANEL %dX%d 1-BIT", QDOS_SCREEN_W, QDOS_SCREEN_H);
	qdos_console_puts(con, 0, row++, line);

	qdos_console_rule(con, ROW_CONTENT_LAST);
}

static const char* angle_text(void) {
	return qdos_math_degrees() ? "DEG" : "RAD";
}

static void decimals_text(const qdos_shell* sh, char* out, size_t cap) {
	if (sh->decimals == DECIMALS_AUTO)
		snprintf(out, cap, "AUTO");
	else
		snprintf(out, cap, "%d", sh->decimals);
}

static void auto_off_text(const qdos_shell* sh, char* out, size_t cap) {
	const int minutes = AUTO_OFF_MINUTES[sh->auto_off];
	if (minutes == 0)
		snprintf(out, cap, "NEVER");
	else
		snprintf(out, cap, "%d MIN", minutes);
}

static void render_settings(qdos_shell* sh, qdos_console* con) {
	qdos_console_puts(con, 0, ROW_HEADER, "SETTINGS");
	qdos_console_rule(con, ROW_HEADER);

	char value[16];
	decimals_text(sh, value, sizeof(value));

	char off[16];
	auto_off_text(sh, off, sizeof(off));

	char blocked[16];
	snprintf(blocked, sizeof(blocked), "%zu BLOCKED", qdos_natives_blocked_count(&sh->natives));

	static const char* const NAMES[SETTING__COUNT] = {
			"ANGLE", "DECIMALS", "AUTO OFF", "USB", "MODULES"};
	const char* values[SETTING__COUNT] = {
			angle_text(), value, off, sh->usb_exported ? "SHARED" : "OFF", blocked};

	qdos_setting shown[SETTING__COUNT];
	const size_t count = settings_visible(sh, shown);

	for (size_t i = 0; i < count; i++) {
		const int row = ROW_CONTENT_FIRST + (int)i;
		qdos_console_puts(con, 1, row, NAMES[shown[i]]);
		qdos_console_puts_right(con, row, values[shown[i]]);
		if (i == sh->setting_sel)
			qdos_console_invert(con, 0, row, QDOS_COLS);
	}

	qdos_console_rule(con, ROW_CONTENT_LAST);
}

static void render_debug(qdos_shell* sh, qdos_console* con) {
	char header[QDOS_COLS + 1];
	snprintf(header, sizeof(header), "%zu", sh->log_count);
	qdos_console_puts(con, 0, ROW_HEADER, "DEBUG");
	qdos_console_puts_right(con, ROW_HEADER, header);
	qdos_console_rule(con, ROW_HEADER);

	const size_t held = log_held(sh);
	if (held == 0)
		qdos_console_puts(con, 1, ROW_CONTENT_FIRST, "NOTHING LOGGED");

	for (size_t i = 0; i < (size_t)LIST_ROWS; i++) {
		const char* text = log_at(sh, sh->log_top + i);
		if (text == NULL)
			break;
		qdos_console_puts(con, 0, ROW_CONTENT_FIRST + (int)i, text);
	}

	qdos_console_rule(con, ROW_CONTENT_LAST);
}

static void render_list(qdos_shell* sh, qdos_console* con) {
	const size_t total = list_count(sh);

	char header[QDOS_COLS + 1];
	snprintf(header, sizeof(header), "%zu/%zu", total ? sh->list_sel + 1 : 0, total);
	qdos_console_puts(con, 0, ROW_HEADER, sh->list_all ? "CATALOG" : "APPS");
	qdos_console_puts_right(con, ROW_HEADER, header);
	qdos_console_rule(con, ROW_HEADER);

	for (size_t i = 0; i < LIST_ROWS; i++) {
		const size_t item = sh->list_top + i;
		if (item >= total)
			break;

		const int row = ROW_CONTENT_FIRST + (int)i;

		const qdos_native_entry* module = list_module(sh, item);
		if (module != NULL) {
			char shown[QDOS_PROGRAM_NAME_MAX + 3];
			snprintf(shown, sizeof(shown), "%s%s", module->name,
					qdos_native_is_app(module) ? "" : "::");
			qdos_console_puts(con, 1, row, shown);

			// What went wrong outranks where it came from
			qdos_console_puts_right(con, row,
					module->error[0] ? module->error
									 : origin_text(module->system, module->inbox, module->user));
		} else {
			qdos_console_puts(con, 1, row, list_name(sh, item));

			const qdos_program_entry* e = list_program(sh, item);
			if (e != NULL)
				qdos_console_puts_right(con, row, origin_text(e->system, e->inbox, e->user));
		}
		if (item == sh->list_sel)
			qdos_console_invert(con, 0, row, QDOS_COLS);
	}

	if (total == 0)
		qdos_console_puts(con, 1, ROW_CONTENT_FIRST, "NONE INSTALLED");

	qdos_console_rule(con, ROW_CONTENT_LAST);

}

/** @brief Whatever the shell last had to say, under the content */
static void render_message(qdos_shell* sh, qdos_console* con) {
	if (!sh->message[0])
		return;

	qdos_console_puts(con, 0, ROW_MESSAGE, sh->message);
	if (sh->message_is_error)
		qdos_console_invert(con, 0, ROW_MESSAGE, (int)strlen(sh->message));
}

/** @brief Repaint the whole display */
static void render(qdos_shell* sh) {
	qdos_console* con = &sh->con;
	qdos_console_clear(con);

	if (sh->mode == QDOS_MODE_EDIT) {
		render_edit(sh, con);
		render_message(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_LIST) {
		// The delete prompt lives here, so the list has to show messages too
		render_list(sh, con);
		render_message(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_SETTINGS) {
		// Sharing the card reports from here, so this page needs one as well
		render_settings(sh, con);
		render_message(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_DEBUG) {
		render_debug(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_ABOUT) {
		render_about(con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	const size_t depth = qd_interp_depth(sh->interp);

	// Top of stack nearest the input line; deeper than the rows hold, the top
	// one says so instead of holding a value.
	const size_t visible = (depth <= (size_t)STACK_ROWS) ? depth : (size_t)STACK_ROWS - 1;
	for (size_t i = 0; i < visible; i++) {
		qd_interp_value value;
		if (!qd_interp_peek(sh->interp, i, &value))
			continue;

		const int row = ROW_CONTENT_LAST - (int)i;

		char label[16];
		snprintf(label, sizeof(label), "%zu:", i + 1);
		const int used = qdos_console_puts(con, 0, row, label);

		char shown[QD_INTERP_VALUE_TEXT_MAX];
		format_value(sh, &value, shown, sizeof(shown), (size_t)(QDOS_COLS - used - 1));
		qdos_console_puts_right_within(con, row, used + 1, shown);
	}

	// Its own row: sharing one with a value made the entry look like the marker
	if (depth > visible) {
		char hidden[QDOS_COLS + 1];
		snprintf(hidden, sizeof(hidden), "%zu MORE", depth - visible);
		qdos_console_puts(con, 0, ROW_STACK_FIRST, hidden);
	}

	qdos_console_rule(con, ROW_CONTENT_LAST);
	render_soft(sh, con);

	// The message and the input line are the same row, so only one is on it.
	// What was typed is still there underneath, and the next key brings it back.
	if (sh->message[0]) {
		render_message(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	const bool line_mode = (sh->mode == QDOS_MODE_LINE);
	const char* prompt = line_mode ? LINE_PROMPT : PROMPT;
	const char* text = line_mode ? sh->input : sh->entry;
	size_t len = line_mode ? sh->input_len : sh->entry_len;

	if (line_mode) {
		const char* newline = strrchr(sh->input, '\n');
		if (newline != NULL) {
			prompt = CONT_PROMPT;
			text = newline + 1;
			len = sh->input_len - (size_t)(newline + 1 - sh->input);
		}
	}

	// After the prompt, with a space of its own: without one it reads as the
	// first letter of what is being typed
	const char mod = modifier_char(sh);
	char shown_prompt[PROMPT_LEN + 2] = {prompt[0], prompt[1], '\0', '\0'};
	int prompt_len = PROMPT_LEN;
	if (mod != '\0') {
		shown_prompt[1] = mod;
		shown_prompt[2] = ' ';
		prompt_len = PROMPT_LEN + 1;
	}

	qdos_console_puts(con, 0, ROW_INPUT, shown_prompt);

	const int room = QDOS_COLS - prompt_len - 1; // reserve a cell for the cursor

	// Scroll so the cursor stays on screen, rather than always showing the tail
	size_t caret = line_mode ? sh->input_cursor - (size_t)(text - sh->input) : len;
	if (caret > len)
		caret = len;
	const size_t start = (caret > (size_t)room) ? caret - (size_t)room : 0;

	qdos_console_puts(con, prompt_len, ROW_INPUT, text + start);
	if (sh->cursor_on)
		qdos_console_invert(con, prompt_len + (int)(caret - start), ROW_INPUT, 1);

	sh->hal->present(sh->hal, con->fb);
}

/* ------------------------------------------------------------------------ */
/* Native words                                                             */
/*                                                                          */
/* The machine's own capabilities, exposed as words in the language.         */
/* ------------------------------------------------------------------------ */

/** Take the top of stack as a storable value. */
static bool pop_value(qd_context* ctx, qdos_value* out) {
	const qd_stack* st = ctx->st;
	if (st->size == 0) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	switch (st->data[st->size - 1].type) {
		case QD_STACK_TYPE_FLOAT:
			out->type = QDOS_VALUE_FLOAT;
			return qd_pop_f(ctx, &out->f) == 0;
		case QD_STACK_TYPE_INT:
			out->type = QDOS_VALUE_INT;
			return qd_pop_i(ctx, &out->i) == 0;
		case QD_STACK_TYPE_STR:
			out->type = QDOS_VALUE_STRING;
			return qd_pop_s(ctx, out->s, sizeof(out->s)) == 0;
		default:
			return false;
	}
}

/** Push a stored value back. */
static int push_value(qd_context* ctx, const qdos_value* value) {
	switch (value->type) {
		case QDOS_VALUE_INT: return qd_push_i(ctx, value->i);
		case QDOS_VALUE_FLOAT: return qd_push_f(ctx, value->f);
		case QDOS_VALUE_STRING: return qd_push_s(ctx, value->s);
		case QDOS_VALUE_EMPTY: break;
	}
	qd_set_error_msg(ctx, "REGISTER IS EMPTY");
	return 1;
}

/** Pop a register number and name its storage entry. */
static bool pop_register_key(qd_context* ctx, const char* word, char* key, size_t cap) {
	int64_t slot = 0;
	if (qd_pop_i(ctx, &slot) != 0) {
		qd_set_error_msg(ctx, "REGISTER MUST BE A NUMBER");
		return false;
	}
	if (!qdos_register_key(slot, key, cap)) {
		char message[64];
		snprintf(message, sizeof(message), "%s: REGISTER 0 TO %d", word, QDOS_REGISTER_MAX);
		qd_set_error_msg(ctx, message);
		return false;
	}
	return true;
}

/** `sto` - (value slot -- ) store a value in a numbered register */
static int native_sto(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char key[32];
	if (!pop_register_key(ctx, "sto", key, sizeof(key))) {
		return 1;
	}

	qdos_value value;
	if (!pop_value(ctx, &value)) {
		qd_set_error_msg(ctx, "sto: NOTHING TO STORE");
		return 1;
	}

	if (qdos_storage_save(sh->hal, key, &value) != QDOS_STORE_OK) {
		qd_set_error_msg(ctx, "sto: STORAGE WRITE FAILED");
		return 1;
	}
	return 0;
}

/** `rcl` - (slot -- value) recall a numbered register */
static int native_rcl(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char key[32];
	if (!pop_register_key(ctx, "rcl", key, sizeof(key))) {
		return 1;
	}

	qdos_value value;
	const qdos_store_result result = qdos_storage_load(sh->hal, key, &value);
	if (result == QDOS_STORE_NOT_FOUND) {
		qd_set_error_msg(ctx, "rcl: REGISTER IS EMPTY");
		return 1;
	}
	if (result != QDOS_STORE_OK) {
		qd_set_error_msg(ctx, "rcl: STORAGE READ FAILED");
		return 1;
	}
	return push_value(ctx, &value);
}

/** `clr` - (slot -- ) empty a numbered register */
static int native_clr(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char key[32];
	if (!pop_register_key(ctx, "clr", key, sizeof(key))) {
		return 1;
	}

	if (qdos_storage_erase(sh->hal, key) != QDOS_STORE_OK) {
		qd_set_error_msg(ctx, "clr: STORAGE WRITE FAILED");
		return 1;
	}
	return 0;
}

/** `forget` - (name -- ) remove a Quadrate-defined word */
static int native_forget(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char name[QDOS_VALUE_STRING_MAX];
	if (qd_pop_s(ctx, name, sizeof(name)) != 0) {
		qd_set_error_msg(ctx, "forget: NEED A STRING");
		return 1;
	}

	char message[80];

	// A read-only program cannot be removed, only overridden -- but a word
	// written at the prompt covering one is yours, and dropping it is what
	// brings the shipped version back
	if (!declared_here(sh, name) && qdos_program_is_readonly(sh->hal, name)
			&& !qdos_program_is_user(sh->hal, name)) {
		const bool card = qdos_program_is_inbox(sh->hal, name);
		snprintf(message, sizeof(message), "'%.10s' IS %s", name, card ? "ON THE CARD" : "BUILT IN");
		qd_set_error_msg(ctx, message);
		return 1;
	}

	if (!qd_interp_undeclare(sh->interp, name)) {
		snprintf(message, sizeof(message), "'%.12s' IS NOT DECLARED", name);
		qd_set_error_msg(ctx, message);
		return 1;
	}

	forget_here(sh, name);
	qdos_program_erase(sh->hal, name);

	// Forgetting an override brings back whatever it was covering, card before
	// firmware, so the same copy wins as at startup
	char source[QDOS_PROGRAM_MAX];
	if (qdos_program_load(sh->hal, QDOS_SCOPE_INBOX, name, source, sizeof(source)) == QDOS_STORE_OK
			|| qdos_program_load(sh->hal, QDOS_SCOPE_SYSTEM, name, source, sizeof(source)) == QDOS_STORE_OK)
		qdos_guarded_eval(sh->interp, source);

	return 0;
}

/**
 * `edit` - (name -- ) load a program's source into the input line
 *
 * Flattened to one line with comments dropped: the input is a single line and
 * Quadrate does not care about the whitespace.
 */
static int native_edit(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char name[QDOS_PROGRAM_NAME_MAX];
	if (qd_pop_s(ctx, name, sizeof(name)) != 0) {
		qd_set_error_msg(ctx, "edit: NEED A STRING");
		return 1;
	}

	char source[QDOS_PROGRAM_MAX];
	const bool found = load_program_anywhere(sh, name, source, sizeof(source));

	// An unknown name starts a new program rather than being an error
	edit_open(sh, name, found ? source : NULL);
	return 0;
}

/**
 * `qdos::key` - ( -- key:i64 ch:i64 got:i64) the next keypress, if there is one
 *
 * For a program that has taken the screen. The shell is waiting inside the
 * call, so the keypad is the program's until it returns.
 */
static int native_key(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	qdos_key_event event;
	if (!sh->hal->poll_key(sh->hal, &event)) {
		qd_push_i(ctx, 0);
		qd_push_i(ctx, 0);
		return qd_push_i(ctx, 0);
	}

	qd_push_i(ctx, (int64_t)event.key);
	qd_push_i(ctx, (int64_t)event.ch);
	return qd_push_i(ctx, 1);
}

/** `qdos::running` - ( -- r:i64) false once the machine is stopping */
static int native_running(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;
	return qd_push_i(ctx, sh->hal->running(sh->hal) ? 1 : 0);
}

/** `qdos::ticks` - ( -- ms:i64) */
static int native_ticks(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;
	return qd_push_i(ctx, (int64_t)sh->hal->ticks_ms(sh->hal));
}

/** `cls` - ( -- ) clear the message line */
static int native_cls(qd_context* ctx, void* userdata) {
	(void)ctx;
	qdos_shell* sh = userdata;
	sh->message[0] = '\0';
	sh->message_is_error = false;
	return 0;
}

static void register_natives(qdos_shell* sh, qd_interp* interp) {
	qd_interp_register(interp, "sto", "(value:i64 slot:i64 -- )", native_sto, sh);
	qd_interp_register(interp, "rcl", "(slot:i64 -- value:i64)", native_rcl, sh);
	qd_interp_register(interp, "clr", "(slot:i64 -- )", native_clr, sh);
	qd_interp_register(interp, "forget", "(name:str -- )", native_forget, sh);
	qd_interp_register(interp, "edit", "(name:str -- )", native_edit, sh);
	qd_interp_register(interp, "cls", "( -- )", native_cls, sh);

	// The machine itself, for a program that has taken the screen
	qd_interp_register(interp, "qdos::key", "( -- key:i64 ch:i64 got:i64)", native_key, sh);
	qd_interp_register(interp, "qdos::running", "( -- r:i64)", native_running, sh);
	qd_interp_register(interp, "qdos::ticks", "( -- ms:i64)", native_ticks, sh);
	qdos_register_math(interp);
}

/** @brief A module is C and can fault; the shell is respawned, not resumed */
typedef struct {
	const qdos_shell* sh;
	char* out;
	size_t cap;
	bool found;
} module_walk;

static bool spot_new_module(const char* entry, void* user) {
	module_walk* walk = (module_walk*)user;

	// An app that has just arrived brings its own modules with it
	char folder[QDOS_PROGRAM_NAME_MAX];
	if (qdos_app_name(entry, folder, sizeof(folder))) {
		walk->sh->hal->store_list(
				walk->sh->hal, QDOS_SCOPE_INBOX, folder, spot_new_module, walk);
		return !walk->found;
	}

	char name[QDOS_PROGRAM_NAME_MAX];
	if (!qdos_module_name(entry, name, sizeof(name)))
		return true;
	if (qdos_natives_find(&walk->sh->natives, name) != NULL)
		return true;

	snprintf(walk->out, walk->cap, "%s", name);
	walk->found = true;
	return false;
}

static bool card_has_new_module(const qdos_shell* sh, char* out, size_t cap) {
	if (sh->hal->store_list == NULL)
		return false;

	module_walk walk = {.sh = sh, .out = out, .cap = cap, .found = false};
	sh->hal->store_list(sh->hal, QDOS_SCOPE_INBOX, NULL, spot_new_module, &walk);
	return walk.found;
}

/**
 * @brief Read the card again, something having landed on it
 *
 * A program can be declared over the top of itself. A module cannot: the
 * interpreter holds registrations inside one already open, so it is named and
 * left until a restart.
 */
static void reload_card(qdos_shell* sh) {
	const int found = qdos_programs_restore(sh->hal, QDOS_SCOPE_INBOX, sh->interp);

	// An app that has just arrived is a word the interpreter has not got yet
	register_apps(sh, sh->interp);

	// A list on screen is a snapshot, so it has to be taken again
	if (sh->mode == QDOS_MODE_LIST) {
		const size_t was = sh->list_sel;
		list_load(sh);

		const size_t count = list_count(sh);
		sh->list_sel = (was < count) ? was : (count > 0 ? count - 1 : 0);
		list_scroll_into_view(sh);
	}

	char waiting[QDOS_PROGRAM_NAME_MAX];
	char message[QDOS_COLS + 1];

	if (card_has_new_module(sh, waiting, sizeof(waiting)))
		snprintf(message, sizeof(message), "RESTART FOR '%.10s'", waiting);
	else
		snprintf(message, sizeof(message), "%d FROM THE CARD", found > 0 ? found : 0);

	set_message(sh, message, false);
}

static void save_session_before_native(void* user) {
	qdos_shell* sh = (qdos_shell*)user;
	qdos_storage_save_session(sh->hal, sh->interp);
}

qdos_shell* qdos_shell_create(qdos_hal* hal) {
	if (!hal)
		return NULL;

	qdos_shell* sh = calloc(1, sizeof(*sh));
	if (!sh)
		return NULL;

	sh->interp = qd_interp_create(STACK_SIZE);
	if (!sh->interp) {
		free(sh);
		return NULL;
	}

	sh->hal = hal;
	sh->decimals = DECIMALS_AUTO; // calloc would otherwise mean nought decimals
	sh->cursor_on = true;		  // nor a cursor that starts out invisible
	sh->auto_off = AUTO_OFF_DEFAULT;
	restore_settings(sh); // over the defaults just set, where anything was saved
	register_natives(sh, sh->interp);
	qdos_console_init(&sh->con);

	// Every scope is opened before any is registered: a module replaced by a
	// nearer one is closed, and its words would dangle
	qdos_natives_bind(hal, sh->con.fb);
	const bool faulted = qdos_natives_recover(&sh->natives, hal);
	for (int scope = 0; scope < QDOS_SCOPE__COUNT; scope++)
		qdos_natives_load(&sh->natives, hal, (qdos_store_scope)scope);
	qdos_natives_register(&sh->natives, sh->interp);
	qdos_natives_on_call(save_session_before_native, sh);

	// In scope order, so each one shadows the one before it
	qdos_programs_restore(hal, QDOS_SCOPE_SYSTEM, sh->interp);
	qdos_programs_restore(hal, QDOS_SCOPE_INBOX, sh->interp);
	qdos_programs_restore(hal, QDOS_SCOPE_USER, sh->interp);
	register_apps(sh, sh->interp);

	// A calculator that is ready says so by being on screen, so a clean boot
	// leaves the message line empty. Only a restored stack is worth a word,
	// because the numbers above the prompt would otherwise be unexplained.
	const qdos_store_result restored = qdos_storage_restore_session(hal, sh->interp);
	if (restored == QDOS_STORE_OK && qd_interp_depth(sh->interp) > 0)
		set_message(sh, "SESSION RESTORED", false);

	if (faulted) {
		char message[QDOS_COLS + 1];
		snprintf(message, sizeof(message), "'%.12s' FAULTED", sh->natives.faulted);
		set_message(sh, message, true);
	}

	return sh;
}

void qdos_shell_destroy(qdos_shell* sh) {
	if (!sh)
		return;

	// Interpreter first: its registrations point into the modules
	qd_interp_destroy(sh->interp);
	qdos_natives_unload(&sh->natives);
	free(sh);
}

void qdos_shell_run(qdos_shell* sh) {
	if (!sh)
		return;

	render(sh);

	uint32_t last_key = sh->hal->ticks_ms(sh->hal);
	uint32_t last_blink = last_key;

	while (sh->hal->running(sh->hal)) {
		qdos_key_event ev;
		bool dirty = false;

		while (sh->hal->poll_key(sh->hal, &ev)) {
			handle_key(sh, &ev);
			dirty = true;

			// Checked after handling rather than on the key, so however the
			// press arrives it is the handler that decides this is an off
			if (sh->powering_off) {
				qdos_storage_save_session(sh->hal, sh->interp);
				return;
			}
		}

		// Never while a host has the card: those blocks are not ours to read
		if (!sh->usb_exported && sh->hal->store_changed != NULL
				&& sh->hal->store_changed(sh->hal)) {
			reload_card(sh);
			dirty = true;
		}

		const uint32_t now = sh->hal->ticks_ms(sh->hal);

		if (dirty) {
			// Typing is never the moment to be showing a dark cursor, so a key
			// puts it back on and starts the period again.
			last_key = now;
			last_blink = now;
			sh->cursor_on = true;
			sh->off_warned = false;
			render(sh);
		}

		const uint32_t idle_ms = now - last_key;

		// Nobody has touched it in a while: settle to a steady cursor, which
		// still says where you are but needs no further repaints.
		const bool settled = idle_ms >= CURSOR_SETTLE_MS;

		if (settled && !sh->cursor_on) {
			sh->cursor_on = true;
			render(sh);
		} else if (!settled && has_cursor(sh) && (now - last_blink) >= CURSOR_BLINK_MS) {
			last_blink = now;
			sh->cursor_on = !sh->cursor_on;
			render(sh);
		}

		// Put down rather than paused: say so, then turn the machine off. The
		// session is saved on the way out, so it comes back as it was left.
		const uint32_t off_after = auto_off_ms(sh);
		if (off_after > 0) {
			if (idle_ms >= off_after) {
				qdos_storage_save_session(sh->hal, sh->interp);
				return;
			}

			if (!sh->off_warned && idle_ms >= off_after - AUTO_OFF_WARN_MS) {
				sh->off_warned = true;
				set_message(sh, "TURNING OFF", false);
				render(sh);
			}
		}

		// Draining the keys is how a backend learns it is being shut down, and
		// the wait below has no timer to come back on. Check before sleeping on
		// a machine that has already stopped.
		if (!sh->hal->running(sh->hal))
			break;

		// How long until something is due: the next blink, the warning, or the
		// power-off. Nothing due means waiting on the keypad and nothing else,
		// which is what a calculator sitting on a desk should be doing.
		int timeout = -1;
		if (!settled && has_cursor(sh)) {
			const uint32_t since = now - last_blink;
			timeout = (since >= CURSOR_BLINK_MS) ? 0 : (int)(CURSOR_BLINK_MS - since);
		}
		if (off_after > 0) {
			const uint32_t due = sh->off_warned ? off_after : off_after - AUTO_OFF_WARN_MS;
			const int until = (idle_ms >= due) ? 0 : (int)(due - idle_ms);
			if (timeout < 0 || until < timeout)
				timeout = until;
		}
		sh->hal->wait(sh->hal, timeout);
	}

	qdos_storage_save_session(sh->hal, sh->interp);
}
