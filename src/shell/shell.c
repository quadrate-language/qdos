/**
 * @file shell.c
 * @brief The calculator's user-facing loop
 */

#include <qdos/shell.h>

#include <quadrate/interp/interp.h>

#include "../ui/console.h"
#include "complete.h"
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

/* Screen layout, in rows. */
#define ROW_HEADER 0
#define ROW_TOP_RULE 1
#define ROW_STACK_FIRST 2
#define ROW_BOTTOM_RULE (QDOS_ROWS - 3)
#define ROW_MESSAGE (QDOS_ROWS - 2)
#define ROW_INPUT (QDOS_ROWS - 1)

#define STACK_ROWS (ROW_BOTTOM_RULE - ROW_STACK_FIRST)

/** Prompts. The character says which mode the keypad is in. */
#define PROMPT "> "
#define LINE_PROMPT ": "
#define CONT_PROMPT ".."
#define PROMPT_LEN 2

/** @brief What the keypad is doing */
typedef enum {
	QDOS_MODE_CALC, ///< Digits build a number; an operator applies immediately
	QDOS_MODE_LINE	///< Whole lines of Quadrate, evaluated on Enter
} qdos_mode;

struct qdos_shell {
	qdos_hal* hal;	   ///< Borrowed, not owned
	qd_interp* interp; ///< Owned
	qdos_console con;

	qdos_mode mode;

	char entry[ENTRY_MAX]; ///< Number being typed, not yet on the stack
	size_t entry_len;

	char input[INPUT_MAX]; ///< Line being typed in QDOS_MODE_LINE
	size_t input_len;

	char message[QDOS_COLS + 1]; ///< Error or status under the stack
	bool message_is_error;
};

/** @brief Append text to the input line, silently ignoring overflow */
static void input_append(qdos_shell* sh, const char* text) {
	const size_t len = strlen(text);
	if (sh->input_len + len >= INPUT_MAX)
		return;
	memcpy(sh->input + sh->input_len, text, len);
	sh->input_len += len;
	sh->input[sh->input_len] = '\0';
}

static void input_backspace(qdos_shell* sh) {
	if (sh->input_len > 0)
		sh->input[--sh->input_len] = '\0';
}

static void input_clear(qdos_shell* sh) {
	sh->input_len = 0;
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
		snprintf(message, sizeof(message), "saved '%.20s'", name);
		set_message(sh, message, false);
	} else {
		snprintf(message, sizeof(message), "'%.12s' declared, not saved", name);
		set_message(sh, message, true);
	}
}

/** @brief Evaluate the input line and report the outcome */
static void submit(qdos_shell* sh) {
	if (sh->input_len == 0)
		return;

	if (qd_interp_eval(sh->interp, sh->input)) {
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
		set_message(sh, "no match", false);
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
	set_message(sh, "line mode - Esc to leave", false);
}

static void leave_line_mode(qdos_shell* sh) {
	sh->mode = QDOS_MODE_CALC;
	input_clear(sh);
	set_message(sh, "", false);
}

/** Keys while the keypad is a calculator. */
static void handle_calc_key(qdos_shell* sh, const qdos_key_event* ev) {
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
				set_message(sh, "stack cleared", false);
			}
			break;

		case QDOS_KEY_CHAR:
			if (ev->ch == ':') {
				enter_line_mode(sh);
			} else if (ev->ch != ' ') {
				set_message(sh, "press : to type a line", false);
			}
			break;

		default:
			break;
	}
}

/** Keys while whole lines of Quadrate are being typed. */
static void handle_line_key(qdos_shell* sh, const qdos_key_event* ev) {
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

		case QDOS_KEY_CLEAR:
			leave_line_mode(sh);
			break;

		default:
			break;
	}
}

static void handle_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (sh->mode == QDOS_MODE_LINE) {
		handle_line_key(sh, ev);
	} else {
		handle_calc_key(sh, ev);
	}
}

/** @brief Repaint the whole display */
static void render(qdos_shell* sh) {
	qdos_console* con = &sh->con;
	qdos_console_clear(con);

	char header[QDOS_COLS + 1];
	const size_t depth = qd_interp_depth(sh->interp);
	snprintf(header, sizeof(header), "QDOS");
	qdos_console_puts(con, 0, ROW_HEADER, header);

	char depth_text[24];
	snprintf(depth_text, sizeof(depth_text), "depth %zu", depth);
	qdos_console_puts_right(con, ROW_HEADER, depth_text);

	qdos_console_rule(con, ROW_TOP_RULE);

	// Top of stack nearest the input line.
	const size_t visible = (depth < STACK_ROWS) ? depth : (size_t)STACK_ROWS;
	for (size_t i = 0; i < visible; i++) {
		qd_interp_value value;
		if (!qd_interp_peek(sh->interp, i, &value))
			continue;

		const int row = ROW_BOTTOM_RULE - 1 - (int)i;

		char label[16];
		snprintf(label, sizeof(label), "%zu:", i + 1);
		qdos_console_puts(con, 0, row, label);
		qdos_console_puts_right(con, row, value.text);
	}

	if (depth > (size_t)STACK_ROWS)
		qdos_console_puts(con, 0, ROW_STACK_FIRST, "...");

	qdos_console_rule(con, ROW_BOTTOM_RULE);

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

	qdos_console_puts(con, 0, ROW_INPUT, prompt);

	const int room = QDOS_COLS - PROMPT_LEN - 1; // reserve a cell for the cursor
	const char* shown = text;
	if ((int)len > room)
		shown = text + (len - (size_t)room);

	const int drawn = qdos_console_puts(con, PROMPT_LEN, ROW_INPUT, shown);
	qdos_console_invert(con, PROMPT_LEN + drawn, ROW_INPUT, 1);

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
	qd_set_error_msg(ctx, "register is empty");
	return 1;
}

/** Pop a register number and name its storage entry. */
static bool pop_register_key(qd_context* ctx, const char* word, char* key, size_t cap) {
	int64_t slot = 0;
	if (qd_pop_i(ctx, &slot) != 0) {
		qd_set_error_msg(ctx, "register must be a number");
		return false;
	}
	if (!qdos_register_key(slot, key, cap)) {
		char message[64];
		snprintf(message, sizeof(message), "%s: register 0 to %d", word, QDOS_REGISTER_MAX);
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
		qd_set_error_msg(ctx, "sto: nothing to store");
		return 1;
	}

	if (qdos_storage_save(sh->hal, key, &value) != QDOS_STORE_OK) {
		qd_set_error_msg(ctx, "sto: storage write failed");
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
		qd_set_error_msg(ctx, "rcl: register is empty");
		return 1;
	}
	if (result != QDOS_STORE_OK) {
		qd_set_error_msg(ctx, "rcl: storage read failed");
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
		qd_set_error_msg(ctx, "clr: storage write failed");
		return 1;
	}
	return 0;
}

/** `forget` - (name -- ) remove a Quadrate-defined word */
static int native_forget(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char name[QDOS_VALUE_STRING_MAX];
	if (qd_pop_s(ctx, name, sizeof(name)) != 0) {
		qd_set_error_msg(ctx, "forget: need a string");
		return 1;
	}

	if (!qd_interp_undeclare(sh->interp, name)) {
		char message[80];
		snprintf(message, sizeof(message), "'%.12s' is not declared", name);
		qd_set_error_msg(ctx, message);
		return 1;
	}

	qdos_program_erase(sh->hal, name);
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

static void register_natives(qdos_shell* sh) {
	qd_interp_register(sh->interp, "sto", "(value:i64 slot:i64 -- )", native_sto, sh);
	qd_interp_register(sh->interp, "rcl", "(slot:i64 -- value:i64)", native_rcl, sh);
	qd_interp_register(sh->interp, "clr", "(slot:i64 -- )", native_clr, sh);
	qd_interp_register(sh->interp, "forget", "(name:str -- )", native_forget, sh);
	qd_interp_register(sh->interp, "cls", "( -- )", native_cls, sh);
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
	register_natives(sh);
	qdos_console_init(&sh->con);

	const int programs = qdos_programs_restore(hal, sh->interp);
	const qdos_store_result restored = qdos_storage_restore_session(hal, sh->interp);

	char message[80];
	if (programs > 0) {
		snprintf(message, sizeof(message), "ready - %d program%s", programs,
				 programs == 1 ? "" : "s");
		set_message(sh, message, false);
	} else if (restored == QDOS_STORE_OK && qd_interp_depth(sh->interp) > 0) {
		set_message(sh, "session restored", false);
	} else {
		set_message(sh, "ready", false);
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
