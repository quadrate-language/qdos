/**
 * @file module_bad.c
 * @brief Modules the loader has to turn away, one per BAD_* define
 */

#include <qdos/native.h>

/* A module with no descriptor, or one declaring no words, has no use for these */
#if !defined(NO_SYMBOL) && !defined(BAD_WORDS)
static int word_nothing(qdos_native_ctx* ctx, const qdos_native_api* api) {
	(void)ctx;
	(void)api;
	return 0;
}

static const qdos_native_word WORDS[] = {
		{"nothing", "( -- )", word_nothing},
};
#endif

#if defined(BAD_MAGIC)
#define MODULE_MAGIC 0u
#else
#define MODULE_MAGIC QDOS_NATIVE_MAGIC
#endif

#if defined(BAD_ABI)
#define MODULE_ABI (QDOS_NATIVE_ABI + 99u)
#else
#define MODULE_ABI QDOS_NATIVE_ABI
#endif

#if defined(BAD_ARCH)
#define MODULE_ARCH "pdp11"
#else
#define MODULE_ARCH QDOS_NATIVE_ARCH
#endif

#if defined(BAD_OPEN)
static int refuse_open(const qdos_native_api* api) {
	(void)api;
	return 1;
}
#define MODULE_OPEN refuse_open
#else
#define MODULE_OPEN NULL
#endif

#if defined(BAD_WORDS)
#define MODULE_WORDS NULL
#define MODULE_WORD_COUNT 0
#else
#define MODULE_WORDS WORDS
#define MODULE_WORD_COUNT (sizeof(WORDS) / sizeof(*WORDS))
#endif

#if !defined(NO_SYMBOL)
const qdos_native_module qdos_module = {
		MODULE_MAGIC,
		MODULE_ABI,
		MODULE_ARCH,
		MODULE_WORDS,
		MODULE_WORD_COUNT,
		MODULE_OPEN,
		NULL,
		NULL,
};
#endif
