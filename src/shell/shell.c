/**
 * @file shell.c
 * @brief The calculator's user-facing loop
 *
 * Reads keys from the HAL, maintains the input line, hands submitted lines to
 * the evaluator, and paints the stack. The stack is the display: this is an RPN
 * machine, so what the user needs to see at all times is what is on it.
 */

#include <qdos/shell.h>

#include <quadrate/interp/interp.h>

#include "../ui/console.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Maximum characters in the input line. */
#define INPUT_MAX 256

/** Evaluator stack capacity, in elements. */
#define STACK_SIZE 4096

/* Screen layout, in rows. */
#define ROW_HEADER 0
#define ROW_TOP_RULE 1
#define ROW_STACK_FIRST 2
#define ROW_BOTTOM_RULE (QDOS_ROWS - 4)
#define ROW_MESSAGE (QDOS_ROWS - 3)
#define ROW_INPUT (QDOS_ROWS - 1)
#define STACK_ROWS (ROW_BOTTOM_RULE - ROW_STACK_FIRST)

/** Prompt drawn at the start of the input line. */
#define PROMPT "> "
#define PROMPT_LEN 2

struct qdos_shell {
	qdos_hal* hal;	 ///< Borrowed, not owned
	qd_interp* interp; ///< Owned
	qdos_console con;

	char input[INPUT_MAX];
	size_t input_len;

	char message[QDOS_COLS + 1]; ///< Error or status under the stack
	bool message_is_error;
};

/**
 * @brief Append text to the input line, silently ignoring overflow
 *
 * Overflow is dropped rather than reported because the only way to hit it is to
 * type 256 characters into a calculator, and a diagnostic there would be noise.
 */
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

/**
 * @brief Evaluate the input line and report the outcome
 */
static void submit(qdos_shell* sh) {
	if (sh->input_len == 0)
		return;

	if (qd_interp_eval(sh->interp, sh->input)) {
		set_message(sh, "", false);
	} else {
		set_message(sh, qd_interp_error(sh->interp), true);
	}
	input_clear(sh);
}

/**
 * @brief Translate a key press into an edit or an action
 */
static void handle_key(qdos_shell* sh, const qdos_key_event* ev) {
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

		// Operator keys carry their own separators, so pressing 1 2 + gives
		// "12 +" rather than "12+" — one keypress, correctly tokenised
		case QDOS_KEY_ADD: input_append(sh, " + "); break;
		case QDOS_KEY_SUB: input_append(sh, " - "); break;
		case QDOS_KEY_MUL: input_append(sh, " * "); break;
		case QDOS_KEY_DIV: input_append(sh, " / "); break;
		case QDOS_KEY_DUP: input_append(sh, " dup "); break;
		case QDOS_KEY_DROP: input_append(sh, " drop "); break;
		case QDOS_KEY_SWAP: input_append(sh, " swap "); break;

		case QDOS_KEY_CHAR:
			if (ev->ch) {
				const char text[2] = {ev->ch, '\0'};
				input_append(sh, text);
			}
			break;

		case QDOS_KEY_ENTER: submit(sh); break;
		case QDOS_KEY_BACKSPACE: input_backspace(sh); break;

		// Clear wipes the line being typed; only when there is nothing left to
		// wipe does it clear the stack, so a mistyped entry never costs the
		// user their working values
		case QDOS_KEY_CLEAR:
			if (sh->input_len > 0) {
				input_clear(sh);
			} else {
				qd_interp_eval(sh->interp, "clear");
				set_message(sh, "stack cleared", false);
			}
			break;

		default:
			break;
	}
}

/**
 * @brief Repaint the whole display
 */
static void render(qdos_shell* sh) {
	qdos_console* con = &sh->con;
	qdos_console_clear(con);

	// Header
	char header[QDOS_COLS + 1];
	const size_t depth = qd_interp_depth(sh->interp);
	snprintf(header, sizeof(header), "QDOS");
	qdos_console_puts(con, 0, ROW_HEADER, header);

	char depth_text[24];
	snprintf(depth_text, sizeof(depth_text), "depth %zu", depth);
	qdos_console_puts_right(con, ROW_HEADER, depth_text);

	qdos_console_rule(con, ROW_TOP_RULE);

	// Stack, bottom-aligned: the top of the stack sits nearest the input line,
	// so the value about to be consumed is the one closest to what is typed
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

	// Message
	if (sh->message[0]) {
		qdos_console_puts(con, 0, ROW_MESSAGE, sh->message);
		if (sh->message_is_error)
			qdos_console_invert(con, 0, ROW_MESSAGE, (int)strlen(sh->message));
	}

	// Input line, with the tail visible when it is longer than the console
	qdos_console_puts(con, 0, ROW_INPUT, PROMPT);

	const int room = QDOS_COLS - PROMPT_LEN - 1; // reserve a cell for the cursor
	const char* shown = sh->input;
	if ((int)sh->input_len > room)
		shown = sh->input + (sh->input_len - room);

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
		snprintf(message, sizeof(message), "%s: register must be 0 to %d", word, QDOS_REGISTER_MAX);
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

	// A calculator is expected to come back holding what it held. Nothing saved
	// is a first boot, not a failure.
	const qdos_store_result restored = qdos_storage_restore_session(hal, sh->interp);
	if (restored == QDOS_STORE_OK && qd_interp_depth(sh->interp) > 0) {
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
