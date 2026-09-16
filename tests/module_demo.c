/**
 * @file module_demo.c
 * @brief A native module, built as libdemo.so for test_native
 */

#include <qdos/native.h>

static int g_opened;

/** `demo::double` - (n:i64 -- r:i64) */
static int word_double(qdos_native_ctx* ctx, const qdos_native_api* api) {
	int64_t n = 0;
	if (api->pop_int(ctx, &n) != 0) {
		api->fail(ctx, "double: NEED A NUMBER");
		return 1;
	}
	return api->push_int(ctx, n * 2);
}

/** `demo::hypot` - (a:f64 b:f64 -- r:f64), without libm to prove the point */
static int word_hypot(qdos_native_ctx* ctx, const qdos_native_api* api) {
	double b = 0.0;
	double a = 0.0;
	if (api->pop_float(ctx, &b) != 0 || api->pop_float(ctx, &a) != 0) {
		api->fail(ctx, "hypot: NEED TWO FLOATS");
		return 1;
	}

	double squared = a * a + b * b;
	double guess = squared > 1.0 ? squared : 1.0;
	for (int i = 0; i < 40; i++)
		guess = 0.5 * (guess + squared / guess);

	return api->push_float(ctx, guess);
}

/** `demo::name` - ( -- r:str) */
static int word_name(qdos_native_ctx* ctx, const qdos_native_api* api) {
	return api->push_str(ctx, g_opened ? "opened" : "cold");
}

/** `demo::sink` - (s:str -- r:i64), counting what it was handed */
static int word_sink(qdos_native_ctx* ctx, const qdos_native_api* api) {
	char text[64];
	if (api->pop_str(ctx, text, sizeof(text)) != 0) {
		api->fail(ctx, "sink: NEED A STRING");
		return 1;
	}

	int64_t n = 0;
	while (text[n] != '\0')
		n++;
	return api->push_int(ctx, n);
}

/** `demo::deep` - ( -- r:i64), how much was on the stack before the call */
static int word_deep(qdos_native_ctx* ctx, const qdos_native_api* api) {
	return api->push_int(ctx, (int64_t)api->depth(ctx));
}

/** `demo::refuse` - ( -- ), a word that always gives up */
static int word_refuse(qdos_native_ctx* ctx, const qdos_native_api* api) {
	api->fail(ctx, "refuse: AS ADVERTISED");
	return 1;
}

static const qdos_native_word WORDS[] = {
		{"double", "(n:i64 -- r:i64)", word_double},
		{"hypot", "(a:f64 b:f64 -- r:f64)", word_hypot},
		{"name", "( -- r:str)", word_name},
		{"sink", "(s:str -- r:i64)", word_sink},
		{"deep", "( -- r:i64)", word_deep},
		{"refuse", "( -- )", word_refuse},
};

static int demo_open(const qdos_native_api* api) {
	(void)api;
	g_opened = 1;
	return 0;
}

static void demo_close(void) {
	g_opened = 0;
}

const qdos_native_module qdos_module = {
		QDOS_NATIVE_HEADER,
		WORDS,
		sizeof(WORDS) / sizeof(*WORDS),
		demo_open,
		demo_close,
		NULL, // a library, not a program
};
