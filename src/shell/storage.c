// POSIX strnlen on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

/**
 * @file storage.c
 * @brief Persisting values and sessions through the HAL
 */

#include "storage.h"

#include "guarded.h"

#include <quadrate/rt/qd_string.h>
#include <quadrate/rt/runtime.h>
#include <quadrate/rt/stack.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Bumping the version makes older firmware refuse the record.
static const uint8_t STORE_MAGIC[3] = {'Q', 'D', 'S'};
#define STORE_VERSION 1

// 3 magic + 1 version + 1 type + 3 reserved + 8 payload
#define HEADER_SIZE 16
#define PAYLOAD_OFFSET 8

/** @brief Entry holding the saved stack */
#define SESSION_KEY "session"

/** @brief How many values the last save had no encoding for; absent means none */
#define SESSION_LOST_KEY "session.lost"

/** @brief Most stack values a saved session carries */
#define SESSION_MAX 64

/** @brief Extension marking a stored entry as program source */
#define PROGRAM_SUFFIX ".qd"
#define PROGRAM_SUFFIX_LEN 3

/** @brief What a shared object is called, which is what a compiler names one */
#define MODULE_PREFIX "lib"
#define MODULE_PREFIX_LEN 3
#define MODULE_SUFFIX ".so"
#define MODULE_SUFFIX_LEN 3

static void put_u64(uint8_t* out, uint64_t value) {
	for (size_t i = 0; i < 8; i++) {
		out[i] = (uint8_t)((value >> (i * 8)) & 0xFFu);
	}
}

static uint64_t get_u64(const uint8_t* in) {
	uint64_t value = 0;
	for (size_t i = 0; i < 8; i++) {
		value |= (uint64_t)in[i] << (i * 8);
	}
	return value;
}

bool qdos_value_encode(const qdos_value* value, uint8_t* out, size_t* len) {
	if (value == NULL || out == NULL || len == NULL) {
		return false;
	}

	memset(out, 0, HEADER_SIZE);
	memcpy(out, STORE_MAGIC, sizeof(STORE_MAGIC));
	out[3] = STORE_VERSION;
	out[4] = (uint8_t)value->type;

	switch (value->type) {
	case QDOS_VALUE_EMPTY:
		*len = HEADER_SIZE;
		return true;

	case QDOS_VALUE_INT:
		put_u64(&out[PAYLOAD_OFFSET], (uint64_t)value->i);
		*len = HEADER_SIZE;
		return true;

	case QDOS_VALUE_FLOAT: {
		// Via the bit pattern, so the record does not depend on struct layout.
		uint64_t bits = 0;
		memcpy(&bits, &value->f, sizeof(bits));
		put_u64(&out[PAYLOAD_OFFSET], bits);
		*len = HEADER_SIZE;
		return true;
	}

	case QDOS_VALUE_STRING: {
		const size_t length = strnlen(value->s, QDOS_VALUE_STRING_MAX - 1);
		put_u64(&out[PAYLOAD_OFFSET], (uint64_t)length);
		memcpy(&out[HEADER_SIZE], value->s, length);
		*len = HEADER_SIZE + length;
		return true;
	}
	}
	return false;
}

bool qdos_value_decode(const uint8_t* in, size_t len, qdos_value* value) {
	if (in == NULL || value == NULL || len < HEADER_SIZE) {
		return false;
	}
	if (memcmp(in, STORE_MAGIC, sizeof(STORE_MAGIC)) != 0 || in[3] != STORE_VERSION) {
		return false;
	}

	memset(value, 0, sizeof(*value));
	const uint64_t payload = get_u64(&in[PAYLOAD_OFFSET]);

	switch (in[4]) {
	case QDOS_VALUE_EMPTY:
		value->type = QDOS_VALUE_EMPTY;
		return true;

	case QDOS_VALUE_INT:
		value->type = QDOS_VALUE_INT;
		value->i = (int64_t)payload;
		return true;

	case QDOS_VALUE_FLOAT:
		value->type = QDOS_VALUE_FLOAT;
		memcpy(&value->f, &payload, sizeof(value->f));
		return true;

	case QDOS_VALUE_STRING: {
		// A length that overruns the record means the store is damaged
		if (payload >= QDOS_VALUE_STRING_MAX || len < HEADER_SIZE + payload) {
			return false;
		}
		value->type = QDOS_VALUE_STRING;
		memcpy(value->s, &in[HEADER_SIZE], (size_t)payload);
		value->s[payload] = '\0';
		return true;
	}

	default:
		return false;
	}
}

qdos_store_result qdos_storage_save(qdos_hal* hal, const char* key, const qdos_value* value) {
	if (hal == NULL || key == NULL || value == NULL) {
		return QDOS_STORE_IO_ERROR;
	}

	uint8_t buf[QDOS_VALUE_ENCODED_MAX];
	size_t len = 0;
	if (!qdos_value_encode(value, buf, &len)) {
		return QDOS_STORE_IO_ERROR;
	}
	return hal->store_write(hal, key, buf, len);
}

qdos_store_result qdos_storage_load(qdos_hal* hal, const char* key, qdos_value* value) {
	if (hal == NULL || key == NULL || value == NULL) {
		return QDOS_STORE_IO_ERROR;
	}

	uint8_t buf[QDOS_VALUE_ENCODED_MAX];
	size_t len = 0;
	const qdos_store_result result = hal->store_read(hal, QDOS_SCOPE_USER, key, buf, sizeof(buf), &len);
	if (result != QDOS_STORE_OK) {
		return result;
	}

	// Unreadable bytes read as absent: foreign firmware cannot wedge us.
	return qdos_value_decode(buf, len, value) ? QDOS_STORE_OK : QDOS_STORE_NOT_FOUND;
}

qdos_store_result qdos_storage_erase(qdos_hal* hal, const char* key) {
	const qdos_value empty = {.type = QDOS_VALUE_EMPTY, .i = 0, .f = 0.0, .s = {0}};
	return qdos_storage_save(hal, key, &empty);
}

bool qdos_register_key(int64_t slot, char* buf, size_t cap) {
	if (slot < 0 || slot > QDOS_REGISTER_MAX) {
		return false;
	}
	const int written = snprintf(buf, cap, "reg%02d", (int)slot);
	return written > 0 && (size_t)written < cap;
}

/** Name the entry holding one value of a saved session. */
/** @brief Whether a name is safe as a filename and legal as a Quadrate word */
static bool valid_program_name(const char* name) {
	if (!name || !*name) {
		return false;
	}

	size_t n = 0;
	for (const char* c = name; *c; c++, n++) {
		const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
		if (!ok) {
			return false;
		}
	}
	return n + PROGRAM_SUFFIX_LEN < QDOS_PROGRAM_NAME_MAX;
}

bool qdos_program_key(const char* name, char* buf, size_t cap) {
	if (!valid_program_name(name)) {
		return false;
	}

	const int written = snprintf(buf, cap, "%s%s", name, PROGRAM_SUFFIX);
	return written > 0 && (size_t)written < cap;
}

bool qdos_app_key(const char* app, const char* leaf, char* buf, size_t cap) {
	if (!valid_program_name(app) || leaf == NULL || *leaf == '\0') {
		return false;
	}

	const int written = snprintf(buf, cap, "%s/%s", app, leaf);
	return written > 0 && (size_t)written < cap;
}

bool qdos_app_name(const char* entry, char* out, size_t cap) {
	if (entry == NULL) {
		return false;
	}

	const size_t len = strlen(entry);
	if (len < 2 || entry[len - 1] != QDOS_STORE_DIR_MARK || len > cap) {
		return false;
	}

	memcpy(out, entry, len - 1);
	out[len - 1] = '\0';

	// The folder names a word in the vocabulary, so it has to lex as one
	return valid_program_name(out);
}

bool qdos_app_exists(qdos_hal* hal, const char* name) {
	if (hal == NULL || hal->store_read == NULL) {
		return false;
	}

	char key[QDOS_PROGRAM_NAME_MAX * 2];
	if (!qdos_app_key(name, QDOS_APP_MAIN, key, sizeof(key))) {
		return false;
	}

	for (int scope = 0; scope < QDOS_SCOPE__COUNT; scope++) {
		char probe[1];
		size_t len = 0;
		const qdos_store_result r = hal->store_read(hal, (qdos_store_scope)scope, key, probe, sizeof(probe), &len);

		// A source of any length at all is there; TOO_BIG says so loudest
		if (r == QDOS_STORE_TOO_BIG || (r == QDOS_STORE_OK && len > 0)) {
			return true;
		}
	}
	return false;
}

bool qdos_module_name(const char* entry, char* out, size_t cap) {
	if (entry == NULL) {
		return false;
	}

	const size_t len = strlen(entry);
	if (len <= MODULE_PREFIX_LEN + MODULE_SUFFIX_LEN) {
		return false;
	}
	if (strncmp(entry, MODULE_PREFIX, MODULE_PREFIX_LEN) != 0) {
		return false;
	}
	if (strcmp(entry + len - MODULE_SUFFIX_LEN, MODULE_SUFFIX) != 0) {
		return false;
	}

	const size_t stem = len - MODULE_PREFIX_LEN - MODULE_SUFFIX_LEN;
	if (stem >= cap) {
		return false;
	}

	memcpy(out, entry + MODULE_PREFIX_LEN, stem);
	out[stem] = '\0';

	// The stem becomes a scope in the vocabulary, so it has to lex as one
	return valid_program_name(out);
}

bool qdos_module_key(const char* name, char* buf, size_t cap) {
	if (!valid_program_name(name)) {
		return false;
	}

	const int written = snprintf(buf, cap, "%s%s%s", MODULE_PREFIX, name, MODULE_SUFFIX);
	return written > 0 && (size_t)written < cap;
}

/**
 * @brief The store key holding a program's source
 *
 * An app keeps its source inside its own folder, a loose program beside
 * everything else. Which one a name is, the card decides.
 */
static bool source_key(qdos_hal* hal, const char* name, char* buf, size_t cap) {
	if (qdos_app_exists(hal, name)) {
		return qdos_app_key(name, QDOS_APP_MAIN, buf, cap);
	}

	return qdos_program_key(name, buf, cap);
}

qdos_store_result qdos_program_save(qdos_hal* hal, const char* name, const char* source) {
	char key[QDOS_PROGRAM_NAME_MAX * 2];
	if (!source_key(hal, name, key, sizeof(key)) || !source) {
		return QDOS_STORE_IO_ERROR;
	}

	const size_t len = strnlen(source, QDOS_PROGRAM_MAX);
	if (len == 0 || len == QDOS_PROGRAM_MAX) {
		return QDOS_STORE_TOO_BIG;
	}

	return hal->store_write(hal, key, source, len);
}

qdos_store_result qdos_program_load(qdos_hal* hal, qdos_store_scope scope, const char* name, char* buf, size_t cap) {
	char key[QDOS_PROGRAM_NAME_MAX * 2];
	if (!source_key(hal, name, key, sizeof(key)) || cap == 0) {
		return QDOS_STORE_IO_ERROR;
	}

	size_t len = 0;
	const qdos_store_result result = hal->store_read(hal, scope, key, buf, cap - 1, &len);
	if (result != QDOS_STORE_OK) {
		return result;
	}

	buf[len] = '\0';
	// An erased program is a zero-byte file, not a missing one.
	return len > 0 ? QDOS_STORE_OK : QDOS_STORE_NOT_FOUND;
}

qdos_store_result qdos_program_erase(qdos_hal* hal, const char* name) {
	char key[QDOS_PROGRAM_NAME_MAX * 2];
	if (!source_key(hal, name, key, sizeof(key))) {
		return QDOS_STORE_IO_ERROR;
	}

	return hal->store_write(hal, key, "", 0);
}

typedef struct {
	qdos_hal* hal;
	qdos_store_scope scope;
	qd_interp* interp;
	int declared;
} restore_walk;

static bool restore_one(const char* entry, void* user) {
	restore_walk* walk = (restore_walk*)user;

	// An app is not declared with the rest: every one of them calls its entry
	// point `main`, so they would overwrite each other. It is declared when it
	// is run, and forgotten again afterwards.
	const size_t len = strlen(entry);
	if (len > 0 && entry[len - 1] == QDOS_STORE_DIR_MARK) {
		return true;
	}

	if (len <= PROGRAM_SUFFIX_LEN || strcmp(entry + len - PROGRAM_SUFFIX_LEN, PROGRAM_SUFFIX) != 0) {
		return true;
	}

	char name[QDOS_PROGRAM_NAME_MAX];
	const size_t stem = len - PROGRAM_SUFFIX_LEN;
	if (stem >= sizeof(name)) {
		return true;
	}
	memcpy(name, entry, stem);
	name[stem] = '\0';

	char source[QDOS_PROGRAM_MAX];
	if (qdos_program_load(walk->hal, walk->scope, name, source, sizeof(source)) != QDOS_STORE_OK) {
		return true;
	}

	// A stored program that kills the runtime must not stop the shell booting
	if (qdos_guarded_eval(walk->interp, source) && qd_interp_last_declared(walk->interp)) {
		walk->declared++;
	}

	return true;
}

typedef struct {
	qdos_hal* hal;
	qdos_program_entry* out;
	size_t cap;
	size_t count;
	qdos_store_scope scope;
} gather_walk;

/** Mark an entry as found in this scope */
static void mark_origin(qdos_program_entry* e, qdos_store_scope scope) {
	switch (scope) {
	case QDOS_SCOPE_SYSTEM:
		e->system = true;
		break;
	case QDOS_SCOPE_INBOX:
		e->inbox = true;
		break;
	default:
		e->user = true;
		break;
	}
}

static bool gather_one(const char* entry, void* userdata) {
	gather_walk* w = (gather_walk*)userdata;

	char name[QDOS_PROGRAM_NAME_MAX];
	const bool app = qdos_app_name(entry, name, sizeof(name));

	if (!app) {
		const size_t len = strlen(entry);
		if (len <= PROGRAM_SUFFIX_LEN || strcmp(entry + len - PROGRAM_SUFFIX_LEN, PROGRAM_SUFFIX) != 0) {
			return true;
		}

		const size_t stem = len - PROGRAM_SUFFIX_LEN;
		if (stem >= QDOS_PROGRAM_NAME_MAX) {
			return true;
		}

		memcpy(name, entry, stem);
		name[stem] = '\0';
	}

	// Erasing writes an empty entry, there being no delete in the HAL, so a
	// dropped program is still listed unless its contents are looked at. A
	// folder with nothing to run is not an app either.
	char source[QDOS_PROGRAM_MAX];
	if (qdos_program_load(w->hal, w->scope, name, source, sizeof(source)) != QDOS_STORE_OK) {
		return true;
	}

	for (size_t i = 0; i < w->count; i++) {
		if (strcmp(w->out[i].name, name) == 0) {
			mark_origin(&w->out[i], w->scope);
			return true;
		}
	}

	if (w->count >= w->cap) {
		return false;
	}

	qdos_program_entry* e = &w->out[w->count];
	memset(e, 0, sizeof(*e));
	snprintf(e->name, QDOS_PROGRAM_NAME_MAX, "%s", name);
	e->app = app;
	mark_origin(e, w->scope);
	w->count++;
	return true;
}

/** @brief Nearest first, the same order in which one shadows another */
static int entry_rank(const qdos_program_entry* entry) {
	if (entry->user) {
		return 0;
	}
	if (entry->inbox) {
		return 1;
	}
	return 2;
}

/** @brief By scope, then name: the one you want is nearly always your own */
static int entry_order(const void* a, const void* b) {
	const qdos_program_entry* first = (const qdos_program_entry*)a;
	const qdos_program_entry* second = (const qdos_program_entry*)b;

	const int rank = entry_rank(first) - entry_rank(second);
	return (rank != 0) ? rank : strcmp(first->name, second->name);
}

size_t qdos_programs_gather(qdos_hal* hal, qdos_program_entry* out, size_t cap) {
	if (!hal->store_list || cap == 0) {
		return 0;
	}

	gather_walk walk = {.hal = hal, .out = out, .cap = cap, .count = 0, .scope = QDOS_SCOPE_SYSTEM};
	for (int scope = 0; scope < QDOS_SCOPE__COUNT; scope++) {
		walk.scope = (qdos_store_scope)scope;
		hal->store_list(hal, walk.scope, NULL, gather_one, &walk);
	}

	qsort(out, walk.count, sizeof(*out), entry_order);
	return walk.count;
}

bool qdos_program_in_scope(qdos_hal* hal, qdos_store_scope scope, const char* name) {
	char source[QDOS_PROGRAM_MAX];
	return qdos_program_load(hal, scope, name, source, sizeof(source)) == QDOS_STORE_OK;
}

bool qdos_program_is_system(qdos_hal* hal, const char* name) {
	return qdos_program_in_scope(hal, QDOS_SCOPE_SYSTEM, name);
}

bool qdos_program_is_user(qdos_hal* hal, const char* name) {
	return qdos_program_in_scope(hal, QDOS_SCOPE_USER, name);
}

bool qdos_program_is_inbox(qdos_hal* hal, const char* name) {
	return qdos_program_in_scope(hal, QDOS_SCOPE_INBOX, name);
}

bool qdos_program_is_readonly(qdos_hal* hal, const char* name) {
	return qdos_program_in_scope(hal, QDOS_SCOPE_SYSTEM, name) || qdos_program_in_scope(hal, QDOS_SCOPE_INBOX, name);
}

int qdos_programs_restore(qdos_hal* hal, qdos_store_scope scope, qd_interp* interp) {
	if (!hal->store_list) {
		return -1;
	}

	// A stored file that runs rather than declares would otherwise leave its
	// values on the stack, and do it again on every boot. Trim back to what
	// was there before rather than clearing: reloading the inbox part-way
	// through a session must not cost the user their stack.
	const size_t before = qd_interp_depth(interp);

	restore_walk walk = {.hal = hal, .scope = scope, .interp = interp, .declared = 0};
	hal->store_list(hal, scope, NULL, restore_one, &walk);

	qd_context* ctx = qd_interp_context(interp);
	while (qd_interp_depth(interp) > before) {
		qd_stack_element_t discard;
		if (qd_stack_pop(ctx->st, &discard) != QD_STACK_OK) {
			break;
		}
	}
	return walk.declared;
}

static bool session_key(size_t index, char* buf, size_t cap) {
	const int written = snprintf(buf, cap, SESSION_KEY "%02zu", index);
	return written > 0 && (size_t)written < cap;
}

/**
 * @brief One stack element as a value the store can hold
 *
 * False where there is none. A pointer is an address into this run's heap --
 * an array is the one a user can now make -- and it means nothing once the
 * machine has been off. Writing a placeholder would bring it back as a number
 * nobody entered, which is worse than not bringing it back at all.
 */
static bool session_encode(const qd_stack_element_t* from, qdos_value* value) {
	memset(value, 0, sizeof(*value));

	switch (from->type) {
	case QD_STACK_TYPE_INT:
		value->type = QDOS_VALUE_INT;
		value->i = from->value.i;
		return true;
	case QD_STACK_TYPE_FLOAT:
		value->type = QDOS_VALUE_FLOAT;
		value->f = from->value.f;
		return true;
	case QD_STACK_TYPE_STR: {
		const char* text = (from->value.s != NULL) ? qd_string_data(from->value.s) : NULL;
		value->type = QDOS_VALUE_STRING;
		snprintf(value->s, sizeof(value->s), "%s", (text != NULL) ? text : "");
		return true;
	}
	case QD_STACK_TYPE_PTR:
		return false;
	}
	return false;
}

qdos_store_result qdos_storage_save_session(qdos_hal* hal, qd_interp* interp) {
	if (hal == NULL || interp == NULL) {
		return QDOS_STORE_IO_ERROR;
	}

	const size_t depth = qd_interp_depth(interp);
	const size_t wanted = (depth < SESSION_MAX) ? depth : SESSION_MAX;

	// Raw, not qd_interp_peek: that renders strings quoted for display.
	const qd_stack* st = qd_interp_context(interp)->st;

	// Stopped at the first value with no encoding rather than skipping it:
	// what sits above one would come back at the wrong depth, and a stack read
	// off by one is not a restored session.
	size_t saved = wanted;
	for (size_t i = 0; i < wanted; i++) {
		qdos_value probe;
		if (!session_encode(&st->data[i], &probe)) {
			saved = i;
			break;
		}
	}

	qdos_value count;
	memset(&count, 0, sizeof(count));
	count.type = QDOS_VALUE_INT;
	count.i = (int64_t)saved;
	const qdos_store_result header = qdos_storage_save(hal, SESSION_KEY, &count);
	if (header != QDOS_STORE_OK) {
		return header;
	}

	// Written bottom-first so restoring pushes in the same order
	for (size_t i = 0; i < saved; i++) {
		qdos_value value;
		session_encode(&st->data[i], &value); // checked above

		char key[32];
		if (!session_key(i, key, sizeof(key))) {
			return QDOS_STORE_IO_ERROR;
		}
		const qdos_store_result result = qdos_storage_save(hal, key, &value);
		if (result != QDOS_STORE_OK) {
			return result;
		}
	}

	// Left behind, for the next start to own up to. Absent means nothing was,
	// the way an unwritten setting means its default.
	if (saved < wanted) {
		qdos_value lost;
		memset(&lost, 0, sizeof(lost));
		lost.type = QDOS_VALUE_INT;
		lost.i = (int64_t)(wanted - saved);
		qdos_storage_save(hal, SESSION_LOST_KEY, &lost);
	} else {
		qdos_storage_erase(hal, SESSION_LOST_KEY);
	}
	return QDOS_STORE_OK;
}

size_t qdos_storage_session_lost(qdos_hal* hal) {
	qdos_value value;
	if (hal == NULL || qdos_storage_load(hal, SESSION_LOST_KEY, &value) != QDOS_STORE_OK) {
		return 0;
	}
	return (value.type == QDOS_VALUE_INT && value.i > 0) ? (size_t)value.i : 0;
}

qdos_store_result qdos_storage_restore_session(qdos_hal* hal, qd_interp* interp) {
	if (hal == NULL || interp == NULL) {
		return QDOS_STORE_IO_ERROR;
	}

	qdos_value count;
	const qdos_store_result header = qdos_storage_load(hal, SESSION_KEY, &count);
	if (header != QDOS_STORE_OK) {
		return header;
	}
	if (count.type != QDOS_VALUE_INT || count.i < 0 || count.i > SESSION_MAX) {
		return QDOS_STORE_NOT_FOUND;
	}

	qd_context* ctx = qd_interp_context(interp);
	for (size_t i = 0; i < (size_t)count.i; i++) {
		char key[32];
		if (!session_key(i, key, sizeof(key))) {
			return QDOS_STORE_IO_ERROR;
		}

		qdos_value value;
		if (qdos_storage_load(hal, key, &value) != QDOS_STORE_OK) {
			return QDOS_STORE_IO_ERROR;
		}

		switch (value.type) {
		case QDOS_VALUE_INT:
			qd_push_i(ctx, value.i);
			break;
		case QDOS_VALUE_FLOAT:
			qd_push_f(ctx, value.f);
			break;
		case QDOS_VALUE_STRING:
			qd_push_s(ctx, value.s);
			break;
		case QDOS_VALUE_EMPTY:
			// A session written by firmware that stored a placeholder where a
			// pointer had been. Stop rather than push the nought it decodes to:
			// everything above it was saved at a depth that no longer holds.
			return QDOS_STORE_OK;
		}
	}
	return QDOS_STORE_OK;
}
