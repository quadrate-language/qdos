/**
 * @file decimal.c
 * @brief The boundary between the decimal calculator and the words written for f64
 */

#include "decimal.h"

#include <quadrate/rt/array.h>
#include <quadrate/rt/runtime.h>
#include <quadrate/rt/stack.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool g_decimal;

void qdos_decimal_set(bool on) {
	g_decimal = on;
}

bool qdos_decimal(void) {
	return g_decimal;
}

qd_f128_t qdos_decimal_of_double(double v) {
	char text[40];
	for (int digits = 15; digits <= 17; digits++) {
		snprintf(text, sizeof(text), "%.*g", digits, v);
		if (strtod(text, NULL) == v) {
			break;
		}
	}
	qd_f128_t r;
	if (!qd_f128_parse(text, &r)) {
		r = qd_f128_from_f64(v); // inf and nan, which %g spells as f128 reads them anyway
	}
	return r;
}

bool qdos_decimal_peek(qd_context* ctx, size_t depth, qd_f128_t* out) {
	const size_t size = qd_stack_size(ctx->st);
	if (depth >= size) {
		return false;
	}
	qd_stack_element_t e;
	if (qd_stack_element(ctx->st, size - 1 - depth, &e) != QD_STACK_OK) {
		return false;
	}
	switch (e.type) {
	case QD_STACK_TYPE_F128:
		*out = e.value.q;
		return true;
	case QD_STACK_TYPE_INT:
		*out = qd_f128_from_i64(e.value.i);
		return true;
	case QD_STACK_TYPE_FLOAT:
		*out = qdos_decimal_of_double(e.value.f);
		return true;
	default:
		return false;
	}
}

// An array of f128 as a fresh array of f64, taking over the reference the slot held
static void array_to_f64(qd_stack_element_t* slot) {
	qd_array_t* from = (qd_array_t*)slot->value.p;
	if (from == NULL || !qd_array_is_valid(from) || from->elemType != QD_ARRAY_TYPE_F128) {
		return;
	}
	qd_array_t* to = qd_array_create(from->length, QD_ARRAY_TYPE_FLOAT);
	if (to == NULL) {
		return;
	}
	for (size_t i = 0; i < from->length; i++) {
		qd_f128_t q;
		qd_array_get_f128(from, i, &q);
		qd_array_push_float(to, qd_f128_to_f64(q));
	}
	slot->value.p = to;
	qd_array_release(from);
}

int qdos_decimal_call(qd_context* ctx, size_t inputs, qd_interp_native_fn fn, void* user) {
	if (!g_decimal) {
		return fn(ctx, user);
	}
	qd_stack* st = ctx->st;
	if (inputs > st->size) {
		inputs = st->size;
	}
	const size_t from = st->size - inputs;
	for (size_t k = from; k < st->size; k++) {
		qd_stack_element_t* e = &st->data[k];
		if (e->type == QD_STACK_TYPE_F128) {
			e->value.f = qd_f128_to_f64(e->value.q);
			e->type = QD_STACK_TYPE_FLOAT;
		} else if (e->type == QD_STACK_TYPE_PTR) {
			array_to_f64(e);
		}
	}

	const int result = fn(ctx, user);

	for (size_t k = from; k < st->size; k++) {
		qd_stack_element_t* e = &st->data[k];
		if (e->type == QD_STACK_TYPE_FLOAT) {
			e->value.q = qdos_decimal_of_double(e->value.f);
			e->type = QD_STACK_TYPE_F128;
		}
	}
	return result;
}

size_t qdos_signature_inputs(const char* signature) {
	const char* end = strstr(signature, "--");
	size_t count = 0;
	bool in_word = false;
	for (const char* p = signature; p != end && *p != '\0'; p++) {
		const bool word = *p != ' ' && *p != '(' && *p != ')';
		if (word && !in_word) {
			count++;
		}
		in_word = word;
	}
	return count;
}

static int through_decimal(qd_context* ctx, void* user) {
	const qdos_f64_word* word = user;
	return qdos_decimal_call(ctx, qdos_signature_inputs(word->signature), word->fn, NULL);
}

void qdos_register_f64_words(qd_interp* interp, const qdos_f64_word* words, size_t count) {
	for (size_t i = 0; i < count; i++) {
		qd_interp_register(interp, words[i].name, words[i].signature, through_decimal, (void*)&words[i]);
	}
}
