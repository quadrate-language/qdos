/**
 * @file wordlist.h
 * @brief The interpreter's vocabulary, gathered and sorted for display
 */

#ifndef QDOS_WORDLIST_H
#define QDOS_WORDLIST_H

#include <quadrate/interp/interp.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QDOS_WORDLIST_MAX 192
#define QDOS_WORDLIST_NAME 32

typedef struct {
	char name[QDOS_WORDLIST_MAX][QDOS_WORDLIST_NAME];
	size_t count;
} qdos_wordlist;

/** @brief Gather every word, sorted and without duplicates */
void qdos_wordlist_gather(const qd_interp* interp, qdos_wordlist* out);

#ifdef __cplusplus
}
#endif

#endif // QDOS_WORDLIST_H
