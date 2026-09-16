/**
 * @file native.h
 * @brief What a native module is built against
 *
 * A module is a shared object named libNAME.so, dropped in any of the stores.
 * It is a library if it offers words, which are scoped by the file -- libfoo.so
 * gives foo::bar -- and a program if it offers main(), which takes the bare
 * name. It may be both. See README.md.
 *
 * A module includes this header and libc and nothing else: everything it may
 * call arrives in a table, so it keeps working when QDOS or Quadrate moves.
 */

#ifndef QDOS_NATIVE_H
#define QDOS_NATIVE_H

#include <qdos/keys.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 'QDNM' */
#define QDOS_NATIVE_MAGIC 0x51444E4Du

/** @brief Bump whenever anything below changes shape, or a stale module reads
 * past the end of what it was built with */
#define QDOS_NATIVE_ABI 3u

/** @brief Coarse: only used to say which board a file was meant for */
#if defined(__aarch64__)
#define QDOS_NATIVE_ARCH "aarch64"
#elif defined(__arm__)
#define QDOS_NATIVE_ARCH "arm"
#elif defined(__x86_64__)
#define QDOS_NATIVE_ARCH "x86_64"
#else
#define QDOS_NATIVE_ARCH "unknown"
#endif

/** @brief The stack a word acts on; reach it through qdos_native_api */
typedef struct qdos_native_ctx qdos_native_ctx;

/** @brief Everything a module may call. Push and pop return 0 on success. */
typedef struct {
	int (*push_int)(qdos_native_ctx* ctx, int64_t value);
	int (*push_float)(qdos_native_ctx* ctx, double value);
	int (*push_str)(qdos_native_ctx* ctx, const char* value);

	int (*pop_int)(qdos_native_ctx* ctx, int64_t* out);
	int (*pop_float)(qdos_native_ctx* ctx, double* out);
	int (*pop_str)(qdos_native_ctx* ctx, char* buf, size_t cap);

	size_t (*depth)(qdos_native_ctx* ctx);

	/** @brief Why this word is giving up. Twenty-five columns are read. */
	void (*fail)(qdos_native_ctx* ctx, const char* message);

	/*
	 * The panel and the keypad, yours until the word returns. There is no
	 * graphics mode: the panel is these pixels and the shell's text is only
	 * another thing written into them.
	 */

	/** @brief One byte per pixel, row-major. Under 128 is ink. Borrowed. */
	uint8_t* (*canvas)(qdos_native_ctx* ctx, int* width, int* height);

	void (*present)(qdos_native_ctx* ctx);

	/** @brief False when no key is waiting */
	bool (*key)(qdos_native_ctx* ctx, qdos_key* key, char* ch);

	uint32_t (*ticks)(qdos_native_ctx* ctx);

	/** @brief Give the machine back; a negative timeout waits for a key */
	void (*wait)(qdos_native_ctx* ctx, int timeout_ms);

	/** @brief False when the backend is shutting down. A loop must stop. */
	bool (*running)(qdos_native_ctx* ctx);

	/** @brief Where a file on the card sits, for the data a program brings */
	bool (*path)(qdos_native_ctx* ctx, const char* name, char* buf, size_t cap);
} qdos_native_api;

/** @brief Returns 0, or non-zero having called api->fail() */
typedef int (*qdos_native_fn)(qdos_native_ctx* ctx, const qdos_native_api* api);

typedef struct {
	/** @brief Unqualified: libfoo.so's "bar" is reached as foo::bar */
	const char* name;

	/** @brief Stack effect, e.g. "(pin:i64 on:i64 -- )". The inputs are
	 * checked before the call, so a word is never entered without them. */
	const char* signature;

	qdos_native_fn fn;
} qdos_native_word;

/** @brief What a module exports, under the name QDOS_NATIVE_SYMBOL */
typedef struct {
	uint32_t magic;
	uint32_t abi;
	const char* arch;

	/** @brief The library half; NULL for a module that is only a program */
	const qdos_native_word* words;
	size_t word_count;

	/** @brief Optional. Non-zero refuses the module and nothing registers. */
	int (*open)(const qdos_native_api* api);

	/** @brief Optional */
	void (*close)(void);

	/**
	 * @brief The program half, taking the module's bare name
	 *
	 * Holds the screen and the keypad until it returns, which it must do once
	 * api->running() goes false. NULL for a library.
	 */
	int (*main)(qdos_native_ctx* ctx, const qdos_native_api* api);
} qdos_native_module;

#define QDOS_NATIVE_SYMBOL "qdos_module"

/** @brief On the declaration, so a module built with -fvisibility=hidden
 * exports this and nothing else */
#if defined(__GNUC__)
#define QDOS_NATIVE_EXPORT __attribute__((visibility("default")))
#else
#define QDOS_NATIVE_EXPORT
#endif

/** @brief Opens a descriptor: magic, ABI and architecture */
#define QDOS_NATIVE_HEADER QDOS_NATIVE_MAGIC, QDOS_NATIVE_ABI, QDOS_NATIVE_ARCH

extern QDOS_NATIVE_EXPORT const qdos_native_module qdos_module;

#ifdef __cplusplus
}
#endif

#endif // QDOS_NATIVE_H
