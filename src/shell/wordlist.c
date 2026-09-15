/**
 * @file wordlist.c
 * @brief The interpreter's vocabulary, gathered and sorted for display
 */

#include "wordlist.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool collect(const char* name, void* userdata) {
	qdos_wordlist* out = (qdos_wordlist*)userdata;
	if (out->count >= QDOS_WORDLIST_MAX)
		return false;

	if (strlen(name) >= QDOS_WORDLIST_NAME)
		return true;

	snprintf(out->name[out->count], QDOS_WORDLIST_NAME, "%s", name);
	out->count++;
	return true;
}

static int by_name(const void* a, const void* b) {
	return strcmp((const char*)a, (const char*)b);
}

void qdos_wordlist_gather(const qd_interp* interp, qdos_wordlist* out) {
	memset(out, 0, sizeof(*out));
	qd_interp_visit_words(interp, collect, out);

	qsort(out->name, out->count, QDOS_WORDLIST_NAME, by_name);

	// A declaration shadowing a builtin appears twice; keep one
	size_t kept = 0;
	for (size_t i = 0; i < out->count; i++) {
		if (kept > 0 && strcmp(out->name[kept - 1], out->name[i]) == 0)
			continue;
		if (kept != i)
			memcpy(out->name[kept], out->name[i], QDOS_WORDLIST_NAME);
		kept++;
	}
	out->count = kept;
}
