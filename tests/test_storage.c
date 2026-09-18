/**
 * @file test_storage.c
 * @brief Storage encoding and session persistence
 */

#include "check.h"

#include "../src/shell/storage.h"

#include <math.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------- */
/* A HAL with working storage and nothing else                            */
/* ---------------------------------------------------------------------- */

#define SLOTS 8
#define SLOT_BYTES 512

static struct {
	char name[32];
	qdos_store_scope scope;
	uint8_t data[SLOT_BYTES];
	size_t len;
	bool used;
} g_slots[SLOTS];

static void store_reset(void) {
	memset(g_slots, 0, sizeof(g_slots));
}

static qdos_store_result mem_read(
		qdos_hal* hal, qdos_store_scope scope, const char* name, void* buf, size_t cap, size_t* len) {
	(void)hal;
	for (int i = 0; i < SLOTS; i++) {
		if (g_slots[i].used && g_slots[i].scope == scope && strcmp(g_slots[i].name, name) == 0) {
			if (g_slots[i].len > cap) {
				return QDOS_STORE_TOO_BIG;
			}
			memcpy(buf, g_slots[i].data, g_slots[i].len);
			if (len) {
				*len = g_slots[i].len;
			}
			return QDOS_STORE_OK;
		}
	}
	return QDOS_STORE_NOT_FOUND;
}

static qdos_store_result mem_write(qdos_hal* hal, const char* name, const void* buf, size_t len) {
	(void)hal;
	if (len > SLOT_BYTES) {
		return QDOS_STORE_TOO_BIG;
	}

	int slot = -1;
	for (int i = 0; i < SLOTS; i++) {
		// Writes only ever land in the user store
		if (g_slots[i].used && g_slots[i].scope == QDOS_SCOPE_USER && strcmp(g_slots[i].name, name) == 0) {
			slot = i;
			break;
		}
		if (!g_slots[i].used && slot < 0) {
			slot = i;
		}
	}
	if (slot < 0) {
		return QDOS_STORE_IO_ERROR;
	}

	snprintf(g_slots[slot].name, sizeof(g_slots[slot].name), "%s", name);
	memcpy(g_slots[slot].data, buf, len);
	g_slots[slot].len = len;
	g_slots[slot].scope = QDOS_SCOPE_USER;
	g_slots[slot].used = true;
	return QDOS_STORE_OK;
}

/** A name with a '/' in it sits in a folder; see stub_list in test_shell.c */
static qdos_store_result mem_list(
		qdos_hal* hal, qdos_store_scope scope, const char* folder, qdos_store_visit visit, void* user) {
	(void)hal;
	for (int i = 0; i < SLOTS; i++) {
		if (!g_slots[i].used || g_slots[i].scope != scope) {
			continue;
		}

		const char* name = g_slots[i].name;
		const char* slash = strchr(name, '/');

		if (folder != NULL) {
			if (slash == NULL || strncmp(name, folder, (size_t)(slash - name)) != 0 || folder[slash - name] != '\0') {
				continue;
			}
			if (!visit(slash + 1, user)) {
				break;
			}
			continue;
		}

		if (slash != NULL) {
			char dir[QDOS_PROGRAM_NAME_MAX];
			snprintf(dir, sizeof(dir), "%.*s/", (int)(slash - name), name);
			if (!visit(dir, user)) {
				break;
			}
			continue;
		}

		if (!visit(name, user)) {
			break;
		}
	}
	return QDOS_STORE_OK;
}

/** Put a program in a read-only scope, which nothing is allowed to write */
static void seed_scope(qdos_store_scope scope, const char* name, const char* source) {
	for (int i = 0; i < SLOTS; i++) {
		if (g_slots[i].used) {
			continue;
		}
		snprintf(g_slots[i].name, sizeof(g_slots[i].name), "%s.qd", name);
		memcpy(g_slots[i].data, source, strlen(source));
		g_slots[i].len = strlen(source);
		g_slots[i].scope = scope;
		g_slots[i].used = true;
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

static qdos_hal make_hal(void) {
	qdos_hal hal;
	memset(&hal, 0, sizeof(hal));
	hal.store_read = mem_read;
	hal.store_write = mem_write;
	hal.store_list = mem_list;
	return hal;
}

/* ---------------------------------------------------------------------- */

static void test_encode_decode_roundtrip(void) {
	uint8_t buf[QDOS_VALUE_ENCODED_MAX];
	size_t len = 0;
	qdos_value out;

	const qdos_value ints = {.type = QDOS_VALUE_INT, .i = -9223372036854775807LL - 1};
	CHECK(qdos_value_encode(&ints, buf, &len));
	CHECK(qdos_value_decode(buf, len, &out));
	CHECK(out.type == QDOS_VALUE_INT && out.i == ints.i); // INT64_MIN survives

	const qdos_value big = {.type = QDOS_VALUE_INT, .i = 9223372036854775807LL};
	CHECK(qdos_value_encode(&big, buf, &len) && qdos_value_decode(buf, len, &out));
	CHECK(out.i == big.i);

	const qdos_value pi = {.type = QDOS_VALUE_FLOAT, .f = 3.141592653589793};
	CHECK(qdos_value_encode(&pi, buf, &len) && qdos_value_decode(buf, len, &out));
	CHECK(out.type == QDOS_VALUE_FLOAT && out.f == pi.f); // exact, not approximate

	const qdos_value tiny = {.type = QDOS_VALUE_FLOAT, .f = -2.2250738585072014e-308};
	CHECK(qdos_value_encode(&tiny, buf, &len) && qdos_value_decode(buf, len, &out));
	CHECK(out.f == tiny.f);

	qdos_value text;
	memset(&text, 0, sizeof(text));
	text.type = QDOS_VALUE_STRING;
	snprintf(text.s, sizeof(text.s), "hello, calculator");
	CHECK(qdos_value_encode(&text, buf, &len) && qdos_value_decode(buf, len, &out));
	CHECK(out.type == QDOS_VALUE_STRING && strcmp(out.s, text.s) == 0);

	const qdos_value empty = {.type = QDOS_VALUE_EMPTY};
	CHECK(qdos_value_encode(&empty, buf, &len) && qdos_value_decode(buf, len, &out));
	CHECK(out.type == QDOS_VALUE_EMPTY);
}

static void test_encoding_is_explicit(void) {
	// The layout is part of the on-flash format, so it is pinned here rather
	// than left to whatever the compiler chooses
	uint8_t buf[QDOS_VALUE_ENCODED_MAX];
	size_t len = 0;
	const qdos_value one = {.type = QDOS_VALUE_INT, .i = 1};

	CHECK(qdos_value_encode(&one, buf, &len));
	CHECK(len == 16);
	CHECK(buf[0] == 'Q' && buf[1] == 'D' && buf[2] == 'S');
	CHECK(buf[3] == 1); // version
	CHECK(buf[4] == QDOS_VALUE_INT);
	CHECK(buf[8] == 1 && buf[9] == 0); // little-endian payload
	CHECK(buf[15] == 0);
}

static void test_decode_rejects_bad_records(void) {
	uint8_t buf[QDOS_VALUE_ENCODED_MAX];
	size_t len = 0;
	qdos_value out;
	const qdos_value value = {.type = QDOS_VALUE_INT, .i = 7};
	CHECK(qdos_value_encode(&value, buf, &len));

	CHECK(!qdos_value_decode(buf, len - 1, &out)); // truncated

	uint8_t wrong[QDOS_VALUE_ENCODED_MAX];
	memcpy(wrong, buf, len);
	wrong[0] = 'X';
	CHECK(!qdos_value_decode(wrong, len, &out)); // wrong magic

	memcpy(wrong, buf, len);
	wrong[3] = 99;
	CHECK(!qdos_value_decode(wrong, len, &out)); // a version this build predates

	memcpy(wrong, buf, len);
	wrong[4] = 77;
	CHECK(!qdos_value_decode(wrong, len, &out)); // unknown type

	// A string claiming to be longer than the record must not be believed
	qdos_value text;
	memset(&text, 0, sizeof(text));
	text.type = QDOS_VALUE_STRING;
	snprintf(text.s, sizeof(text.s), "abc");
	CHECK(qdos_value_encode(&text, buf, &len));
	buf[8] = 200; // claim 200 bytes of string in a 19-byte record
	CHECK(!qdos_value_decode(buf, len, &out));

	CHECK(!qdos_value_decode(NULL, 16, &out));
	CHECK(!qdos_value_decode(buf, 16, NULL));
}

static void test_save_load_erase(void) {
	store_reset();
	qdos_hal hal = make_hal();
	qdos_value out;

	CHECK(qdos_storage_load(&hal, "reg00", &out) == QDOS_STORE_NOT_FOUND);

	const qdos_value value = {.type = QDOS_VALUE_INT, .i = 42};
	CHECK(qdos_storage_save(&hal, "reg00", &value) == QDOS_STORE_OK);
	CHECK(qdos_storage_load(&hal, "reg00", &out) == QDOS_STORE_OK);
	CHECK(out.i == 42);

	// Erase leaves the entry present but empty, since the HAL has no delete
	CHECK(qdos_storage_erase(&hal, "reg00") == QDOS_STORE_OK);
	CHECK(qdos_storage_load(&hal, "reg00", &out) == QDOS_STORE_OK);
	CHECK(out.type == QDOS_VALUE_EMPTY);
}

static void test_corrupt_store_reads_as_absent(void) {
	store_reset();
	qdos_hal hal = make_hal();

	// Bytes written by other firmware must not wedge the machine
	const uint8_t junk[20] = {'Z', 'Z', 'Z', 9, 1};
	CHECK(mem_write(&hal, "reg05", junk, sizeof(junk)) == QDOS_STORE_OK);

	qdos_value out;
	CHECK(qdos_storage_load(&hal, "reg05", &out) == QDOS_STORE_NOT_FOUND);
}

static void test_register_keys(void) {
	char key[32];
	CHECK(qdos_register_key(0, key, sizeof(key)) && strcmp(key, "reg00") == 0);
	CHECK(qdos_register_key(7, key, sizeof(key)) && strcmp(key, "reg07") == 0);
	CHECK(qdos_register_key(QDOS_REGISTER_MAX, key, sizeof(key)) && strcmp(key, "reg99") == 0);

	CHECK(!qdos_register_key(-1, key, sizeof(key)));
	CHECK(!qdos_register_key(QDOS_REGISTER_MAX + 1, key, sizeof(key)));
	CHECK(!qdos_register_key(0, key, 3)); // does not fit
}

static void test_session_survives_a_restart(void) {
	store_reset();
	qdos_hal hal = make_hal();

	// One "power cycle": build a stack, save, throw the interpreter away
	qd_interp* before = qd_interp_create(256);
	CHECK(qd_interp_eval(before, "10 20 2.5"));
	CHECK(qd_interp_depth(before) == 3);
	CHECK(qdos_storage_save_session(&hal, before) == QDOS_STORE_OK);
	qd_interp_destroy(before);

	// A fresh interpreter, as after a reboot
	qd_interp* after = qd_interp_create(256);
	CHECK(qdos_storage_restore_session(&hal, after) == QDOS_STORE_OK);
	CHECK(qd_interp_depth(after) == 3);

	qd_interp_value value;
	CHECK(qd_interp_peek(after, 0, &value));
	CHECK(value.type == QD_INTERP_VALUE_FLOAT && value.f == 2.5); // order preserved
	CHECK(qd_interp_peek(after, 2, &value));
	CHECK(value.type == QD_INTERP_VALUE_INT && value.i == 10);

	// And it is a working stack, not just restored numbers
	CHECK(qd_interp_eval(after, "drop +"));
	CHECK(qd_interp_peek(after, 0, &value) && value.i == 30);

	qd_interp_destroy(after);
}

static void test_no_session_is_not_an_error(void) {
	store_reset();
	qdos_hal hal = make_hal();

	qd_interp* interp = qd_interp_create(256);
	// A first boot: absent, and the stack is left alone
	CHECK(qdos_storage_restore_session(&hal, interp) == QDOS_STORE_NOT_FOUND);
	CHECK(qd_interp_depth(interp) == 0);
	qd_interp_destroy(interp);
}

static void test_empty_session_roundtrips(void) {
	store_reset();
	qdos_hal hal = make_hal();

	qd_interp* interp = qd_interp_create(256);
	CHECK(qdos_storage_save_session(&hal, interp) == QDOS_STORE_OK);
	CHECK(qdos_storage_restore_session(&hal, interp) == QDOS_STORE_OK);
	CHECK(qd_interp_depth(interp) == 0);
	qd_interp_destroy(interp);
}

/*
 * An array is a pointer into the heap of the run that made it, and there is no
 * encoding for one. Saving stops there: what is above it was saved at a depth
 * that will not hold, and a value that comes back as a nought nobody entered is
 * worse than one that does not come back.
 */
static void test_session_stops_at_a_value_it_cannot_encode(void) {
	store_reset();
	qdos_hal hal = make_hal();

	qd_interp* before = qd_interp_create(256);
	CHECK(qd_interp_eval(before, "10 20 [1 2 3] 30"));
	CHECK(qd_interp_depth(before) == 4);
	CHECK(qdos_storage_save_session(&hal, before) == QDOS_STORE_OK);
	qd_interp_destroy(before);

	// Two below the array kept, the array and everything above it dropped
	CHECK(qdos_storage_session_lost(&hal) == 2);

	qd_interp* after = qd_interp_create(256);
	CHECK(qdos_storage_restore_session(&hal, after) == QDOS_STORE_OK);
	CHECK(qd_interp_depth(after) == 2);

	qd_interp_value value;
	CHECK(qd_interp_peek(after, 0, &value));
	CHECK(value.type == QD_INTERP_VALUE_INT && value.i == 20);
	CHECK(qd_interp_peek(after, 1, &value));
	CHECK(value.type == QD_INTERP_VALUE_INT && value.i == 10);
	qd_interp_destroy(after);
}

/** A stack that encodes whole leaves nothing behind to report */
static void test_a_whole_session_loses_nothing(void) {
	store_reset();
	qdos_hal hal = make_hal();

	qd_interp* interp = qd_interp_create(256);
	CHECK(qd_interp_eval(interp, "1 2 3"));
	CHECK(qdos_storage_save_session(&hal, interp) == QDOS_STORE_OK);
	CHECK(qdos_storage_session_lost(&hal) == 0);
	qd_interp_destroy(interp);
}

/** And the mark is cleared once a clean stack is written over a lossy one */
static void test_saving_again_clears_the_mark(void) {
	store_reset();
	qdos_hal hal = make_hal();

	qd_interp* lossy = qd_interp_create(256);
	CHECK(qd_interp_eval(lossy, "1 [2 3]"));
	CHECK(qdos_storage_save_session(&hal, lossy) == QDOS_STORE_OK);
	CHECK(qdos_storage_session_lost(&hal) == 1);
	qd_interp_destroy(lossy);

	qd_interp* clean = qd_interp_create(256);
	CHECK(qd_interp_eval(clean, "7 8"));
	CHECK(qdos_storage_save_session(&hal, clean) == QDOS_STORE_OK);
	CHECK(qdos_storage_session_lost(&hal) == 0);
	qd_interp_destroy(clean);
}

/*
 * Firmware before this wrote a placeholder where a pointer had been, and it
 * decodes to an empty value. Restoring stops rather than pushing the nought it
 * would otherwise become.
 */
static void test_an_old_placeholder_stops_the_restore(void) {
	store_reset();
	qdos_hal hal = make_hal();

	qdos_value count = {.type = QDOS_VALUE_INT, .i = 3};
	CHECK(qdos_storage_save(&hal, "session", &count) == QDOS_STORE_OK);

	const qdos_value slots[3] = {
			{.type = QDOS_VALUE_INT, .i = 11},
			{.type = QDOS_VALUE_EMPTY},
			{.type = QDOS_VALUE_INT, .i = 33},
	};
	CHECK(qdos_storage_save(&hal, "session00", &slots[0]) == QDOS_STORE_OK);
	CHECK(qdos_storage_save(&hal, "session01", &slots[1]) == QDOS_STORE_OK);
	CHECK(qdos_storage_save(&hal, "session02", &slots[2]) == QDOS_STORE_OK);

	qd_interp* interp = qd_interp_create(256);
	CHECK(qdos_storage_restore_session(&hal, interp) == QDOS_STORE_OK);
	CHECK(qd_interp_depth(interp) == 1); // not 3, and no nought

	qd_interp_value value;
	CHECK(qd_interp_peek(interp, 0, &value));
	CHECK(value.type == QD_INTERP_VALUE_INT && value.i == 11);
	qd_interp_destroy(interp);
}

static void test_program_keys(void) {
	char key[QDOS_PROGRAM_NAME_MAX];

	CHECK(qdos_program_key("double", key, sizeof(key)) && strcmp(key, "double.qd") == 0);
	CHECK(qdos_program_key("a_1", key, sizeof(key)) && strcmp(key, "a_1.qd") == 0);

	// Nothing that could escape the store directory or name a non-word
	CHECK(!qdos_program_key("", key, sizeof(key)));
	CHECK(!qdos_program_key("../evil", key, sizeof(key)));
	CHECK(!qdos_program_key("has space", key, sizeof(key)));
	CHECK(!qdos_program_key("double", key, 4));
}

static void test_program_save_load_erase(void) {
	store_reset();
	qdos_hal hal = make_hal();

	const char* source = "fn double(x:i64 -- r:i64) { 2 * }";
	CHECK(qdos_program_save(&hal, "double", source) == QDOS_STORE_OK);

	char buf[QDOS_PROGRAM_MAX];
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_USER, "double", buf, sizeof(buf)) == QDOS_STORE_OK);
	CHECK(strcmp(buf, source) == 0);

	CHECK(qdos_program_load(&hal, QDOS_SCOPE_USER, "missing", buf, sizeof(buf)) == QDOS_STORE_NOT_FOUND);

	CHECK(qdos_program_erase(&hal, "double") == QDOS_STORE_OK);
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_USER, "double", buf, sizeof(buf)) == QDOS_STORE_NOT_FOUND);
}

static void test_programs_survive_a_restart(void) {
	store_reset();
	qdos_hal hal = make_hal();

	qd_interp* before = qd_interp_create(256);
	CHECK(qd_interp_eval(before, "fn double(x:i64 -- r:i64) { 2 * }"));
	const char* declared = qd_interp_last_declared(before);
	CHECK(declared && strcmp(declared, "double") == 0);
	CHECK(qdos_program_save(&hal, declared, "fn double(x:i64 -- r:i64) { 2 * }") == QDOS_STORE_OK);
	qd_interp_destroy(before);

	// Power cycle: a fresh interpreter knows nothing until the store is replayed
	qd_interp* after = qd_interp_create(256);
	CHECK(!qd_interp_eval(after, "21 double"));
	CHECK(qdos_programs_restore(&hal, QDOS_SCOPE_USER, after) == 1);
	CHECK(qd_interp_eval(after, "21 double"));

	qd_interp_value value;
	CHECK(qd_interp_peek(after, 0, &value) && value.i == 42);
	qd_interp_destroy(after);
}

static void test_restore_skips_junk(void) {
	store_reset();
	qdos_hal hal = make_hal();

	CHECK(qdos_program_save(&hal, "good", "fn good( -- r:i64) { 7 }") == QDOS_STORE_OK);
	CHECK(qdos_program_save(&hal, "bad", "fn bad( {{{") == QDOS_STORE_OK);

	qdos_value value = {.type = QDOS_VALUE_INT, .i = 5};
	CHECK(qdos_storage_save(&hal, "r00", &value) == QDOS_STORE_OK);

	qd_interp* interp = qd_interp_create(256);
	// One bad upload must not cost the user the rest of their programs
	CHECK(qdos_programs_restore(&hal, QDOS_SCOPE_USER, interp) == 1);
	CHECK(qd_interp_eval(interp, "good"));
	qd_interp_destroy(interp);
}

static void test_restore_without_enumeration(void) {
	store_reset();
	qdos_hal hal = make_hal();
	hal.store_list = NULL;

	qd_interp* interp = qd_interp_create(256);
	CHECK(qdos_programs_restore(&hal, QDOS_SCOPE_USER, interp) == -1);
	qd_interp_destroy(interp);
}

static void test_system_and_user_are_separate(void) {
	store_reset();
	qdos_hal hal = make_hal();
	seed_system("hyp", "fn hyp( -- r:i64) { 5 }");

	char buf[QDOS_PROGRAM_MAX];
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_SYSTEM, "hyp", buf, sizeof(buf)) == QDOS_STORE_OK);
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_USER, "hyp", buf, sizeof(buf)) == QDOS_STORE_NOT_FOUND);

	CHECK(qdos_program_is_system(&hal, "hyp"));
	CHECK(!qdos_program_is_user(&hal, "hyp"));

	// Saving writes the user store, never the system one
	CHECK(qdos_program_save(&hal, "hyp", "fn hyp( -- r:i64) { 9 }") == QDOS_STORE_OK);
	CHECK(qdos_program_is_user(&hal, "hyp"));
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_SYSTEM, "hyp", buf, sizeof(buf)) == QDOS_STORE_OK);
	CHECK(strstr(buf, "5") != NULL);
}

static void test_user_shadows_system(void) {
	store_reset();
	qdos_hal hal = make_hal();
	seed_system("thing", "fn thing( -- r:i64) { 1 }");
	CHECK(qdos_program_save(&hal, "thing", "fn thing( -- r:i64) { 2 }") == QDOS_STORE_OK);

	qd_interp* interp = qd_interp_create(256);
	CHECK(qdos_programs_restore(&hal, QDOS_SCOPE_SYSTEM, interp) == 1);
	CHECK(qdos_programs_restore(&hal, QDOS_SCOPE_USER, interp) == 1);

	qd_interp_value value;
	CHECK(qd_interp_eval(interp, "thing"));
	CHECK(qd_interp_peek(interp, 0, &value) && value.i == 2);

	qd_interp_destroy(interp);
}

static void test_erase_cannot_touch_system(void) {
	store_reset();
	qdos_hal hal = make_hal();
	seed_system("keep", "fn keep( -- r:i64) { 1 }");

	CHECK(qdos_program_erase(&hal, "keep") == QDOS_STORE_OK);

	// The erase went to the user store; the shipped one is untouched
	char buf[QDOS_PROGRAM_MAX];
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_SYSTEM, "keep", buf, sizeof(buf)) == QDOS_STORE_OK);
	CHECK(qdos_program_is_system(&hal, "keep"));
}

/* ---------------------------------------------------------------------- */
/* The inbox                                                              */
/*                                                                        */
/* Programs uploaded from a PC. Read-only like the shipped ones, so an     */
/* upload can be overridden but never overwrites what was written here.    */
/* ---------------------------------------------------------------------- */

static void test_the_inbox_is_its_own_scope(void) {
	store_reset();
	qdos_hal hal = make_hal();
	seed_inbox("hyp", "fn hyp( -- r:i64) { 5 }");

	char buf[QDOS_PROGRAM_MAX];
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_INBOX, "hyp", buf, sizeof(buf)) == QDOS_STORE_OK);
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_USER, "hyp", buf, sizeof(buf)) == QDOS_STORE_NOT_FOUND);
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_SYSTEM, "hyp", buf, sizeof(buf)) == QDOS_STORE_NOT_FOUND);

	CHECK(qdos_program_is_inbox(&hal, "hyp"));
	CHECK(!qdos_program_is_user(&hal, "hyp"));
	CHECK(!qdos_program_is_system(&hal, "hyp"));

	// Read-only, so the shell refuses to drop it
	CHECK(qdos_program_is_readonly(&hal, "hyp"));
}

/**
 * The bug this scope exists to fix: an upload used to be copied into the user
 * store at every boot, so editing it on the calculator lasted until power-off.
 */
static void test_an_upload_never_overwrites_the_users_own(void) {
	store_reset();
	qdos_hal hal = make_hal();
	seed_inbox("thing", "fn thing( -- r:i64) { 1 }");

	// The user edits it on the calculator
	CHECK(qdos_program_save(&hal, "thing", "fn thing( -- r:i64) { 2 }") == QDOS_STORE_OK);

	// Both copies are still there, each in its own scope
	char buf[QDOS_PROGRAM_MAX];
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_INBOX, "thing", buf, sizeof(buf)) == QDOS_STORE_OK);
	CHECK(strstr(buf, "1") != NULL);
	CHECK(qdos_program_load(&hal, QDOS_SCOPE_USER, "thing", buf, sizeof(buf)) == QDOS_STORE_OK);
	CHECK(strstr(buf, "2") != NULL);

	// And the user's is the one that answers, however often this is repeated
	qd_interp* interp = qd_interp_create(256);
	for (int boot = 0; boot < 3; boot++) {
		CHECK(qdos_programs_restore(&hal, QDOS_SCOPE_INBOX, interp) == 1);
		CHECK(qdos_programs_restore(&hal, QDOS_SCOPE_USER, interp) == 1);

		qd_interp_value value;
		CHECK(qd_interp_eval(interp, "thing"));
		CHECK(qd_interp_peek(interp, 0, &value) && value.i == 2);
		qd_interp_eval(interp, "clear");
	}
	qd_interp_destroy(interp);
}

/** Erasing an override falls back to the card, not past it to the firmware. */
static void test_the_card_sits_between_firmware_and_user(void) {
	store_reset();
	qdos_hal hal = make_hal();
	seed_system("thing", "fn thing( -- r:i64) { 1 }");
	seed_inbox("thing", "fn thing( -- r:i64) { 2 }");
	CHECK(qdos_program_save(&hal, "thing", "fn thing( -- r:i64) { 3 }") == QDOS_STORE_OK);

	qd_interp* interp = qd_interp_create(256);
	for (int scope = 0; scope < QDOS_SCOPE__COUNT; scope++) {
		CHECK(qdos_programs_restore(&hal, (qdos_store_scope)scope, interp) == 1);
	}

	qd_interp_value value;
	CHECK(qd_interp_eval(interp, "thing"));
	CHECK(qd_interp_peek(interp, 0, &value) && value.i == 3); // the user's own
	qd_interp_destroy(interp);
}

/** Every origin is reported, so the list can say where each one came from. */
static void test_gather_marks_all_three_origins(void) {
	store_reset();
	qdos_hal hal = make_hal();
	seed_system("shipped", "fn shipped( -- r:i64) { 1 }");
	seed_inbox("uploaded", "fn uploaded( -- r:i64) { 2 }");
	seed_inbox("both", "fn both( -- r:i64) { 3 }");
	CHECK(qdos_program_save(&hal, "both", "fn both( -- r:i64) { 4 }") == QDOS_STORE_OK);
	CHECK(qdos_program_save(&hal, "mine", "fn mine( -- r:i64) { 5 }") == QDOS_STORE_OK);

	qdos_program_entry entries[8];
	const size_t count = qdos_programs_gather(&hal, entries, 8);
	CHECK(count == 4);

	// Nearest scope first, alphabetical inside it: what was written here, then
	// what came on the card, then what shipped
	CHECK_STR(entries[0].name, "both");
	CHECK(entries[0].inbox && entries[0].user && !entries[0].system);

	CHECK_STR(entries[1].name, "mine");
	CHECK(entries[1].user && !entries[1].inbox && !entries[1].system);

	CHECK_STR(entries[2].name, "uploaded");
	CHECK(entries[2].inbox && !entries[2].user && !entries[2].system);

	CHECK_STR(entries[3].name, "shipped");
	CHECK(entries[3].system && !entries[3].inbox && !entries[3].user);
}

/**
 * Reloading the inbox mid-session must not cost the stack. At boot there is
 * nothing on it, but the USB setting reloads with the user's work in hand.
 */
static void test_restoring_keeps_what_is_on_the_stack(void) {
	store_reset();
	qdos_hal hal = make_hal();
	seed_inbox("noisy", "fn noisy( -- r:i64) { 7 }");

	qd_interp* interp = qd_interp_create(256);
	CHECK(qd_interp_eval(interp, "1 2 +"));
	CHECK(qd_interp_depth(interp) == 1);

	CHECK(qdos_programs_restore(&hal, QDOS_SCOPE_INBOX, interp) == 1);

	qd_interp_value value;
	CHECK(qd_interp_depth(interp) == 1);
	CHECK(qd_interp_peek(interp, 0, &value) && value.i == 3);

	qd_interp_destroy(interp);
}

/** A file that runs rather than declares still leaves nothing behind. */
static void test_restoring_drops_what_a_program_pushed(void) {
	store_reset();
	qdos_hal hal = make_hal();
	seed_inbox("litter", "1 2 3");

	qd_interp* interp = qd_interp_create(256);
	CHECK(qd_interp_eval(interp, "9"));

	qdos_programs_restore(&hal, QDOS_SCOPE_INBOX, interp);

	qd_interp_value value;
	CHECK(qd_interp_depth(interp) == 1); // the 9, and none of the litter
	CHECK(qd_interp_peek(interp, 0, &value) && value.i == 9);

	qd_interp_destroy(interp);
}

int main(void) {
	test_encode_decode_roundtrip();
	test_encoding_is_explicit();
	test_decode_rejects_bad_records();
	test_save_load_erase();
	test_corrupt_store_reads_as_absent();
	test_register_keys();
	test_session_survives_a_restart();
	test_no_session_is_not_an_error();
	test_empty_session_roundtrips();
	test_session_stops_at_a_value_it_cannot_encode();
	test_a_whole_session_loses_nothing();
	test_saving_again_clears_the_mark();
	test_an_old_placeholder_stops_the_restore();
	test_program_keys();
	test_program_save_load_erase();
	test_programs_survive_a_restart();
	test_restore_skips_junk();
	test_restore_without_enumeration();
	test_system_and_user_are_separate();
	test_user_shadows_system();
	test_erase_cannot_touch_system();
	test_the_inbox_is_its_own_scope();
	test_an_upload_never_overwrites_the_users_own();
	test_the_card_sits_between_firmware_and_user();
	test_gather_marks_all_three_origins();
	test_restoring_keeps_what_is_on_the_stack();
	test_restoring_drops_what_a_program_pushed();
	return check_report("storage");
}
