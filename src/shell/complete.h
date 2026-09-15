/**
 * @file complete.h
 * @brief Completing a partly typed word against the interpreter's vocabulary
 */

#ifndef QDOS_COMPLETE_H
#define QDOS_COMPLETE_H

#include <quadrate/interp/interp.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QDOS_WORD_MAX 64
#define QDOS_COMPLETE_LISTING 120

typedef struct {
	size_t matches;
	char common[QDOS_WORD_MAX];			 ///< Longest prefix shared by every match
	char listing[QDOS_COMPLETE_LISTING]; ///< Matches, space separated, cut with '...'
} qdos_completion;

/**
 * @brief Length of the word being typed at the end of a line
 *
 * Only letters, digits and underscore count, so nothing completes after an
 * operator or a number.
 */
size_t qdos_complete_prefix_len(const char* input, size_t len);

/** @brief Match a prefix against builtins, natives and declared words */
void qdos_complete(const qd_interp* interp, const char* prefix, qdos_completion* out);

#ifdef __cplusplus
}
#endif

#endif // QDOS_COMPLETE_H
