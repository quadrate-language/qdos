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
	uint8_t data[SLOT_BYTES];
	size_t len;
	bool used;
} g_slots[SLOTS];

static void store_reset(void) {
	memset(g_slots, 0, sizeof(g_slots));
}

static qdos_store_result mem_read(qdos_hal* hal, const char* name, void* buf, size_t cap, size_t* len) {
	(void)hal;
	for (int i = 0; i < SLOTS; i++) {
		if (g_slots[i].used && strcmp(g_slots[i].name, name) == 0) {
			if (g_slots[i].len > cap)
				return QDOS_STORE_TOO_BIG;
			memcpy(buf, g_slots[i].data, g_slots[i].len);
			if (len)
				*len = g_slots[i].len;
			return QDOS_STORE_OK;
		}
	}
	return QDOS_STORE_NOT_FOUND;
}

static qdos_store_result mem_write(qdos_hal* hal, const char* name, const void* buf, size_t len) {
	(void)hal;
	if (len > SLOT_BYTES)
		return QDOS_STORE_TOO_BIG;

	int slot = -1;
	for (int i = 0; i < SLOTS; i++) {
		if (g_slots[i].used && strcmp(g_slots[i].name, name) == 0) {
			slot = i;
			break;
		}
		if (!g_slots[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return QDOS_STORE_IO_ERROR;

	snprintf(g_slots[slot].name, sizeof(g_slots[slot].name), "%s", name);
	memcpy(g_slots[slot].data, buf, len);
	g_slots[slot].len = len;
	g_slots[slot].used = true;
	return QDOS_STORE_OK;
}

static qdos_store_result mem_list(qdos_hal* hal, qdos_store_visit visit, void* user) {
	(void)hal;
	for (int i = 0; i < SLOTS; i++) {
		if (g_slots[i].used && !visit(g_slots[i].name, user))
			break;
	}
	return QDOS_STORE_OK;
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
	CHECK(buf[3] == 1);						 // version
	CHECK(buf[4] == QDOS_VALUE_INT);
	CHECK(buf[8] == 1 && buf[9] == 0);		 // little-endian payload
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
	CHECK(qdos_program_load(&hal, "double", buf, sizeof(buf)) == QDOS_STORE_OK);
	CHECK(strcmp(buf, source) == 0);

	CHECK(qdos_program_load(&hal, "missing", buf, sizeof(buf)) == QDOS_STORE_NOT_FOUND);

	CHECK(qdos_program_erase(&hal, "double") == QDOS_STORE_OK);
	CHECK(qdos_program_load(&hal, "double", buf, sizeof(buf)) == QDOS_STORE_NOT_FOUND);
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
	CHECK(qdos_programs_restore(&hal, after) == 1);
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
	CHECK(qdos_programs_restore(&hal, interp) == 1);
	CHECK(qd_interp_eval(interp, "good"));
	qd_interp_destroy(interp);
}

static void test_restore_without_enumeration(void) {
	store_reset();
	qdos_hal hal = make_hal();
	hal.store_list = NULL;

	qd_interp* interp = qd_interp_create(256);
	CHECK(qdos_programs_restore(&hal, interp) == -1);
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
	test_program_keys();
	test_program_save_load_erase();
	test_programs_survive_a_restart();
	test_restore_skips_junk();
	test_restore_without_enumeration();
	return check_report("storage");
}
