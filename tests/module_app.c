/**
 * @file module_app.c
 * @brief A module that is a program rather than a library, for the tests
 *
 * Draws a mark and returns at once: there is no loop a test could get out of.
 */

#include <qdos/native.h>

#include <string.h>

/** The corner it blacks in, which a test looks for on the panel */
#define MARK 8

static int g_calls;

static int app_main(qdos_native_ctx* ctx, const qdos_native_api* api) {
	int w = 0;
	int h = 0;
	uint8_t* canvas = api->canvas(ctx, &w, &h);
	if (canvas == NULL || w <= 0 || h <= 0) {
		api->fail(ctx, "app: NO CANVAS");
		return 1;
	}

	g_calls++;

	memset(canvas, 0xFF, (size_t)w * h);
	for (int y = 0; y < MARK; y++) {
		for (int x = 0; x < MARK; x++) {
			canvas[(size_t)y * w + x] = 0x00;
		}
	}

	(void)api->ticks(ctx);
	(void)api->running(ctx);
	qdos_key key = QDOS_KEY_NONE;
	char ch = 0;
	while (api->key(ctx, &key, &ch))
		; // drain

	api->present(ctx);
	return 0;
}

/** `app::runs` - ( -- r:i64) how many times main has been entered */
static int word_runs(qdos_native_ctx* ctx, const qdos_native_api* api) {
	return api->push_int(ctx, g_calls);
}

static const qdos_native_word WORDS[] = {
		{"runs", "( -- r:i64)", word_runs},
};

/* Both halves at once: a program, and a word to ask about it */
const qdos_native_module qdos_module = {
		QDOS_NATIVE_HEADER,
		WORDS,
		sizeof(WORDS) / sizeof(*WORDS),
		NULL,
		NULL,
		app_main,
};
