// POSIX strnlen on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

/**
 * @file storage.c
 * @brief Persisting values and sessions through the HAL
 */

#include "storage.h"

#include <quadrate/rt/qd_string.h>
#include <quadrate/rt/runtime.h>
#include <quadrate/rt/stack.h>

#include <stdio.h>
#include <string.h>

// Bumping the version makes older firmware refuse the record.
static const uint8_t STORE_MAGIC[3] = {'Q', 'D', 'S'};
#define STORE_VERSION 1

// 3 magic + 1 version + 1 type + 3 reserved + 8 payload
#define HEADER_SIZE 16
#define PAYLOAD_OFFSET 8

/** @brief Entry holding the saved stack */
#define SESSION_KEY "session"

/** @brief Most stack values a saved session carries */
#define SESSION_MAX 64

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
	const qdos_store_result result = hal->store_read(hal, key, buf, sizeof(buf), &len);
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
static bool session_key(size_t index, char* buf, size_t cap) {
	const int written = snprintf(buf, cap, SESSION_KEY "%02zu", index);
	return written > 0 && (size_t)written < cap;
}

qdos_store_result qdos_storage_save_session(qdos_hal* hal, qd_interp* interp) {
	if (hal == NULL || interp == NULL) {
		return QDOS_STORE_IO_ERROR;
	}

	const size_t depth = qd_interp_depth(interp);
	const size_t saved = (depth < SESSION_MAX) ? depth : SESSION_MAX;

	qdos_value count;
	memset(&count, 0, sizeof(count));
	count.type = QDOS_VALUE_INT;
	count.i = (int64_t)saved;
	const qdos_store_result header = qdos_storage_save(hal, SESSION_KEY, &count);
	if (header != QDOS_STORE_OK) {
		return header;
	}

	// Raw, not qd_interp_peek: that renders strings quoted for display.
	const qd_stack* st = qd_interp_context(interp)->st;

	// Written bottom-first so restoring pushes in the same order
	for (size_t i = 0; i < saved; i++) {
		const qd_stack_element_t* from = &st->data[i];

		qdos_value value;
		memset(&value, 0, sizeof(value));
		switch (from->type) {
			case QD_STACK_TYPE_INT:
				value.type = QDOS_VALUE_INT;
				value.i = from->value.i;
				break;
			case QD_STACK_TYPE_FLOAT:
				value.type = QDOS_VALUE_FLOAT;
				value.f = from->value.f;
				break;
			case QD_STACK_TYPE_STR: {
				const char* text = (from->value.s != NULL) ? qd_string_data(from->value.s) : NULL;
				value.type = QDOS_VALUE_STRING;
				snprintf(value.s, sizeof(value.s), "%s", (text != NULL) ? text : "");
				break;
			}
			case QD_STACK_TYPE_PTR:
				// A pointer means nothing after a power cycle
				value.type = QDOS_VALUE_EMPTY;
				break;
		}

		char key[32];
		if (!session_key(i, key, sizeof(key))) {
			return QDOS_STORE_IO_ERROR;
		}
		const qdos_store_result result = qdos_storage_save(hal, key, &value);
		if (result != QDOS_STORE_OK) {
			return result;
		}
	}
	return QDOS_STORE_OK;
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
			case QDOS_VALUE_INT: qd_push_i(ctx, value.i); break;
			case QDOS_VALUE_FLOAT: qd_push_f(ctx, value.f); break;
			case QDOS_VALUE_STRING: qd_push_s(ctx, value.s); break;
			case QDOS_VALUE_EMPTY: qd_push_i(ctx, 0); break;
		}
	}
	return QDOS_STORE_OK;
}
