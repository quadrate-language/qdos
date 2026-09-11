/**
 * @file check.h
 * @brief Minimal test assertions
 *
 * Deliberately small: QDOS has no test framework dependency, and these tests
 * run under `meson test`, which already reports pass and fail.
 */

#ifndef QDOS_TESTS_CHECK_H
#define QDOS_TESTS_CHECK_H

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                      \
	do {                                                                                 \
		g_checks++;                                                                      \
		if (!(cond)) {                                                                   \
			g_failures++;                                                                \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
		}                                                                                \
	} while (0)

#define CHECK_STR(actual, expected)                                                      \
	do {                                                                                 \
		g_checks++;                                                                      \
		if (strcmp((actual), (expected)) != 0) {                                         \
			g_failures++;                                                                \
			fprintf(stderr, "FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, \
					(actual), (expected));                                               \
		}                                                                                \
	} while (0)

static inline int check_report(const char* suite) {
	if (g_failures == 0) {
		printf("%s: %d checks passed\n", suite, g_checks);
		return 0;
	}
	fprintf(stderr, "%s: %d of %d checks failed\n", suite, g_failures, g_checks);
	return 1;
}

#endif // QDOS_TESTS_CHECK_H
