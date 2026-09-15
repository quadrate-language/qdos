/**
 * @file shell.c
 * @brief The calculator's user-facing loop
 */

#include <qdos/shell.h>

#include <quadrate/interp/interp.h>

#include "../ui/console.h"
#include "complete.h"
#include "editor.h"
#include "mathwords.h"

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
#define ROW_MESSAGE (QDOS_ROWS - 3)
#define ROW_CONTENT_LAST (ROW_MESSAGE - 1)
#define ROW_INPUT (QDOS_ROWS - 2)

/* Last row, so the labels sit against the edge the function keys are under */
#define ROW_SOFT (QDOS_ROWS - 1)

#define SOFT_KEYS 5
#define SOFT_WIDTH (QDOS_COLS / SOFT_KEYS)

#define STACK_ROWS (ROW_CONTENT_LAST - ROW_STACK_FIRST + 1)

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
	QDOS_MODE_EDIT,	///< Editing a program in the stack area
	QDOS_MODE_ABOUT ///< What this firmware is
} qdos_mode;

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
	bool list_all; ///< Every word, rather than just the installed programs
	size_t list_sel;
	size_t list_top;
	qdos_mode list_from;
	qdos_mode about_from; ///< Mode to return to

	qdos_editor ed;
	size_t ed_top; ///< First visible line
	size_t input_len;
	size_t input_cursor;

	char message[QDOS_COLS + 1]; ///< Error or status under the stack
	bool message_is_error;
};

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

static void set_message(qdos_shell* sh, const char* text, bool is_error) {
	snprintf(sh->message, sizeof(sh->message), "%s", text ? text : "");
	sh->message_is_error = is_error;
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

/** @brief Store the source of a word the last eval declared, so it survives a reboot */
static void persist_declaration(qdos_shell* sh) {
	const char* name = qd_interp_last_declared(sh->interp);
	if (!name) {
		set_message(sh, "", false);
		return;
	}

	char message[80];
	if (qdos_program_save(sh->hal, name, sh->input) == QDOS_STORE_OK) {
		snprintf(message, sizeof(message), "SAVED '%.20s'", name);
		set_message(sh, message, false);
	} else {
		snprintf(message, sizeof(message), "'%.12s' DECLARED, NOT SAVED", name);
		set_message(sh, message, true);
	}
}

/** A soft key's label, and the key it stands for in this mode */
typedef struct {
	const char* label;
	qdos_key key;
} soft_key;

static const soft_key SOFT[5][SOFT_KEYS] = {
	[QDOS_MODE_CALC] = {{"CLR", QDOS_KEY_CLEAR}, {"APPS", QDOS_KEY_LIST}, {"CAT", QDOS_KEY_CATALOG},
			{"INFO", QDOS_KEY_ABOUT}, {"OFF", QDOS_KEY_POWER}},
	[QDOS_MODE_LINE] = {{"ESC", QDOS_KEY_CLEAR}, {"APPS", QDOS_KEY_LIST}, {"COMP", QDOS_KEY_TAB},
			{"CAT", QDOS_KEY_CATALOG}, {"", QDOS_KEY_NONE}},
	// down then up, so the pair sits like vim's j and k
	[QDOS_MODE_LIST] = {{"ESC", QDOS_KEY_CLEAR}, {"DOWN", QDOS_KEY_DOWN}, {"UP", QDOS_KEY_UP},
			{"PICK", QDOS_KEY_ENTER}, {"EDIT", QDOS_KEY_OPEN}},
	[QDOS_MODE_EDIT] = {{"DROP", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"CHECK", QDOS_KEY_CHECK},
			{"", QDOS_KEY_NONE}, {"SAVE", QDOS_KEY_SAVE}},
	[QDOS_MODE_ABOUT] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE},
			{"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE}},
};

static void render_soft(qdos_shell* sh, qdos_console* con) {
	for (int i = 0; i < SOFT_KEYS; i++) {
		const char* label = SOFT[sh->mode][i].label;
		if (label[0] == '\0')
			continue;

		// Only a program can be edited, and the catalog lists words too
		if (sh->mode == QDOS_MODE_LIST && sh->list_all && SOFT[sh->mode][i].key == QDOS_KEY_OPEN)
			continue;

		const int width = (int)strlen(label);
		qdos_console_puts(con, i * SOFT_WIDTH + (SOFT_WIDTH - width) / 2, ROW_SOFT, label);
	}
}

/** @brief Evaluate the input line and report the outcome */
static void submit(qdos_shell* sh) {
	if (sh->input_len == 0)
		return;

	// A word may take over the screen, and then the line and the message it
	// left are its business, not ours
	const qdos_mode before = sh->mode;

	if (qd_interp_eval(sh->interp, sh->input)) {
		if (sh->mode == before)
			persist_declaration(sh);
	} else {
		set_message(sh, qd_interp_error(sh->interp), true);
	}

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

	if (!qd_interp_eval(sh->interp, sh->entry)) {
		set_message(sh, qd_interp_error(sh->interp), true);
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
	if (qd_interp_eval(sh->interp, word)) {
		set_message(sh, "", false);
	} else {
		set_message(sh, qd_interp_error(sh->interp), true);
	}
}

static void enter_line_mode(qdos_shell* sh) {
	// Commit first, or the digits already typed are lost.
	entry_commit(sh);
	sh->mode = QDOS_MODE_LINE;
	input_clear(sh);
	set_message(sh, "LINE MODE - ESC TO LEAVE", false);
}

static void leave_line_mode(qdos_shell* sh) {
	sh->mode = QDOS_MODE_CALC;
	input_clear(sh);
	set_message(sh, "", false);
}

/** Keys while the keypad is a calculator. */
/* In QDOS_KEY_FN_FIRST..QDOS_KEY_FN_LAST order */
static const char* const FUNCTION_WORD[] = {
	"sin", "cos", "tan", "ln", "log10", "sqrt", "sq",
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

static void handle_calc_key(qdos_shell* sh, const qdos_key_event* ev) {
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
		case QDOS_KEY_DIV: apply_word(sh, "/"); break;
		case QDOS_KEY_DUP: apply_word(sh, "dup"); break;
		case QDOS_KEY_DROP: apply_word(sh, "drop"); break;
		case QDOS_KEY_SWAP: apply_word(sh, "swap"); break;
		case QDOS_KEY_NEG: entry_negate(sh); break;

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
				qd_interp_eval(sh->interp, "clear");
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

		case QDOS_KEY_CLEAR:
			leave_line_mode(sh);
			break;

		default:
			break;
	}
}

static void edit_open(qdos_shell* sh, const char* name, const char* source);

#define LIST_ROWS (ROW_CONTENT_LAST - ROW_CONTENT_FIRST + 1)

static size_t list_count(const qdos_shell* sh) {
	return sh->list_all ? sh->list.count : sh->app_count;
}

static const char* list_name(const qdos_shell* sh, size_t i) {
	return sh->list_all ? sh->list.name[i] : sh->apps[i].name;
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
				input_append(sh, " ");
			}
			break;

		case QDOS_KEY_OPEN: {
			if (list_count(sh) == 0 || sh->list_all)
				break;

			const char* name = list_name(sh, sh->list_sel);
			char source[QDOS_PROGRAM_MAX];
			const bool found =
					qdos_program_load(sh->hal, QDOS_SCOPE_USER, name, source, sizeof(source)) == QDOS_STORE_OK ||
					qdos_program_load(sh->hal, QDOS_SCOPE_SYSTEM, name, source, sizeof(source)) == QDOS_STORE_OK;
			if (found)
				edit_open(sh, name, source);
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
 */
static void check_program(qdos_shell* sh) {
	qd_interp* scratch = qd_interp_create(STACK_SIZE);
	if (!scratch) {
		set_message(sh, "CANNOT CHECK", true);
		return;
	}

	register_natives(sh, scratch);
	qdos_programs_restore(sh->hal, QDOS_SCOPE_SYSTEM, scratch);
	qdos_programs_restore(sh->hal, QDOS_SCOPE_USER, scratch);

	char message[80];
	const bool ok = qd_interp_eval(scratch, sh->ed.text);
	if (ok)
		snprintf(message, sizeof(message), "'%.12s' COMPILES", sh->ed.name);
	else
		snprintf(message, sizeof(message), "%s", qd_interp_error(scratch));

	qd_interp_destroy(scratch);
	set_message(sh, message, !ok);
}

static void handle_edit_key(qdos_shell* sh, const qdos_key_event* ev) {
	const char* word = function_word(ev->key);
	if (word != NULL) {
		qdos_editor_insert(&sh->ed, ' ');
		for (const char* c = word; *c; c++)
			qdos_editor_insert(&sh->ed, *c);
		qdos_editor_insert(&sh->ed, ' ');
		edit_scroll_into_view(sh);
		return;
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
			if (!qd_interp_eval(sh->interp, sh->ed.text)) {
				// Stay in the editor: the text is still the only copy
				set_message(sh, qd_interp_error(sh->interp), true);
				break;
			}
			qdos_program_save(sh->hal, sh->ed.name, sh->ed.text);
			snprintf(message, sizeof(message), "SAVED '%.12s'", sh->ed.name);
			sh->mode = QDOS_MODE_CALC;
			set_message(sh, message, false);
			break;
		}

		case QDOS_KEY_CLEAR:
			sh->mode = QDOS_MODE_CALC;
			set_message(sh, "NOT SAVED", false);
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
	qdos_key_event expanded;
	if (expand_soft(sh, ev, &expanded)) {
		handle_mode_key(sh, &expanded);
		return;
	}

	handle_mode_key(sh, ev);
}

static void handle_mode_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (ev->key == QDOS_KEY_LIST && sh->mode != QDOS_MODE_LIST) {
		list_open(sh);
		return;
	}

	if (ev->key == QDOS_KEY_CATALOG && sh->mode != QDOS_MODE_LIST) {
		catalog_open(sh);
		return;
	}

	if (ev->key == QDOS_KEY_ABOUT && sh->mode != QDOS_MODE_ABOUT) {
		sh->about_from = sh->mode;
		sh->mode = QDOS_MODE_ABOUT;
		set_message(sh, "", false);
		return;
	}

	if (sh->mode == QDOS_MODE_ABOUT) {
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

	char header[QDOS_COLS + 1];
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

		if (sh->ed_top + i == line)
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
		qdos_console_puts(con, 1, row, list_name(sh, item));
		if (!sh->list_all) {
			const qdos_program_entry* e = &sh->apps[item];
			qdos_console_puts_right(con, row, e->user ? (e->system ? "USER*" : "USER") : "SYS");
		}
		if (item == sh->list_sel)
			qdos_console_invert(con, 0, row, QDOS_COLS);
	}

	if (total == 0)
		qdos_console_puts(con, 1, ROW_CONTENT_FIRST, "NONE INSTALLED");

	qdos_console_rule(con, ROW_CONTENT_LAST);

}

/** @brief Repaint the whole display */
static void render(qdos_shell* sh) {
	qdos_console* con = &sh->con;
	qdos_console_clear(con);

	if (sh->mode == QDOS_MODE_EDIT) {
		render_edit(sh, con);
		if (sh->message[0]) {
			qdos_console_puts(con, 0, ROW_MESSAGE, sh->message);
			if (sh->message_is_error)
				qdos_console_invert(con, 0, ROW_MESSAGE, (int)strlen(sh->message));
		}
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_LIST) {
		render_list(sh, con);
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

	// Top of stack nearest the input line.
	const size_t visible = (depth < STACK_ROWS) ? depth : (size_t)STACK_ROWS;
	for (size_t i = 0; i < visible; i++) {
		qd_interp_value value;
		if (!qd_interp_peek(sh->interp, i, &value))
			continue;

		const int row = ROW_CONTENT_LAST - (int)i;

		char label[16];
		snprintf(label, sizeof(label), "%zu:", i + 1);
		qdos_console_puts(con, 0, row, label);
		qdos_console_puts_right(con, row, value.text);
	}

	if (depth > (size_t)STACK_ROWS)
		qdos_console_puts(con, 0, ROW_STACK_FIRST, "...");

	qdos_console_rule(con, ROW_CONTENT_LAST);

	if (sh->message[0]) {
		qdos_console_puts(con, 0, ROW_MESSAGE, sh->message);
		if (sh->message_is_error)
			qdos_console_invert(con, 0, ROW_MESSAGE, (int)strlen(sh->message));
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

	render_soft(sh, con);

	qdos_console_puts(con, 0, ROW_INPUT, prompt);

	const int room = QDOS_COLS - PROMPT_LEN - 1; // reserve a cell for the cursor

	// Scroll so the cursor stays on screen, rather than always showing the tail
	size_t caret = line_mode ? sh->input_cursor - (size_t)(text - sh->input) : len;
	if (caret > len)
		caret = len;
	const size_t start = (caret > (size_t)room) ? caret - (size_t)room : 0;

	qdos_console_puts(con, PROMPT_LEN, ROW_INPUT, text + start);
	qdos_console_invert(con, PROMPT_LEN + (int)(caret - start), ROW_INPUT, 1);

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

	// A shipped program cannot be removed, only overridden
	if (qdos_program_is_system(sh->hal, name) && !qdos_program_is_user(sh->hal, name)) {
		snprintf(message, sizeof(message), "'%.12s' IS BUILT IN", name);
		qd_set_error_msg(ctx, message);
		return 1;
	}

	if (!qd_interp_undeclare(sh->interp, name)) {
		snprintf(message, sizeof(message), "'%.12s' IS NOT DECLARED", name);
		qd_set_error_msg(ctx, message);
		return 1;
	}

	qdos_program_erase(sh->hal, name);

	// Forgetting an override brings the shipped version back
	char source[QDOS_PROGRAM_MAX];
	if (qdos_program_load(sh->hal, QDOS_SCOPE_SYSTEM, name, source, sizeof(source)) == QDOS_STORE_OK)
		qd_interp_eval(sh->interp, source);

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
	const bool found =
			qdos_program_load(sh->hal, QDOS_SCOPE_USER, name, source, sizeof(source)) == QDOS_STORE_OK ||
			qdos_program_load(sh->hal, QDOS_SCOPE_SYSTEM, name, source, sizeof(source)) == QDOS_STORE_OK;

	// An unknown name starts a new program rather than being an error
	edit_open(sh, name, found ? source : NULL);
	return 0;
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
	qdos_register_math(interp);
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
	register_natives(sh, sh->interp);
	qdos_console_init(&sh->con);

	// System first, so a user program of the same name shadows it
	const int sys = qdos_programs_restore(hal, QDOS_SCOPE_SYSTEM, sh->interp);
	const int programs = qdos_programs_restore(hal, QDOS_SCOPE_USER, sh->interp);
	const qdos_store_result restored = qdos_storage_restore_session(hal, sh->interp);

	char message[80];
	if (programs > 0 || sys > 0) {
		snprintf(message, sizeof(message), "READY - %d SYS %d USER",
				 sys > 0 ? sys : 0, programs > 0 ? programs : 0);
		set_message(sh, message, false);
	} else if (restored == QDOS_STORE_OK && qd_interp_depth(sh->interp) > 0) {
		set_message(sh, "SESSION RESTORED", false);
	} else {
		set_message(sh, "READY", false);
	}
	return sh;
}

void qdos_shell_destroy(qdos_shell* sh) {
	if (!sh)
		return;
	qd_interp_destroy(sh->interp);
	free(sh);
}

void qdos_shell_run(qdos_shell* sh) {
	if (!sh)
		return;

	render(sh);

	while (sh->hal->running(sh->hal)) {
		qdos_key_event ev;
		bool dirty = false;

		while (sh->hal->poll_key(sh->hal, &ev)) {
			if (ev.key == QDOS_KEY_POWER) {
				qdos_storage_save_session(sh->hal, sh->interp);
				return;
			}
			handle_key(sh, &ev);
			dirty = true;
		}

		if (dirty)
			render(sh);

		sh->hal->idle(sh->hal);
	}

	qdos_storage_save_session(sh->hal, sh->interp);
}
