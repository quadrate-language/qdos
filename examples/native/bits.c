/**
 * @file bits.c
 * @brief A native module: the bit twiddling a calculator has no key for
 *
 * Build it for the machine:
 *
 *     ./cross/build-module.sh examples/native/bits.c
 *
 * or for the simulator, which runs whatever the host runs:
 *
 *     cc -shared -fPIC -I include examples/native/bits.c -o qdos-store/libbits.so
 *
 * Either way the file is libbits.so, and its words are bits::pop, bits::hex
 * and bits::rev. See README.md.
 */

#include <qdos/native.h>

/** `bits::pop` - (n:i64 -- r:i64) how many bits are set */
static int word_pop(qdos_native_ctx* ctx, const qdos_native_api* api) {
	int64_t n = 0;
	if (api->pop_int(ctx, &n) != 0) {
		api->fail(ctx, "pop: NEED A NUMBER");
		return 1;
	}

	uint64_t bits = (uint64_t)n;
	int64_t set = 0;
	while (bits != 0) {
		set += (int64_t)(bits & 1u);
		bits >>= 1;
	}
	return api->push_int(ctx, set);
}

/** `bits::rev` - (n:i64 -- r:i64) the 64 bits, end for end */
static int word_rev(qdos_native_ctx* ctx, const qdos_native_api* api) {
	int64_t n = 0;
	if (api->pop_int(ctx, &n) != 0) {
		api->fail(ctx, "rev: NEED A NUMBER");
		return 1;
	}

	uint64_t in = (uint64_t)n;
	uint64_t out = 0;
	for (int i = 0; i < 64; i++) {
		out = (out << 1) | (in & 1u);
		in >>= 1;
	}
	return api->push_int(ctx, (int64_t)out);
}

/**
 * `bits::hex` - (n:i64 -- s:str) written the way a byte is read
 *
 * The display shows decimal and there is no key that changes that, so a word
 * that hands back the other base is the whole reason to reach for C here.
 */
static int word_hex(qdos_native_ctx* ctx, const qdos_native_api* api) {
	int64_t n = 0;
	if (api->pop_int(ctx, &n) != 0) {
		api->fail(ctx, "hex: NEED A NUMBER");
		return 1;
	}

	static const char DIGITS[] = "0123456789ABCDEF";
	char text[19]; // "0x" and sixteen digits
	size_t at = sizeof(text) - 1;
	text[at] = '\0';

	uint64_t bits = (uint64_t)n;
	do {
		text[--at] = DIGITS[bits & 0xFu];
		bits >>= 4;
	} while (bits != 0 && at > 2);

	text[--at] = 'x';
	text[--at] = '0';
	return api->push_str(ctx, text + at);
}

static const qdos_native_word WORDS[] = {
		{"pop", "(n:i64 -- r:i64)", word_pop},
		{"rev", "(n:i64 -- r:i64)", word_rev},
		{"hex", "(n:i64 -- s:str)", word_hex},
};

const qdos_native_module qdos_module = {
		QDOS_NATIVE_HEADER,
		WORDS,
		sizeof(WORDS) / sizeof(*WORDS),
		NULL, // nothing to set up
		NULL, // nor to put away
		NULL, // a library: there is nothing here to run, only words to call
};
